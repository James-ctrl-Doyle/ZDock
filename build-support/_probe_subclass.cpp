// 验证：**从外部子类化 Ling 窗口**是否可行（阶段五拖放要用的通路）。
//
// 背景：Ling 的 `WinBase` 只暴露少量可覆写点（onCreated / onHitTest / onMinMaxInfo /
// setCursor / layout）和一组 winrt 事件，**没有通用消息钩子**，也不处理 WM_DROPFILES。
// 而拖放（`WM_DROPFILES` / OLE `IDropTarget`）都是"消息进窗口过程"才能拿到的东西。
//
// 三条路：
//   A) 改 Ling 加钩子          —— 要动别人的库、发新版本，代价大
//   B) 用覆盖窗口收拖放        —— 会挡住 dock 自己的 hover 命中，功能打架
//   C) **子类化 Ling 窗口**     —— 纯 ZDock 侧，零 Ling 改动  ← 本 demo 验证这条
//
// 子类化的做法：
//   ```cpp
//   WNDPROC orig = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&MyProc);
//   // MyProc 里处理完自己的消息后：
//   return CallWindowProcW(orig, hwnd, msg, wp, lp);
//   ```
//
// ⚠ 要验证的不只是"能收到消息"，还有"**没把 Ling 弄坏**"：
//   1. 能收到投给该窗口的自定义消息；
//   2. Ling 自己存在 `GWLP_USERDATA` 里的 self 指针没被破坏；
//   3. 子类化后 Ling 的既有能力（layout / refresh / 原消息处理）仍然工作。
//
// 输出一律 ASCII（Git Bash 管道按 UTF-8 解码，宽字符中文经管道会乱码）。

#include <include/App.h>
#include <include/Node.h>
#include <include/WinBase.h>

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>

namespace {

	constexpr UINT kTestMsg = WM_APP + 999;

	WNDPROC g_orig = nullptr;
	int     g_gotPayload = -1;
	int     g_gotDrop = 0;
	int     g_origCalls = 0;   // 转发给原 wndProc 的次数（证明链路没断）

	/// 被测的 Ling 窗口
	class ProbeWin : public Ling::WinBase
	{
	public:
		int mouseMoves = 0;
		void onCreated() override
		{
			auto* n = body->makeChild<Ling::Node>();
			n->setPositionType(Ling::Position::Absolute);
			n->setPosition(Ling::Edge::Left, 0.f);
			n->setPosition(Ling::Edge::Top, 0.f);
			n->setSize(200.f, 150.f);
			n->setBg(Ling::Color(0x202020FFu));
		}
	};

	LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
	{
		if (m == kTestMsg) {
			g_gotPayload = static_cast<int>(wp);
			return 0;                       // 自己吃掉，不往下传
		}
		if (m == WM_DROPFILES) {
			g_gotDrop = 1;
			DragFinish(reinterpret_cast<HDROP>(wp));
			return 0;
		}
		++g_origCalls;
		return CallWindowProcW(g_orig, h, m, wp, lp);
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
			Sleep(15);
		}
	}

} // namespace

int wmain()
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	Ling::init();
	Ling::WinBase::setAppWindowClassName(L"ZDockSubclassProbe");

	ProbeWin win;
	win.setSize(200.f, 150.f);
	win.createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
	win.setPosition(20, 20);
	win.show();
	win.layout();
	win.refresh();
	Pump(400);

	if (!win.hwnd) { std::printf("ling window failed\n"); return 1; }

	const LONG_PTR userBefore = GetWindowLongPtrW(win.hwnd, GWLP_USERDATA);

	std::printf("\n===== subclassing a Ling window =====\n\n");
	std::printf("ling hwnd                = %p\n", reinterpret_cast<void*>(win.hwnd));
	std::printf("GWLP_USERDATA (before)   = 0x%p\n",
		reinterpret_cast<void*>(static_cast<ULONG_PTR>(userBefore)));

	// ---- 子类化 ----
	g_orig = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrW(win.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubProc)));
	std::printf("original wndproc         = %p  %s\n",
		reinterpret_cast<void*>(g_orig), g_orig ? "(captured)" : "(NULL - FAIL!)");
	if (!g_orig) return 1;

	// ---- 1) 自定义消息能收到吗 ----
	PostMessageW(win.hwnd, kTestMsg, 4242, 0);
	Pump(300);
	std::printf("\n[1] PostMessage(WM_APP+999, 4242) -> received %d  %s\n",
		g_gotPayload, g_gotPayload == 4242 ? "OK" : "FAIL");

	// ---- 2) Ling 的 self 指针没被破坏吗 ----
	const LONG_PTR userAfter = GetWindowLongPtrW(win.hwnd, GWLP_USERDATA);
	std::printf("[2] GWLP_USERDATA unchanged  %s (before=0x%p after=0x%p)\n",
		userAfter == userBefore ? "OK" : "CHANGED - RISK!",
		reinterpret_cast<void*>(static_cast<ULONG_PTR>(userBefore)),
		reinterpret_cast<void*>(static_cast<ULONG_PTR>(userAfter)));

	// ---- 3) 原 wndProc 链路还通吗（子类化后 Ling 的既有能力）----
	const int callsBefore = g_origCalls;
	PostMessageW(win.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
	win.layout();
	win.refresh();
	Pump(400);
	std::printf("[3] forwarded to original wndproc: %d more calls  %s\n",
		g_origCalls - callsBefore, (g_origCalls - callsBefore) > 0 ? "OK (chain alive)" : "FAIL");
	std::printf("    window still valid      %s\n", IsWindow(win.hwnd) ? "OK" : "FAIL");
	std::printf("    window still visible    %s\n", IsWindowVisible(win.hwnd) ? "OK" : "FAIL");

	// ---- 4) 拖放受体能不能挂上 ----
	DragAcceptFiles(win.hwnd, TRUE);
	std::printf("[4] DragAcceptFiles      called (real drop needs a manual drag)\n");
	std::printf("    WM_DROPFILES received  %s\n", g_gotDrop ? "yes" : "no (expected: no manual drag happened)");

	// ---- 5) 还原 ----
	SetWindowLongPtrW(win.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig));
	std::printf("\n[5] wndproc restored\n");

	std::printf("\nCONCLUSION: subclassing a Ling window works = %s\n",
		(g_gotPayload == 4242 && userAfter == userBefore) ? "YES" : "NO");
	Ling::dispose();
	return 0;
}
