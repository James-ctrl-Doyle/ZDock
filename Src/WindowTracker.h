#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace zdock {

	/// <summary>
	/// 一个被跟踪的顶层窗口（任务栏语义里的"一个窗口"）。
	/// </summary>
	struct TrackedWindow
	{
		HWND hwnd{ nullptr };
		DWORD pid{ 0 };
		std::wstring exePath;      // 空 = UWP / 打包应用（没有传统 exe 路径）
		std::wstring aumid;        // AppUserModelID，UWP / 打包应用的分组键
		std::wstring title;
		bool minimized{ false };
		bool foreground{ false };
		ULONGLONG lastActiveTick{ 0 };   // 用于"多窗口取最近使用的"
	};

	/// <summary>
	/// 一个应用分组（同一个应用的所有窗口）。
	///
	/// 分组键是**双轨**的（任务书 §9.4）：能拿到 AUMID 就用 AUMID，
	/// 否则用 exe 路径的小写形式。UWP / 打包应用没有传统 exe 路径，
	/// 只靠路径分组会把它们全并成一个。
	/// </summary>
	struct AppGroup
	{
		std::wstring key;           // AUMID 或小写 exe 路径
		std::wstring exePath;       // 可能为空（UWP）
		std::wstring aumid;         // 可能为空（传统 Win32）
		std::wstring displayName;   // 进程名去扩展名，或 UWP 的显示名
		std::vector<HWND> windows;  // 按"最近使用"倒序，[0] 是最该被切换到的

		bool running() const { return !windows.empty(); }
		/// <summary>该分组里最近的窗口是否在前台。</summary>
		bool isForeground() const;
	};

	/// <summary>
	/// 事件驱动的窗口跟踪器（任务书 §9.4）。
	///
	/// ⚠ 绝对不轮询（红线 4）：所有变化都来自
	///   · RegisterShellHookWindow 的 HSHELL_* 回调（主通道，事件量小、语义准）
	///   · SetWinEventHook 的少数几个事件（只补 shell hook 拿不到的）
	/// 实测数据（build-support/_probe_track_api.py）：3 秒内 SetWinEventHook 能来
	/// 260 次 CREATE / 249 次 SHOW / 252 次 NAMECHANGE，而 shell hook 只有个位数。
	/// 所以**主用 shell hook**，win event 只订阅 EVENT_SYSTEM_MINIMIZESTART/END
	/// 与 EVENT_OBJECT_NAMECHANGE。
	///
	/// ⚠ 它自己建一个隐藏的接收窗口（因为 Ling 的 WinBase 不暴露消息口），
	///   与 dock 主窗口完全独立。
	/// </summary>
	class WindowTracker
	{
	public:
		WindowTracker();
		~WindowTracker();

		/// <summary>建接收窗口 + 注册钩子。返回是否成功。</summary>
		bool start();
		void stop();

		/// <summary>全量重建分组表（只在启动时做一次；之后靠事件增量维护）。</summary>
		void rebuildAll();

		/// <summary>当前所有分组（含未运行的固定项不在内 —— 那是 DockWin 的事）。</summary>
		const std::vector<AppGroup>& groups() const { return groupList; }

		/// <summary>按分组键找分组；找不到返回 nullptr。</summary>
		const AppGroup* findGroup(const std::wstring& key) const;

		/// <summary>
		/// 按 exe 路径找分组（固定图标用它判断"这个应用的运行状态"）。
		/// 内部按小写路径匹配；UWP 分组不会被路径匹配到。
		/// </summary>
		const AppGroup* findByExePath(const std::wstring& exePath) const;

		/// <summary>分组表发生任何变化时回调（DockWin 用它刷新指示器 / 临时图标）。</summary>
		std::function<void()> onChanged;

		/// <summary>前台窗口变化时回调（参数是新前台 hwnd，可能为 nullptr）。</summary>
		std::function<void(HWND)> onForegroundChanged;

		/// <summary>有窗口发出"注意请求"（HSHELL_FLASH）时回调。</summary>
		std::function<void(HWND)> onFlash;

		/// <summary>全屏应用进入 / 退出的判定变化时回调（参数 true = 有全屏应用在前台）。</summary>
		std::function<void(bool)> onFullscreenChanged;

		/// <summary>
		/// explorer 重启（`TaskbarCreated` 广播）时回调（任务书 §2 功能表 #32）。
		///
		/// ⚠ 这个广播只发给**顶层窗口**，我们借自己的接收窗口收（它已经是顶层了）。
		///   Ling 的 WinBase 不暴露消息口，收不了广播，所以只能在跟踪器这儿收。
		/// </summary>
		std::function<void()> onTaskbarCreated;

		/// <summary>
		/// 显示环境变化时回调：分辨率 / 色深 / 主屏切换（`WM_DISPLAYCHANGE`），
		/// 或系统度量变化（`WM_SETTINGCHANGE` 里的工作区、窗口度量、显示相关项）。
		///
		/// ⚠ 这两条都是**广播给顶层窗口**的消息，Ling 的 WinBase 不暴露消息口，
		///   所以和 `TaskbarCreated` 一样借这个接收窗口收。
		///
		/// ⚠ DPI 变化**不走这里** —— `WM_DPICHANGED` 是发给"被移动/受影响的那个顶层窗口"的，
		///   广播不到我们；Ling 已经处理了它（`WinBase::dpiChange` + `onDpiChanged` 事件），
		///   DockWin 订阅那个事件即可。
		/// </summary>
		std::function<void()> onDisplayChanged;

		/// <summary>
		/// **仅自动化测试用**的注入通道：收到 `kMsgTestInject` 时回调，参数 = 全屏状态。
		///
		/// ⚠ 为什么需要它：全屏让位这条链路的真值来自 `GetForegroundWindow()`，
		///   要造一个真全屏前台窗口就得 `SetForegroundWindow` —— 那会抢走用户的
		///   输入焦点（本工作区明确禁止）。所以留一条注入通道让探针用 `PostMessage`
		///   直接驱动，完全不碰焦点。正常运行时没有任何代码会发这条消息。
		///
		/// ⚠ 载体选这里（而不是热区窗口）：接收窗口是**常驻**的，
		///   而热区会随 `autoHide` 开关创建/销毁 —— 一旦热区没了，
		///   注入通道就断了（踩过：全屏让位与自动隐藏解耦后，
		///   "不自动隐藏但要全屏让位"的组合下根本没有热区可投）。
		/// </summary>
		std::function<void(bool)> onTestInject;

		/// <summary>测试注入用的消息号（探针 PostMessage 时用）。</summary>
		static constexpr UINT kMsgTestInject = WM_APP + 100;

		/// <summary>当前是否有全屏应用在前台。</summary>
		bool fullscreenActive() const { return fullscreen; }

		/// <summary>把某分组的某个窗口提到前台 / 恢复（点击切换用）。</summary>
		static void activateWindow(HWND hwnd);

		/// <summary>取窗口所在的监视器矩形（全屏判定用）。</summary>
		static RECT monitorRectOf(HWND hwnd);

		/// <summary>日志回调（复用 dock 的 Log）。</summary>
		std::function<void(const std::wstring&)> onLog;

	private:
		void log(const std::wstring& s);

		/// <summary>处理单个窗口的创建 / 显示（增量加入分组）。</summary>
		void addWindow(HWND hwnd);
		/// <summary>处理窗口销毁 / 隐藏（增量移出分组）。</summary>
		void removeWindow(HWND hwnd);
		/// <summary>标题变化 / 最小化状态变化后刷新该窗口的记录。</summary>
		void refreshWindow(HWND hwnd);

		/// <summary>这个 hwnd 是否该被跟踪（任务书 §9.4 的过滤规则）。</summary>
		bool shouldTrack(HWND hwnd) const;

		/// <summary>分组键：AUMID 优先，否则小写 exe 路径。</summary>
		std::wstring groupKeyOf(HWND hwnd, const std::wstring& exePath, const std::wstring& aumid) const;

		/// <summary>把该分组的 windows 按 lastActiveTick 倒序重排。</summary>
		void sortGroupWindows(AppGroup& g);

		/// <summary>重算全屏判定并（必要时）回调。</summary>
		void updateFullscreen(HWND fg);

		// ---- win32 ----
		static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
		static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD ev, HWND hwnd,
			LONG idObject, LONG idChild, DWORD tid, DWORD time);

		HWND msgHwnd{ nullptr };
		UINT shellHookMsg{ 0 };
		/// `TaskbarCreated` 注册消息号（explorer 重启时广播它）
		UINT taskbarCreatedMsg{ 0 };
		HWINEVENTHOOK hookMinimizeStart{ nullptr };
		HWINEVENTHOOK hookMinimizeEnd{ nullptr };
		HWINEVENTHOOK hookNameChange{ nullptr };
		/// <summary>
		/// 窗口位置 / 大小变化。
		///
		/// ⚠ 这个钩子是**为了让"全屏让位"能感知"把当前窗口全屏化"**：
		///   按 F11 / 双击标题栏 / 播放器点全屏，**前台窗口并没有换**，
		///   shell 不会发 `WINDOWACTIVATED`，于是全屏状态一直不更新、dock 不让位
		///   （用户反馈："全屏时让位打开后没发现实际效果"）。
		///   加上它之后，前台窗口一变形就重判一次全屏。
		///
		/// ⚠ 这是**事件不是轮询**（红线 4 允许 SetWinEventHook）。
		///   但它很频繁（拖窗口时每帧都发），所以回调里只对
		///   **前台窗口**做一次 GetWindowRect 级重判，不干别的。
		/// </summary>
		HWINEVENTHOOK hookLocation{ nullptr };
		/// <summary>
		/// `LOCATIONCHANGE` 的节流时间戳。
		///
		/// ⚠ 这个事件在拖动窗口 / 播放视频时每秒能来几十上百次，而前台窗口
		///   **一直**在动（动画、重绘）时空闲也会被触发 —— 每次都做
		///   `GetWindowRect` + `MonitorFromWindow` 会把空闲 CPU 抬起来
		///   （实测 0.000% → 1.09%，正好卡在阶段一那条 1% 红线上）。
		///   但"全屏了没有"是个人类尺度的事，100ms 的精度绰绰有余。
		/// </summary>
		ULONGLONG lastLocationCheck{ 0 };

		std::vector<AppGroup> groupList;
		/// hwnd -> 分组在 groupList 里的下标（事件处理要 O(1)）
		std::unordered_map<HWND, size_t> hwndToGroup;
		/// hwnd -> 窗口记录（标题 / 最小化 / 最近使用）
		std::unordered_map<HWND, TrackedWindow> windows;
		/// pid -> exe 路径缓存（任务书：别在每个事件里重查）
		std::unordered_map<DWORD, std::wstring> pidPathCache;

		HWND foreground{ nullptr };
		bool fullscreen{ false };
	};

} // namespace zdock
