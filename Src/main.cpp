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

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	zdock::log(L"[main] message loop ended");

	dock.reset();
	Ling::dispose();
	zdock::log(L"[main] exit");
	return 0;
}
