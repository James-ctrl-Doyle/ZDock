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
		HWINEVENTHOOK hookMinimizeStart{ nullptr };
		HWINEVENTHOOK hookMinimizeEnd{ nullptr };
		HWINEVENTHOOK hookNameChange{ nullptr };

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
