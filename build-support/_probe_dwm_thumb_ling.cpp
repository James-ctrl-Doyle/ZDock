// 决定性验证：**真 Ling 窗口**（DirectComposition 栈）能不能当 DWM 缩略图宿主。
//
// 为什么要单独做这一步：`_probe_dwm_thumb.cpp` 里测的是"普通 GDI 窗口 + 那个扩展样式"，
// 三种形态都能做宿主 —— 但那**不等于** Ling 的窗口也能。ZDock 的主窗口是
// DirectComposition 合成的（Ling 内部给它加了 WS_EX_NOREDIRECTIONBITMAP），
// 而 DWM 缩略图依赖宿主的重定向表面。所以必须拿真 Ling 窗口实测一次。
//
// ⚠ 输出一律 ASCII：Git Bash 的管道按 UTF-8 解码，宽字符中文经管道会变成 GBK 字节，
//   数字都被吞掉（踩过）。

#include <include/App.h>
#include <include/Node.h>
#include <include/WinBase.h>

#include <Windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace {

	constexpr int kWinW = 160;
	constexpr int kWinH = 120;

	// 源窗口：普通 GDI，品红底（数像素用）
	constexpr const wchar_t* kSrcClass = L"ZLingThumbSrc";

	LRESULT CALLBACK SrcProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
	{
		if (m == WM_PAINT) {
			PAINTSTRUCT ps{};
			HDC dc = BeginPaint(h, &ps);
			RECT rc{};
			GetClientRect(h, &rc);
			HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
			FillRect(dc, &rc, b);
			DeleteObject(b);
			EndPaint(h, &ps);
			return 0;
		}
		return DefWindowProcW(h, m, wp, lp);
	}

	/// 被测的 Ling 窗口：一块不透明黑底，缩略图会合成在它上面。
	class ProbeWin : public Ling::WinBase
	{
	public:
		void onCreated() override
		{
			auto* n = body->makeChild<Ling::Node>();
			n->setPositionType(Ling::Position::Absolute);
			n->setPosition(Ling::Edge::Left, 0.f);
			n->setPosition(Ling::Edge::Top, 0.f);
			n->setSize(static_cast<float>(kWinW), static_cast<float>(kWinH));
			// ⚠ Ling::Color 是 0xRRGGBBAA（跟 config 里的 "#RRGGBBAA" 一致）
			n->setBg(Ling::Color(0x000000FFu));   // 不透明黑
		}
	};

	int CountMagenta(HWND hwnd)
	{
		RECT rc{};
		GetWindowRect(hwnd, &rc);
		const int w = rc.right - rc.left, h = rc.bottom - rc.top;
		if (w <= 0 || h <= 0) return -1;
		HDC src = GetWindowDC(hwnd);
		HDC mem = CreateCompatibleDC(src);
		HBITMAP bmp = CreateCompatibleBitmap(src, w, h);
		HGDIOBJ old = SelectObject(mem, bmp);
		const BOOL ok = PrintWindow(hwnd, mem, 2);   // 只渲染窗口自身
		int hits = 0;
		if (ok) {
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			std::vector<BYTE> buf(static_cast<size_t>(w) * h * 4);
			if (GetDIBits(mem, bmp, 0, h, buf.data(), &bi, DIB_RGB_COLORS)) {
				for (size_t i = 0; i + 3 < buf.size(); i += 4) {
					const int dr = std::abs(static_cast<int>(buf[i + 2]) - 255);
					const int dg = std::abs(static_cast<int>(buf[i + 1]) - 0);
					const int db = std::abs(static_cast<int>(buf[i]) - 255);
					if (dr < 40 && dg < 40 && db < 40) ++hits;
				}
			}
		}
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		ReleaseDC(hwnd, src);
		return ok ? hits : -1;
	}

	void Pump(int ms)
	{
		const DWORD end = GetTickCount() + ms;
		while (GetTickCount() < end) {
			MSG m{};
			while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&m);
				DispatchMessageW(&m);
			}
			Sleep(20);
		}
	}

} // namespace

int wmain()
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	Ling::init();
	Ling::WinBase::setAppWindowClassName(L"ZDockThumbProbeLing");

	// ---- 源窗口（品红）----
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = SrcProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = kSrcClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		std::printf("RegisterClassExW failed\n");
		return 1;
	}
	HWND src = CreateWindowExW(WS_EX_TOOLWINDOW, kSrcClass, L"ZLingThumbSrc", WS_POPUP,
		20 + kWinW + 12, 20, kWinW, kWinH, nullptr, nullptr,
		GetModuleHandleW(nullptr), nullptr);
	if (!src) { std::printf("source window failed\n"); return 1; }
	ShowWindow(src, SW_SHOWNOACTIVATE);
	UpdateWindow(src);

	// ---- 被测的 Ling 窗口 ----
	ProbeWin probe;
	probe.setSize(static_cast<float>(kWinW), static_cast<float>(kWinH));
	probe.createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
	probe.setPosition(20, 20);
	probe.show();
	probe.layout();
	probe.refresh();
	Pump(400);

	if (!probe.hwnd) { std::printf("ling window failed\n"); return 1; }

	// 顺便把实际扩展样式打出来，确认它确实是 Composition 式（NOREDIRECTIONBITMAP）
	const LONG_PTR ex = GetWindowLongPtrW(probe.hwnd, GWL_EXSTYLE);
	std::printf("\n===== DWM thumbnail host test : REAL Ling window =====\n\n");
	std::printf("ling hwnd            = %p\n", reinterpret_cast<void*>(probe.hwnd));
	std::printf("exstyle              = 0x%08lX\n", static_cast<unsigned long>(ex));
	std::printf("  WS_EX_NOREDIRECTIONBITMAP : %s\n",
		(ex & WS_EX_NOREDIRECTIONBITMAP) ? "YES" : "no");
	std::printf("  WS_EX_LAYERED             : %s\n",
		(ex & WS_EX_LAYERED) ? "YES" : "no");
	std::printf("\n");

	DWM_THUMBNAIL_PROPERTIES props{};
	HTHUMBNAIL thumb = nullptr;
	const HRESULT hr = DwmRegisterThumbnail(probe.hwnd, src, &thumb);
	std::printf("DwmRegisterThumbnail -> hresult=0x%08lX  %s\n",
		static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "OK" : "FAIL");

	HRESULT hr2 = E_PENDING;
	if (SUCCEEDED(hr) && thumb) {
		RECT dst{ 0, 0, kWinW, kWinH };
		props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
		props.rcDestination = dst;
		props.fVisible = TRUE;
		props.opacity = 255;
		hr2 = DwmUpdateThumbnailProperties(thumb, &props);
	}
	std::printf("DwmUpdateThumbnail   -> hresult=0x%08lX  %s\n",
		static_cast<unsigned long>(hr2), SUCCEEDED(hr2) ? "OK" : "FAIL");

	Pump(900);
	const int pix = CountMagenta(probe.hwnd);
	std::printf("magenta pixels in ling window -> %d (of ~%d)\n", pix, kWinW * kWinH);
	std::printf("\nCONCLUSION: Ling(DirectComposition) window can host DWM thumbnail : %s\n",
		pix > kWinW * kWinH / 2 ? "YES" : "NO");

	DestroyWindow(src);
	Ling::dispose();
	return 0;
}
