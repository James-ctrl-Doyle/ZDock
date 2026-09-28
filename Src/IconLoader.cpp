#include "IconLoader.h"
#include "Log.h"

#include <format>

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <shlwapi.h>

// ⚠ 故意不 include <commctrl.h>：本机 SDK（10.0.26100.0）里它依赖 winuser.h 的 NMHDR，
// 而 windows.h 在 WIN32_LEAN_AND_MEAN 下不会把 winuser.h 带进来 → C3646。
// 我们只需要 ILD_TRANSPARENT 这一个常量，shim 里那两个参数类型也用不上（永不调用），
// 所以干脆自己定义、参数用 void*，把整个依赖摘掉。
#ifndef ILD_TRANSPARENT
#define ILD_TRANSPARENT 0x00000001
#endif

using Microsoft::WRL::ComPtr;

namespace zdock {
	namespace {

		const GUID kIID_IImageList = { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };

		// IImageList 的完整 vtable（commctrl.h 的顺序）。
		// ⚠ 方法一个都不能少、顺序不能错 —— 只调 GetIcon 也得把前面 7 个摆到位，
		// 否则 GetIcon 落在错误的槽位，直接 AccessViolation。
		struct IImageListShim : public IUnknown
		{
			virtual HRESULT STDMETHODCALLTYPE Add(HBITMAP, HBITMAP, int*) = 0;
			virtual HRESULT STDMETHODCALLTYPE ReplaceIcon(int, HICON, int*) = 0;
			virtual HRESULT STDMETHODCALLTYPE SetOverlayImage(int, int) = 0;
			virtual HRESULT STDMETHODCALLTYPE Replace(int, HBITMAP, HBITMAP) = 0;
			virtual HRESULT STDMETHODCALLTYPE AddMasked(HBITMAP, COLORREF, int*) = 0;
			virtual HRESULT STDMETHODCALLTYPE Draw(void*) = 0;
			virtual HRESULT STDMETHODCALLTYPE Remove(int) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetIcon(int, UINT, HICON*) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetImageInfo(int, void*) = 0;
			virtual HRESULT STDMETHODCALLTYPE Copy(int, IUnknown*, int, UINT) = 0;
			virtual HRESULT STDMETHODCALLTYPE Merge(int, IUnknown*, int, int, int, REFIID, void**) = 0;
			virtual HRESULT STDMETHODCALLTYPE Clone(REFIID, void**) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetImageRect(int, RECT*) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetIconSize(int*, int*) = 0;
			virtual HRESULT STDMETHODCALLTYPE SetIconSize(int, int) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetImageCount(int*) = 0;
			virtual HRESULT STDMETHODCALLTYPE SetImageCount(UINT) = 0;
			virtual HRESULT STDMETHODCALLTYPE SetBkColor(COLORREF, COLORREF*) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetBkColor(COLORREF*) = 0;
			virtual HRESULT STDMETHODCALLTYPE BeginDrag(int, int, int) = 0;
			virtual HRESULT STDMETHODCALLTYPE EndDrag() = 0;
			virtual HRESULT STDMETHODCALLTYPE DragEnter(HWND, int, int) = 0;
			virtual HRESULT STDMETHODCALLTYPE DragLeave(HWND) = 0;
			virtual HRESULT STDMETHODCALLTYPE DragMove(int, int) = 0;
			virtual HRESULT STDMETHODCALLTYPE DragShowNolock(BOOL) = 0;
			virtual HRESULT STDMETHODCALLTYPE Save(IStream*) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetItemFlags(int, DWORD*) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetOverlayImage(int, int*) = 0;
		};

		// SHGetImageList 在 shell32 里只按序号导出（不是名字），727 是官方约定值。
		using PFN_SHGetImageList = HRESULT(WINAPI*)(int, REFIID, void**);

		constexpr int kShilJumbo = 4;   // SHIL_JUMBO：256px 档

		// ------------------------------------------------------------------
		// shell 虚拟对象（回收站、此电脑、控制面板……）走 PIDL。
		//
		// ⚠ 这类对象没有 exe 路径，配置里写的是 `"::{CLSID}"`。
		//   实测 `SHGetFileInfoW(L"::{645FF040-...}", ...)` 三种 flags 全返回 0 ——
		//   它**不认裸 CLSID 字符串**，必须先 `SHParseDisplayName` 解析出 PIDL，
		//   再带 `SHGFI_PIDL` 调。
		// ------------------------------------------------------------------

		/// 是不是 shell 虚拟对象路径（`"::{CLSID}"` 这种）。
		bool isShellObject(const std::wstring& path)
		{
			return path.size() >= 3 && path[0] == L':' && path[1] == L':';
		}

		/// 解析成 PIDL。**调用方负责 CoTaskMemFree**。失败返回 nullptr。
		PIDLIST_ABSOLUTE parseShellPidl(const std::wstring& path)
		{
			PIDLIST_ABSOLUTE pidl{};
			const HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
			if (FAILED(hr) || !pidl) {
				log(std::format(L"[icon] SHParseDisplayName({}) hr=0x{:08X}", path, (unsigned)hr));
				return nullptr;
			}
			return pidl;
		}

		HICON getJumboIcon(const std::wstring& path)
		{
			SHFILEINFOW sfi{};
			DWORD flags = SHGFI_SYSICONINDEX;
			LPCWSTR target = path.c_str();

			PIDLIST_ABSOLUTE pidl = nullptr;
			if (isShellObject(path)) {
				pidl = parseShellPidl(path);
				if (!pidl) return nullptr;
				target = reinterpret_cast<LPCWSTR>(pidl);
				flags |= SHGFI_PIDL;
			}

			const DWORD_PTR got = SHGetFileInfoW(target, 0, &sfi, sizeof(sfi), flags);
			if (pidl) CoTaskMemFree(pidl);   // ⚠ 之后的 sfi.iIcon 已经不依赖它了

			if (!got) {
				log(L"[icon] SHGetFileInfo(SYSICONINDEX) 失败");
				return nullptr;
			}

			auto shell32 = GetModuleHandleW(L"shell32.dll");
			if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");
			if (!shell32) return nullptr;
			auto fn = reinterpret_cast<PFN_SHGetImageList>(GetProcAddress(shell32, MAKEINTRESOURCEA(727)));
			if (!fn) {
				log(L"[icon] shell32 里没有序号 727（SHGetImageList）");
				return nullptr;
			}

			IImageListShim* list = nullptr;
			const HRESULT hr = fn(kShilJumbo, kIID_IImageList, reinterpret_cast<void**>(&list));
			if (FAILED(hr) || !list) {
				log(std::format(L"[icon] SHGetImageList(jumbo) hr=0x{:08X}", (unsigned)hr));
				return nullptr;
			}
			HICON icon = nullptr;
			const HRESULT hrIcon = list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
			list->Release();
			if (FAILED(hrIcon) || !icon) {
				log(std::format(L"[icon] GetIcon(i={}) hr=0x{:08X}", sfi.iIcon, (unsigned)hrIcon));
				return nullptr;
			}
			return icon;
		}

		// 兜底：jumbo 拿不到时退回 SHGetFileInfo 的 32px 图标（会糊，但比没有好）
		HICON getFallbackIcon(const std::wstring& path)
		{
			SHFILEINFOW sfi{};
			DWORD flags = SHGFI_ICON | SHGFI_LARGEICON;
			LPCWSTR target = path.c_str();

			PIDLIST_ABSOLUTE pidl = nullptr;
			if (isShellObject(path)) {
				pidl = parseShellPidl(path);
				if (!pidl) return nullptr;
				target = reinterpret_cast<LPCWSTR>(pidl);
				flags |= SHGFI_PIDL;
			}

			const DWORD_PTR got = SHGetFileInfoW(target, 0, &sfi, sizeof(sfi), flags);
			if (pidl) CoTaskMemFree(pidl);
			if (!got) return nullptr;
			return sfi.hIcon;
		}

		ComPtr<ID2D1Bitmap> toD2DBitmap(ID2D1DeviceContext* ctx, HICON icon, int targetPx)
		{
			if (!ctx || !icon) return nullptr;

			ComPtr<IWICImagingFactory> wic;
			HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
			if (FAILED(hr)) {
				log(std::format(L"[icon] WIC 工厂创建失败 hr=0x{:08X}", (unsigned)hr));
				return nullptr;
			}

			ComPtr<IWICBitmap> src;
			hr = wic->CreateBitmapFromHICON(icon, &src);
			if (FAILED(hr)) {
				log(std::format(L"[icon] CreateBitmapFromHICON 失败 hr=0x{:08X}", (unsigned)hr));
				return nullptr;
			}

			// ⚠ 必须先过一道格式转换器转成 32bppPBGRA：CreateBitmapFromHICON 给的
			//   格式 D2D 不一定接受（实测直接喂给 CreateBitmapFromWicBitmap 报
			//   0x88982F80）。Ling 的 Image::loadImg 也是这么干的。
			ComPtr<IWICFormatConverter> converter;
			hr = wic->CreateFormatConverter(&converter);
			if (FAILED(hr)) {
				log(std::format(L"[icon] CreateFormatConverter 失败 hr=0x{:08X}", (unsigned)hr));
				return nullptr;
			}
			hr = converter->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
			if (FAILED(hr)) {
				log(std::format(L"[icon] 格式转换到 32bppPBGRA 失败 hr=0x{:08X}", (unsigned)hr));
				return nullptr;
			}

			// 256px → 目标像素：走 WIC 的 Fant 缩放（高质量），比让 D2D 在 DrawBitmap
			// 里线性抽点清楚得多。目标尺寸和源一样就直接用源。
			UINT sw = 0, sh = 0;
			src->GetSize(&sw, &sh);
			ComPtr<IWICBitmapSource> finalSrc = converter;
			if (targetPx > 0 && (static_cast<UINT>(targetPx) != sw || static_cast<UINT>(targetPx) != sh)) {
				ComPtr<IWICBitmapScaler> scaler;
				if (SUCCEEDED(wic->CreateBitmapScaler(&scaler))
					&& SUCCEEDED(scaler->Initialize(converter.Get(), static_cast<UINT>(targetPx), static_cast<UINT>(targetPx), WICBitmapInterpolationModeFant))) {
					finalSrc = scaler;
				}
				else {
					log(L"[icon] WIC 缩放失败，改用原始尺寸");
				}
			}

			ComPtr<ID2D1Bitmap> out;
			hr = ctx->CreateBitmapFromWicBitmap(finalSrc.Get(), nullptr, &out);
			if (FAILED(hr)) {
				log(std::format(L"[icon] CreateBitmapFromWicBitmap 失败 hr=0x{:08X}（src {}x{}）", (unsigned)hr, sw, sh));
				return nullptr;
			}
			return out;
		}

	} // namespace

	Microsoft::WRL::ComPtr<ID2D1Bitmap> loadShellIcon(ID2D1DeviceContext* ctx, const std::wstring& path, int targetPx)
	{
		if (!ctx || path.empty()) return nullptr;

		// .lnk 指向的目标有时才是真实图标来源，但 shell 对 .lnk 本身也能给图标，
		// 而且用快捷方式自己的图标更符合用户预期 —— 所以先直接按原路径取。
		HICON icon = getJumboIcon(path);
		if (!icon) {
			icon = getFallbackIcon(path);
			if (icon) log(L"[icon] 退回 32px 图标（jumbo 不可用）");
		}
		if (!icon) {
			log(L"[icon] 两条路径都没拿到图标");
			return nullptr;
		}

		auto bmp = toD2DBitmap(ctx, icon, targetPx);
		DestroyIcon(icon);
		return bmp;
	}

	std::wstring resolveShortcutTarget(const std::wstring& path)
	{
		if (path.size() < 4) return L"";
		auto ext = path.substr(path.size() - 4);
		for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
		if (ext != L".lnk") return L"";

		ComPtr<IShellLinkW> link;
		if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return L"";
		ComPtr<IPersistFile> file;
		if (FAILED(link.As(&file))) return L"";
		if (FAILED(file->Load(path.c_str(), STGM_READ))) return L"";

		wchar_t buf[MAX_PATH]{};
		WIN32_FIND_DATAW fd{};
		if (FAILED(link->GetPath(buf, MAX_PATH, &fd, SLGP_UNCPRIORITY))) return L"";
		return buf;
	}

	std::wstring displayNameOf(const std::wstring& path)
	{
		auto name = PathFindFileNameW(path.c_str());
		std::wstring out{ name ? name : path.c_str() };
		auto dot = out.find_last_of(L'.');
		if (dot != std::wstring::npos && dot > 0) out = out.substr(0, dot);
		return out;
	}

} // namespace zdock
