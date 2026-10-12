#include "DockWin.h"
#include "Log.h"
#include "SingleInstance.h"
#include <include/App.h>

#include <memory>
#include <shellscalingapi.h>

// ⚠ 参数类型必须是 LPWSTR：SDK 的 WinBase.h 里声明了
//   `int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)`，
//   写成 LPTSTR 在非 UNICODE 构建下等于 char*，撞上这个声明直接 C2731（extern "C" 不能重载）。
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	zdock::log(L"[main] start");

	// DPI 必须在建任何窗口之前定下来。per-monitor v2：多屏不同缩放时不被位图拉伸。
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	zdock::log(L"[main] dpi awareness set");

	// ---------------------------------------------------------------------------
	// 单实例判定必须在 Ling::init() **之前**做。
	//
	// ⚠ 绝不能用 Ling::App::refuseSecondInstance()：它靠 FindWindow(L"STATIC", appID)
	//   判定，而 appID 来自 App() 构造函数里的 COMPILE_TIME_RAND_STR(6) ——
	//   那个构造函数是**编译进 Ling.lib 静态库**的，种子（__TIME__/__COUNTER__）
	//   取自库自己的编译时刻，打库时就固化了。任何链接**同一个 Ling.lib** 的程序
	//   （ZPin 与 ZDock 都用 ling-v1.3.0 发布包）拿到的 appID 完全相同，
	//   于是共用同一个 message-only 窗口槽位，后启动者必被先启动者误判成
	//   "第二实例"并 ExitProcess —— 这就是 ZPin/ZDock 互相冲突的根因。
	//   改用自己命名的 mutex，见 SingleInstance.h 的详细说明。
	// ---------------------------------------------------------------------------
	zdock::SingleInstance guard;
	if (!guard.acquire()) {
		zdock::log(L"[main] 单实例让位，退出");
		return 0;
	}

	Ling::init();
	zdock::log(L"[main] ling init done");
	// 窗口类名与图标资源必须在**第一个窗口之前**设好（窗口类一个进程只注册一次）
	Ling::WinBase::setAppWindowClassName(L"ZDock");

	std::unique_ptr<zdock::DockWin> dock = std::make_unique<zdock::DockWin>();
	dock->create();
	zdock::log(L"[main] dock created");
	dock->show();

	// ---------------------------------------------------------------------------
	// 高精度动画驱动（0.1.27）。
	//
	// ⚠ 为什么需要：滑动补间原来靠 `WM_TIMER`，而它的节拍被系统时钟钉在 **15.625ms** ——
	//   请求 10ms 也只给 1 拍，实测天花板就是 **~64fps**（详见 _probe_timer_resolution.py）。
	//   要跑到 ~120fps，必须换一个能按 ~8ms 精确唤醒的驱动器：
	//   `SetWaitableTimer`（100ns 单位）+ `MsgWaitForMultipleObjects` 同时等"定时器"和"消息"。
	//
	// ⚠⚠ **只在滑动期间启用**。空闲时每 8ms 醒一次会把 CPU 占用顶上去，而本项目的红线是
	//   "空闲 CPU 接近 0"。所以定时器默认处于**未启动**状态，由 DockWin 通过
	//   `onAnimationTickNeeded` 在滑动开始/结束时开关。
	//
	// ⚠ 拿不到高精度定时器就退回 `WM_TIMER` 那条老路（tickSlide 里保留着兜底），
	//   行为与 0.1.26 一致，不会更差。
	// ---------------------------------------------------------------------------
	constexpr int kAnimTickMs = 6;   // ≈166fps（用户 2026-10-13 确认"170 也 OK"）
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
	HANDLE hAnimTick = CreateWaitableTimerExW(
		nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!hAnimTick) {
		hAnimTick = CreateWaitableTimerW(nullptr, FALSE, nullptr);
		zdock::log(L"[main] 无高精度定时器，退回普通 waitable timer");
	}
	if (hAnimTick) {
		dock->onAnimationTickNeeded = [hAnimTick](bool on) {
			LARGE_INTEGER due{};
			if (on) {
				due.QuadPart = -10000LL * kAnimTickMs;   // 负数 = 相对时间，单位 100ns
				SetWaitableTimer(hAnimTick, &due, kAnimTickMs, nullptr, nullptr, FALSE);
			}
			else {
				// pDueTime = nullptr 表示取消（周期也随之失效）
				SetWaitableTimer(hAnimTick, nullptr, 0, nullptr, nullptr, FALSE);
			}
			};
		zdock::log(L"[main] 高精度动画驱动就绪（6ms 周期，仅滑动期间运行）");
	}

	MSG msg;
	bool running = true;
	while (running)
	{
		const DWORD nHandles = hAnimTick ? 1u : 0u;
		const DWORD r = MsgWaitForMultipleObjects(nHandles, &hAnimTick,
			FALSE, INFINITE, QS_ALLINPUT);
		if (nHandles && r == WAIT_OBJECT_0) {
			dock->animationTick();
			continue;
		}
		if (r == WAIT_OBJECT_0 + nHandles) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT) { running = false; break; }
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			continue;
		}
		break;   // WAIT_FAILED / WAIT_ABANDONED 之类：别再空转
	}
	zdock::log(L"[main] message loop ended");
	if (hAnimTick) CloseHandle(hAnimTick);

	dock.reset();
	Ling::dispose();
	zdock::log(L"[main] exit");
	return 0;
}
