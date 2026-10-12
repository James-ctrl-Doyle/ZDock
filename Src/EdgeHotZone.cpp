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

		// ⚠⚠ alpha 必须是**非零** —— 这是整个自动隐藏能不能唤出的关键。
		//
		// MSDN（WS_EX_LAYERED）：
		//   "Hit testing of a layered window is based on the shape and transparency
		//    of the window. This means that the areas of the window that are
		//    color-keyed or **whose alpha value is zero will let the mouse messages
		//    through**."
		//
		// 也就是说 **alpha=0 的热区收不到任何真实鼠标消息**（系统命中测试直接跳过它）。
		// 早期这里传的是 0，注释还写着"依然收得到鼠标消息" —— 那是错的。
		// 用户 2026-10-12 报"开启自动隐藏好像没效果"，根因就在这里。
		//
		// 为什么此前没测出来：探针是 `PostMessage(WM_MOUSEMOVE)` 直接投给热区 hwnd 的，
		// **绕过了系统命中测试**，所以状态机在探针手里一切正常、真实鼠标却碰不到它。
		// 机制实证见 `build-support/_probe_layered_hittest.py`（同一矩形：
		// alpha=0 中心点命中到下层窗口；alpha=1 命中自己）。
		//
		// 取 1 而不是更大：1/255 ≈ 0.4% 不透明度，肉眼不可见，但**非零** → 参与命中测试。
		// 这里**不能**改用 LWA_COLORKEY —— color-key 掉的区域同样会让鼠标穿透。
		const BYTE kHitAlpha = 1;
		SetLayeredWindowAttributes(hwnd, 0, kHitAlpha, LWA_ALPHA);

		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		entered = false;
		enabled = true;
		log(std::format(L"[hotzone] 已创建 ({},{})-({},{}) {}x{}",
			screenRect.left, screenRect.top, screenRect.right, screenRect.bottom, w, h));
		return true;
	}

	void EdgeHotZone::setEnabled(bool on)
	{
		if (!hwnd) { enabled = on; return; }
		if (enabled == on) return;
		enabled = on;
		// 隐藏的窗口不参与命中测试 → 全屏应用期间不会吃它边缘的点击。
		// hwnd 保留，探针的 PostMessage 注入通道不受影响。
		ShowWindow(hwnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
		log(on ? L"[hotzone] 已启用（参与命中）" : L"[hotzone] 已停用（不参与命中，避免挡住全屏应用）");
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
		// ⚠ 标志必须跟随 enabled：常驻 SWP_SHOWWINDOW 会把"全屏期间被停用"的
		//   热区重新显示出来，又把全屏应用的边缘点击吃回去。
		SetWindowPos(hwnd, HWND_TOPMOST,
			screenRect.left, screenRect.top,
			screenRect.right - screenRect.left,
			screenRect.bottom - screenRect.top,
			SWP_NOACTIVATE | (enabled ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
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
