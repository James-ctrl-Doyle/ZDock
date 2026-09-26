#include "EdgeHotZone.h"

#include <format>

namespace zdock {

	namespace {
		constexpr const wchar_t* kHotZoneClass = L"ZDock.EdgeHotZone";
		/// 钩子窗口的 USERDATA 里挂的对象指针（WndProc 里取）。
		constexpr const wchar_t* kPropKey = L"ZDockHotZoneSelf";
	}

	EdgeHotZone::~EdgeHotZone()
	{
		destroy();
	}

	bool EdgeHotZone::create(const RECT& screenRect)
	{
		// 重复 create 时先清掉旧的：不做的话屏幕边上会越积越多看不见的窗口。
		destroy();

		HINSTANCE hInst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &EdgeHotZone::wndProc;
		wc.hInstance = hInst;
		wc.lpszClassName = kHotZoneClass;
		// 无背景刷 → 窗口不擦除背景，配合下面的 SetLayeredWindowAttributes 全透明。
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			log(std::format(L"[hotzone] RegisterClassExW 失败 err={}", GetLastError()));
			return false;
		}

		const int w = screenRect.right - screenRect.left;
		const int h = screenRect.bottom - screenRect.top;

		hwnd = CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
			kHotZoneClass, L"ZDockHotZone",
			WS_POPUP,
			screenRect.left, screenRect.top, w, h,
			nullptr, nullptr, hInst, this);
		if (!hwnd) {
			log(std::format(L"[hotzone] CreateWindowExW 失败 err={}", GetLastError()));
			return false;
		}

		// 全透明（alpha=0）：用户完全看不见它，但它依然收得到鼠标消息。
		// ⚠ 这里**不能**用 WS_EX_TRANSPARENT 或者 SetWindowRgn 挖空 ——
		//   那样窗口就不参与命中测试了，热区也就不生效了。
		SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		entered = false;
		log(std::format(L"[hotzone] 已创建 ({},{})-({},{}) {}x{}",
			screenRect.left, screenRect.top, screenRect.right, screenRect.bottom, w, h));
		return true;
	}

	void EdgeHotZone::destroy()
	{
		entered = false;
		if (hwnd) {
			DestroyWindow(hwnd);
			hwnd = nullptr;
			log(L"[hotzone] 已销毁");
		}
	}

	void EdgeHotZone::moveTo(const RECT& screenRect)
	{
		if (!hwnd) return;
		SetWindowPos(hwnd, HWND_TOPMOST,
			screenRect.left, screenRect.top,
			screenRect.right - screenRect.left,
			screenRect.bottom - screenRect.top,
			SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}

	LRESULT CALLBACK EdgeHotZone::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		EdgeHotZone* self = reinterpret_cast<EdgeHotZone*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		if (msg == WM_NCCREATE) {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = reinterpret_cast<EdgeHotZone*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			(void)kPropKey;
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
		if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

		switch (msg) {
		case WM_MOUSEMOVE:
			// ⚠ 只在"首次进入"时回调一次。鼠标在热区里挪动会持续来
			//   WM_MOUSEMOVE，每次都回调会把 dock 的滑入动画反复重启。
			if (!self->entered) {
				self->entered = true;
				self->log(L"[hotzone] 鼠标进入，触发滑入");
				if (self->onEnter) self->onEnter();
			}
			return 0;

		case WM_MOUSELEAVE:
			// 离开后允许下一次进入再触发
			self->entered = false;
			return 0;

		case WM_NCHITTEST:
			// 热区必须可命中（HTCLIENT），否则收不到鼠标消息。
			return HTCLIENT;

		case WM_ERASEBKGND:
			return 1;   // 不擦背景（全透明，擦了也没意义）

		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

} // namespace zdock
