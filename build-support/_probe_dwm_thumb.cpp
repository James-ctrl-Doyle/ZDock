// 最小 demo：验证「WS_EX_NOREDIRECTIONBITMAP 的窗口能不能当 DWM 缩略图宿主」。
//
// 背景（任务书 §9.7）：
//   悬停预览首选 `DwmRegisterThumbnail`（DWM 直接合成目标窗口的实时画面，零截图成本），
//   但**宿主窗口形态可能限制可用性** —— 例如 layered 窗口做不了宿主。
//
// ZDock 的情况更微妙：dock 主窗口是 DirectComposition 栈（带 WS_EX_NOREDIRECTIONBITMAP，
// **不是** layered）。而 NOREDIRECTIONBITMAP 的字面意思就是"没有重定向表面"，
// 而 DWM 缩略图恰恰依赖宿主的重定向表面来合成 —— 所以**很可能做不了宿主**。
//
// 这个 demo 把三种宿主形态摆在一起测：
//   A) 普通顶层窗口（有重定向表面）      —— 对照组，应该成功
//   B) WS_EX_NOREDIRECTIONBITMAP        —— 模拟 Composition 窗口
//   C) WS_EX_LAYERED                    —— 任务书点名的反例
//
// 判据：DwmRegisterThumbnail 的 HRESULT + 截图里宿主的像素是否出现了源窗口的画面。

#include <Windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

	constexpr const wchar_t* kSrcClass = L"ZThumbDemoSrc";
	constexpr const wchar_t* kDstClass = L"ZThumbDemoDst";

	LRESULT CALLBACK SrcProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
	{
		if (m == WM_PAINT) {
			PAINTSTRUCT ps{};
			HDC dc = BeginPaint(h, &ps);
			RECT rc{};
			GetClientRect(h, &rc);
			// 纯品红：截图里数这个颜色的像素就能判断"有没有合成进来"
			HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
			FillRect(dc, &rc, b);
			DeleteObject(b);
			EndPaint(h, &ps);
			return 0;
		}
		return DefWindowProcW(h, m, wp, lp);
	}

	LRESULT CALLBACK DstProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
	{
		if (m == WM_PAINT) {
			PAINTSTRUCT ps{};
			HDC dc = BeginPaint(h, &ps);
			RECT rc{};
			GetClientRect(h, &rc);
			// 纯黑底：缩略图画上去就是"黑底上出现品红块"
			HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
			FillRect(dc, &rc, b);
			DeleteObject(b);
			EndPaint(h, &ps);
			return 0;
		}
		return DefWindowProcW(h, m, wp, lp);
	}

	bool RegisterCls(const wchar_t* name, WNDPROC proc)
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = name;
		wc.hbrBackground = nullptr;
		return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	}

	int CountPixels(HWND hwnd, BYTE wantR, BYTE wantG, BYTE wantB)
	{
		RECT rc{};
		GetWindowRect(hwnd, &rc);
		const int w = rc.right - rc.left, h = rc.bottom - rc.top;
		if (w <= 0 || h <= 0) return -1;

		HDC src = GetWindowDC(hwnd);
		HDC mem = CreateCompatibleDC(src);
		HBITMAP bmp = CreateCompatibleBitmap(src, w, h);
		HGDIOBJ old = SelectObject(mem, bmp);
		// ⚠ 只渲染窗口自身（PW_RENDERFULLCONTENT），绝不 BitBlt 桌面
		const BOOL ok = PrintWindow(hwnd, mem, 2);
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
					const int dr = std::abs(static_cast<int>(buf[i + 2]) - wantR);
					const int dg = std::abs(static_cast<int>(buf[i + 1]) - wantG);
					const int db = std::abs(static_cast<int>(buf[i]) - wantB);
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

	struct Case {
		const wchar_t* name;
		const char* asciiName;
		DWORD exStyle;
		HWND hwnd{ nullptr };
		HRESULT reg{ E_FAIL };
		HRESULT upd{ E_FAIL };
		int magenta{ -1 };
	};

	void PumpMessages(int ms)
	{
		const DWORD end = GetTickCount() + ms;
		while (GetTickCount() < end) {
			MSG msg{};
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			Sleep(20);
		}
	}

	// ⚠ 窗口尺寸刻意取小、位置贴着屏幕左上角：
	//   DWM 只合成**可见**窗口，所以这几个窗口必须真的显示出来；
	//   但没必要为了一次验证占掉用户半屏 —— 160x120 够数像素了，
	//   而且全程 SW_SHOWNOACTIVATE（不抢焦点）、跑完立刻销毁。
	constexpr int kWinW = 160;
	constexpr int kWinH = 120;

} // namespace

int wmain()
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	if (!RegisterCls(kSrcClass, SrcProc) || !RegisterCls(kDstClass, DstProc)) {
		std::wprintf(L"RegisterClassEx 失败 err=%lu\n", GetLastError());
		return 1;
	}
	HINSTANCE hInst = GetModuleHandleW(nullptr);

	// 源窗口：品红底。放在宿主右边一点，避免和宿主重叠导致数像素不准。
	HWND src = CreateWindowExW(WS_EX_TOOLWINDOW, kSrcClass, L"ZThumbSrc", WS_POPUP,
		20 + kWinW + 12, 20, kWinW, kWinH, nullptr, nullptr, hInst, nullptr);
	if (!src) { std::wprintf(L"源窗口创建失败\n"); return 1; }
	ShowWindow(src, SW_SHOWNOACTIVATE);
	UpdateWindow(src);

	std::vector<Case> cases = {
		{ L"A 普通窗口（有重定向表面）",
		  "A plain window (has redirection surface)", 0, nullptr, E_FAIL, E_FAIL, -1 },
		{ L"B NOREDIRECTIONBITMAP（Composition 式）",
		  "B WS_EX_NOREDIRECTIONBITMAP (Composition-style)",
		  WS_EX_NOREDIRECTIONBITMAP, nullptr, E_FAIL, E_FAIL, -1 },
		{ L"C LAYERED（任务书点名的反例）",
		  "C WS_EX_LAYERED (the counter-example)",
		  WS_EX_LAYERED, nullptr, E_FAIL, E_FAIL, -1 },
	};

	int y = 20;
	for (auto& c : cases) {
		c.hwnd = CreateWindowExW(c.exStyle, kDstClass, L"ZThumbDst", WS_POPUP | WS_VISIBLE,
			20, y, kWinW, kWinH, nullptr, nullptr, hInst, nullptr);
		y += kWinH + 12;
		if (!c.hwnd) {
			std::wprintf(L"%s : 窗口创建失败 err=%lu\n", c.name, GetLastError());
			continue;
		}
		if (c.exStyle & WS_EX_LAYERED) {
			// layered 必须设置属性才有内容
			SetLayeredWindowAttributes(c.hwnd, 0, 255, LWA_ALPHA);
		}
		ShowWindow(c.hwnd, SW_SHOWNOACTIVATE);
		UpdateWindow(c.hwnd);
	}
	PumpMessages(400);

	std::printf("\n===== DWM thumbnail host test (source = %dx%d magenta) =====\n\n",
		kWinW, kWinH);
	for (auto& c : cases) {
		if (!c.hwnd) continue;

		DWM_THUMBNAIL_PROPERTIES props{};
		HTHUMBNAIL thumb = nullptr;
		c.reg = DwmRegisterThumbnail(c.hwnd, src, &thumb);
		if (SUCCEEDED(c.reg) && thumb) {
			RECT dst{};
			GetClientRect(c.hwnd, &dst);
			props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
			props.rcDestination = dst;
			props.fVisible = TRUE;
			props.opacity = 255;
			c.upd = DwmUpdateThumbnailProperties(thumb, &props);
		}
		PumpMessages(700);
		c.magenta = CountPixels(c.hwnd, 255, 0, 255);

		// ⚠ ASCII only：Git Bash 的管道会按 UTF-8 解码，宽字符中文经管道变成
		//   GBK 字节 → 乱码，数字都被吞掉。验证脚本的输出一律走 ASCII。
		std::printf("%-42s\n", c.asciiName);
		std::printf("    DwmRegisterThumbnail  -> hresult=0x%08lX  %s\n",
			static_cast<unsigned long>(c.reg), SUCCEEDED(c.reg) ? "OK" : "FAIL");
		std::printf("    DwmUpdateThumbnail... -> hresult=0x%08lX  %s\n",
			static_cast<unsigned long>(c.upd),
			c.upd == E_PENDING ? "(not called)" : (SUCCEEDED(c.upd) ? "OK" : "FAIL"));
		std::printf("    magenta pixels in host -> %d  %s\n\n", c.magenta,
			c.magenta > 1000 ? "==> picture IS composited"
			: "==> picture NOT composited");
	}

	std::printf("conclusion:\n");
	std::printf("  plain window as host          : %s\n",
		cases[0].magenta > 1000 ? "YES" : "NO");
	std::printf("  NOREDIRECTIONBITMAP as host   : %s\n",
		cases[1].magenta > 1000 ? "YES" : "NO  <== ZDock main window cannot host it");
	std::printf("  LAYERED as host               : %s\n",
		cases[2].magenta > 1000 ? "YES" : "NO");

	DestroyWindow(src);
	for (auto& c : cases) if (c.hwnd) DestroyWindow(c.hwnd);
	return 0;
}
