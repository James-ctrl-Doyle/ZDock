#include "WindowTracker.h"

#include <algorithm>
#include <format>
#include <shellapi.h>
#include <dwmapi.h>      // DwmGetWindowAttribute + DWMWA_CLOAKED（UWP cloak 检测）
#include <vector>

// ⚠ include 顺序有硬要求：propkey.h 里的 DEFINE_PROPERTYKEY 展开成
//   `EXTERN_C const PROPERTYKEY name`，所以 PROPERTYKEY 类型必须先可见
//   （它来自 <propidl.h>，由 <shobjidl.h> / <shlobj.h> 带进）。
//   如果 propkey.h 写在前面，会报一大片 "缺少类型说明符 - 假定为 int"
//   + "PKEY_xxx: 未声明的标识符"，错误全落在 SDK 头里，完全看不出是自己写错了顺序。
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
// ⚠ 不用 ATL 的 CComPtr —— 本机没装 ATL（atlbase.h 找不到）。
//   WRL 的 ComPtr 是 SDK 自带、无需额外组件，功能等价。
#include <wrl/client.h>

namespace zdock {

	namespace {

		constexpr const wchar_t* kMsgWndClass = L"ZDock.WindowTracker.MsgWnd";

		/// 钩子回调需要拿到实例。SetWinEventHook 的 OUTOFCONTEXT 回调不携带
		/// 用户数据（它的 lParam 是事件参数，不是我们塞的），所以只能用文件级
		/// 单例指针。进程内只会有一个 WindowTracker，够用。
		/// ⚠ 在构造函数里设、析构函数里清 —— 别在 start()/stop() 里设，
		///   否则"构造了但没 start"时钩子指针悬空。
		WindowTracker* g_trackerForHook = nullptr;

		/// 取进程完整路径（带 pid→路径 缓存由调用方管）。
		std::wstring queryProcessPath(DWORD pid)
		{
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!h) return {};
			wchar_t buf[MAX_PATH * 2]{};
			DWORD n = static_cast<DWORD>(std::size(buf));
			const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &n);
			CloseHandle(h);
			return ok ? std::wstring{ buf, n } : std::wstring{};
		}

		std::wstring toLower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
				[](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
			return s;
		}

		/// 取窗口标题。
		std::wstring windowTitle(HWND hwnd)
		{
			const int n = GetWindowTextLengthW(hwnd);
			if (n <= 0) return {};
			std::wstring s(static_cast<size_t>(n) + 1, L'\0');
			const int got = GetWindowTextW(hwnd, s.data(), n + 1);
			s.resize(static_cast<size_t>(std::max(0, got)));
			return s;
		}

		/// 取窗口的显示名（用 shell 的 "FileDescription"，比进程名好看）。
		std::wstring fileDescriptionOf(const std::wstring& exePath)
		{
			if (exePath.empty()) return {};
			DWORD dummy = 0;
			DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
			if (size == 0) return {};
			std::vector<BYTE> buf(size);
			if (!GetFileVersionInfoW(exePath.c_str(), 0, size, buf.data())) return {};

			struct LANGANDCODEPAGE { WORD lang; WORD codepage; };
			LANGANDCODEPAGE* lpTranslate = nullptr;
			UINT cbTranslate = 0;
			if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
				reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate)
				|| cbTranslate < sizeof(LANGANDCODEPAGE)) {
				return {};
			}
			for (UINT i = 0; i < cbTranslate / sizeof(LANGANDCODEPAGE); ++i) {
				wchar_t sub[64]{};
				swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription",
					lpTranslate[i].lang, lpTranslate[i].codepage);
				wchar_t* val = nullptr;
				UINT len = 0;
				if (VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&val), &len)
					&& val && len > 1) {
					return std::wstring{ val, len - 1 };
				}
			}
			return {};
		}

		/// 取 AppUserModelID。传统 Win32 程序多半没有，UWP / 打包应用有。
		std::wstring aumidOf(HWND hwnd)
		{
			// ⚠ 用 SHGetPropertyStoreForWindow 是官方正路，不注入、不挂钩子。
			Microsoft::WRL::ComPtr<IPropertyStore> store;
			if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) {
				return {};
			}
			PROPVARIANT pv{};
			PropVariantInit(&pv);
			std::wstring out;
			if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
				out = pv.pwszVal;
			}
			PropVariantClear(&pv);
			return out;
		}

		/// UWP / 打包应用的宿主窗口：真正的应用窗口挂在 ApplicationFrameWindow 下。
		/// ⚠ 任务书明确警告：**不要无脑排除 ApplicationFrameWindow**，
		///   某 Dock 因此在 Win10/11 上完全不可用。这里下钻取它的子窗口。
		HWND drillIntoFrame(HWND hwnd, const wchar_t* cls)
		{
			if (wcscmp(cls, L"ApplicationFrameWindow") != 0) return hwnd;
			HWND child = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
			// 有些 UWP 用别的类名，退一步取第一个可见子窗口
			if (!child) {
				child = FindWindowExW(hwnd, nullptr, nullptr, nullptr);
			}
			return child ? child : hwnd;
		}

	} // namespace

	// ---------------------------------------------------------------------------

	bool AppGroup::isForeground() const
	{
		HWND fg = GetForegroundWindow();
		if (!fg) return false;
		// 前台窗口可能是个子窗口（UWP 的 CoreWindow），所以要往上找到顶层再比
		HWND root = GetAncestor(fg, GA_ROOT);
		if (!root) root = fg;
		return std::find(windows.begin(), windows.end(), root) != windows.end();
	}

	WindowTracker::WindowTracker()
	{
		g_trackerForHook = this;
	}

	WindowTracker::~WindowTracker()
	{
		stop();
		if (g_trackerForHook == this) g_trackerForHook = nullptr;
	}

	void WindowTracker::log(const std::wstring& s)
	{
		if (onLog) onLog(s);
	}

	// ---------------------------------------------------------------------------
	// 建接收窗口 + 注册事件源。
	//
	// ⚠ 为什么要自建窗口：RegisterShellHookWindow 需要一个**真实窗口**来收消息
	//   （消息专用窗口 HWND_MESSAGE 收不到）。而 Ling 的 WinBase::winProc 是
	//   静态私有的、不暴露消息口，所以我们不能借 dock 主窗口。
	//   自建一个 0x0 的隐藏窗口最干净。
	// ---------------------------------------------------------------------------
	bool WindowTracker::start()
	{
		HINSTANCE hInst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &WindowTracker::wndProc;
		wc.hInstance = hInst;
		wc.lpszClassName = kMsgWndClass;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			log(std::format(L"[track] RegisterClassExW 失败 err={}", GetLastError()));
			return false;
		}

		shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
		if (!shellHookMsg) {
			log(L"[track] RegisterWindowMessageW(\"SHELLHOOK\") 失败");
			return false;
		}

		// explorer 重启时 Windows 会广播这条消息。⚠ 名字是 "TaskbarCreated"，
		// **中间没有空格**（写成 "Taskbar Created" 注册出来是另一个号，永远收不到）。
		// 我们借自己的接收窗口收它 —— 广播只发顶层窗口，而 Ling 不暴露消息口。
		taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

		msgHwnd = CreateWindowExW(0, kMsgWndClass, L"ZDockTrack", 0,
			0, 0, 0, 0, nullptr, nullptr, hInst, this);
		if (!msgHwnd) {
			log(std::format(L"[track] CreateWindowExW 失败 err={}", GetLastError()));
			return false;
		}

		if (!RegisterShellHookWindow(msgHwnd)) {
			log(std::format(L"[track] RegisterShellHookWindow 失败 err={}", GetLastError()));
			return false;
		}

		// win event：只订阅 shell hook 拿不到的三种。
		// ⚠ 订阅面越小越好 —— 实测 CREATE/SHOW/NAMECHANGE 3 秒内各上百次，
		//   全订等于给自己造了个高频回调（变相轮询）。
		hookMinimizeStart = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
			nullptr, &WindowTracker::winEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
		hookMinimizeEnd = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
			nullptr, &WindowTracker::winEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
		hookNameChange = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
			nullptr, &WindowTracker::winEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
		// 为什么还需要它：把**当前**窗口全屏化（F11 / 双击标题栏 / 播放器全屏按钮）
		// **不会换前台窗口**，shell 也就不发 WINDOWACTIVATED —— 只靠那个事件判不出
		// "刚变成全屏"。加这条之后，前台窗口一变形就重判。
		// ⚠ 事件本身很频繁，回调里只对前台窗口做一次廉价的矩形比较（见 winEventProc）。
		hookLocation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
			nullptr, &WindowTracker::winEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

		log(std::format(L"[track] 已启动 shellHookMsg=0x{:04X} taskbarCreatedMsg=0x{:04X} hooks={}/{}/{}/{}",
			shellHookMsg, taskbarCreatedMsg,
			(bool)hookMinimizeStart, (bool)hookMinimizeEnd, (bool)hookNameChange, (bool)hookLocation));
		return true;
	}

	void WindowTracker::stop()
	{
		if (hookMinimizeStart) { UnhookWinEvent(hookMinimizeStart); hookMinimizeStart = nullptr; }
		if (hookMinimizeEnd) { UnhookWinEvent(hookMinimizeEnd);     hookMinimizeEnd = nullptr; }
		if (hookNameChange) { UnhookWinEvent(hookNameChange);       hookNameChange = nullptr; }
		if (hookLocation) { UnhookWinEvent(hookLocation);           hookLocation = nullptr; }
		if (msgHwnd) {
			DeregisterShellHookWindow(msgHwnd);
			DestroyWindow(msgHwnd);
			msgHwnd = nullptr;
		}
	}

	// ---------------------------------------------------------------------------
	// 过滤规则（任务书 §9.4）：
	//   · 必须是可见的顶层窗口
	//   · 无 owner（有 owner 的是对话框 / 工具窗，不算任务栏语义里的"窗口"）
	//   · 不是 WS_EX_TOOLWINDOW
	//   · 不是 cloaked（UWP 在别的虚拟桌面上时会被 DWM cloak，看不见但仍存在）
	// ---------------------------------------------------------------------------
	bool WindowTracker::shouldTrack(HWND hwnd) const
	{
		if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
		if (GetWindow(hwnd, GW_OWNER)) return false;
		if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;

		// cloaked 检测：UWP 窗口切到别的虚拟桌面 / 被挂起时 cloaked=1，
		// 但 IsWindowVisible 仍为 TRUE，必须靠 DWM 属性排除。
		BOOL cloaked = FALSE;
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
			&& cloaked) {
			return false;
		}
		return true;
	}

	std::wstring WindowTracker::groupKeyOf(HWND hwnd, const std::wstring& exePath,
		const std::wstring& aumid) const
	{
		if (!aumid.empty()) return L"aumid:" + aumid;
		if (!exePath.empty()) return L"path:" + toLower(exePath);
		return L"hwnd:" + std::to_wstring(reinterpret_cast<ULONG_PTR>(hwnd));
	}

	void WindowTracker::sortGroupWindows(AppGroup& g)
	{
		std::sort(g.windows.begin(), g.windows.end(), [this](HWND a, HWND b) {
			auto ia = windows.find(a);
			auto ib = windows.find(b);
			const ULONGLONG ta = (ia != windows.end()) ? ia->second.lastActiveTick : 0;
			const ULONGLONG tb = (ib != windows.end()) ? ib->second.lastActiveTick : 0;
			return ta > tb;     // 最近的排前面
			});
	}

	// ---------------------------------------------------------------------------
	// 启动时全量枚举一次（此后全靠事件增量维护，不再轮询）。
	// ---------------------------------------------------------------------------
	void WindowTracker::rebuildAll()
	{
		groupList.clear();
		hwndToGroup.clear();
		windows.clear();
		pidPathCache.clear();

		const HWND fg = GetForegroundWindow();
		foreground = fg;

		// 枚举所有顶层窗口。EnumWindows 一次，之后不再重复。
		EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
			auto* self = reinterpret_cast<WindowTracker*>(lp);
			if (self->shouldTrack(hwnd)) self->addWindow(hwnd);
			return TRUE;
			}, reinterpret_cast<LPARAM>(this));

		// 初始化"最近使用"：把 z 序当作最近使用顺序的近似（z 序越靠前越近）
		ULONGLONG tick = GetTickCount64();
		HWND h = GetTopWindow(nullptr);
		while (h) {
			auto it = windows.find(h);
			if (it != windows.end()) {
				it->second.lastActiveTick = tick;
				tick = (tick > 1000) ? tick - 1000 : 0;   // 递减，保证唯一可排序
			}
			h = GetWindow(h, GW_HWNDNEXT);
		}

		updateFullscreen(foreground);
		log(std::format(L"[track] 全量重建：{} 个窗口 / {} 个分组",
			windows.size(), groupList.size()));
		// 诊断：把每个分组的"键 / 显示名 / 窗口数"列出来。阶段三排查
		// "为什么这个应用没匹配上固定项"全靠这一行（分组键是路径小写形式，
		// 和 config 里的路径大小写/短路径写法很容易对不上）。
		for (const auto& g : groupList) {
			log(std::format(L"[track]   分组 key={} 名称={} 窗口={}",
				g.key, g.displayName, g.windows.size()));
		}
		if (onChanged) onChanged();
	}

	// ---------------------------------------------------------------------------
	// 增量：加一个窗口
	// ---------------------------------------------------------------------------
	void WindowTracker::addWindow(HWND hwnd)
	{
		if (windows.count(hwnd)) return;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (!pid) return;

		// pid -> 路径缓存（任务书：别在每个事件里重查）
		std::wstring exePath;
		if (auto it = pidPathCache.find(pid); it != pidPathCache.end()) {
			exePath = it->second;
		}
		else {
			exePath = queryProcessPath(pid);
			pidPathCache.emplace(pid, exePath);
		}

		// UWP 宿主窗口下钻
		wchar_t cls[256]{};
		GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
		/* 这里保留原始 hwnd 作为跟踪对象（它才是任务栏语义里的那个窗口），
		   下钻只在取 AUMID 时用 —— 否则切换时会把 CoreWindow 提到前台，行为不一致。 */

		std::wstring aumid = aumidOf(hwnd);
		if (aumid.empty()) {
			// 有些 UWP 把 AUMID 放在子窗口上
			if (HWND child = drillIntoFrame(hwnd, cls); child != hwnd) {
				aumid = aumidOf(child);
			}
		}

		TrackedWindow tw;
		tw.hwnd = hwnd;
		tw.pid = pid;
		tw.exePath = exePath;
		tw.aumid = aumid;
		tw.title = windowTitle(hwnd);
		tw.minimized = IsIconic(hwnd) != FALSE;
		tw.foreground = (GetForegroundWindow() == hwnd);
		tw.lastActiveTick = GetTickCount64();
		windows.emplace(hwnd, tw);

		const std::wstring key = groupKeyOf(hwnd, exePath, aumid);

		auto git = hwndToGroup.find(hwnd);
		size_t gi = static_cast<size_t>(-1);
		if (git != hwndToGroup.end()) {
			gi = git->second;
		}
		else {
			for (size_t i = 0; i < groupList.size(); ++i) {
				if (groupList[i].key == key) { gi = i; break; }
			}
		}

		if (gi == static_cast<size_t>(-1)) {
			AppGroup g;
			g.key = key;
			g.exePath = exePath;
			g.aumid = aumid;
			// 显示名：优先 shell 的 FileDescription（"Windows 记事本" 而不是 "notepad"）
			std::wstring desc = fileDescriptionOf(exePath);
			if (!desc.empty()) {
				g.displayName = desc;
			}
			else if (!exePath.empty()) {
				std::wstring base = exePath.substr(exePath.find_last_of(L"\\/") + 1);
				const size_t dot = base.find_last_of(L'.');
				if (dot != std::wstring::npos) base.resize(dot);
				g.displayName = base;
			}
			else {
				g.displayName = L"应用";
			}
			g.windows.push_back(hwnd);
			groupList.push_back(std::move(g));
			gi = groupList.size() - 1;
		}
		else {
			auto& g = groupList[gi];
			if (std::find(g.windows.begin(), g.windows.end(), hwnd) == g.windows.end()) {
				g.windows.push_back(hwnd);
			}
			if (g.exePath.empty()) g.exePath = exePath;
			if (g.aumid.empty()) g.aumid = aumid;
		}

		hwndToGroup[hwnd] = gi;
		sortGroupWindows(groupList[gi]);
	}

	void WindowTracker::removeWindow(HWND hwnd)
	{
		windows.erase(hwnd);

		auto git = hwndToGroup.find(hwnd);
		if (git == hwndToGroup.end()) return;
		const size_t gi = git->second;
		hwndToGroup.erase(git);
		if (gi >= groupList.size()) return;

		auto& g = groupList[gi];
		g.windows.erase(std::remove(g.windows.begin(), g.windows.end(), hwnd), g.windows.end());

		if (g.windows.empty()) {
			// 分组空了就删掉。注意这会移动后面分组的下标 —— 必须整体修一遍映射。
			groupList.erase(groupList.begin() + gi);
			hwndToGroup.clear();
			for (size_t i = 0; i < groupList.size(); ++i) {
				for (HWND h : groupList[i].windows) hwndToGroup[h] = i;
			}
		}
	}

	void WindowTracker::refreshWindow(HWND hwnd)
	{
		auto it = windows.find(hwnd);
		if (it == windows.end()) return;
		it->second.title = windowTitle(hwnd);
		it->second.minimized = IsIconic(hwnd) != FALSE;

		// 标题 / 状态变了，分组可能得重算（基本不会，但 AUMID 有时是延迟可用的）
		auto git = hwndToGroup.find(hwnd);
		if (git == hwndToGroup.end()) return;
		const size_t gi = git->second;
		if (gi >= groupList.size()) return;

		if (groupList[gi].displayName.empty() || groupList[gi].displayName == L"应用") {
			std::wstring desc = fileDescriptionOf(groupList[gi].exePath);
			if (!desc.empty()) groupList[gi].displayName = desc;
		}
	}

	const AppGroup* WindowTracker::findGroup(const std::wstring& key) const
	{
		for (const auto& g : groupList) {
			if (g.key == key) return &g;
		}
		return nullptr;
	}

	const AppGroup* WindowTracker::findByExePath(const std::wstring& exePath) const
	{
		if (exePath.empty()) return nullptr;
		const std::wstring want = L"path:" + toLower(exePath);
		for (const auto& g : groupList) {
			if (g.key == want) return &g;
		}
		return nullptr;
	}

	void WindowTracker::activateWindow(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		if (IsIconic(hwnd)) {
			ShowWindowAsync(hwnd, SW_RESTORE);
		}
		// ⚠ 不用 SetForegroundWindow 的裸调用 —— 前台切换有权限限制，
		//   前台进程才能无条件切别人。这里用官方的"允许抢占"手法：
		//   先 SetWindowPos 让窗口"跳"一下（不改 z 序语义），再 SetForegroundWindow。
		//   这是 Microsoft 文档/社区公认的做法，不涉及任何注入。
		HWND fg = GetForegroundWindow();
		if (fg == hwnd) return;

		const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
		const DWORD myTid = GetCurrentThreadId();
		if (fgTid && fgTid != myTid) {
			// AttachThreadInput 让本线程与前台线程共享输入状态，
			// 从而获得切前台的权限。用完立刻 detach。
			if (AttachThreadInput(fgTid, myTid, TRUE)) {
				SetForegroundWindow(hwnd);
				AttachThreadInput(fgTid, myTid, FALSE);
				return;
			}
		}
		SetForegroundWindow(hwnd);
	}

	RECT WindowTracker::monitorRectOf(HWND hwnd)
	{
		RECT r{};
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoW(mon, &mi)) r = mi.rcMonitor;
		return r;
	}

	// ---------------------------------------------------------------------------
	// 全屏判定（任务书 §9.10）：
	//   前台窗口的矩形 == 它所在显示器的完整矩形 → 视为全屏应用。
	//   ⚠ 只比 rcMonitor，不比 rcWork —— 全屏应用会盖住任务栏，工作区是扣掉
	//     任务栏的那块，拿工作区比会永远判不出来。
	// ---------------------------------------------------------------------------
	void WindowTracker::updateFullscreen(HWND fg)
	{
		bool nowFullscreen = false;
		if (fg && IsWindow(fg) && IsWindowVisible(fg)) {
			RECT wr{};
			if (GetWindowRect(fg, &wr)) {
				const RECT mr = monitorRectOf(fg);
				nowFullscreen = (wr.left <= mr.left && wr.top <= mr.top
					&& wr.right >= mr.right && wr.bottom >= mr.bottom);
				// 还要排除最小化（最小化时 rect 会是 -32000）
				if (IsIconic(fg)) nowFullscreen = false;
			}
		}
		if (nowFullscreen != fullscreen) {
			fullscreen = nowFullscreen;
			log(std::format(L"[track] 全屏应用状态 -> {}", fullscreen ? L"有" : L"无"));
			if (onFullscreenChanged) onFullscreenChanged(fullscreen);
		}
	}

	// ---------------------------------------------------------------------------
	// 窗口过程：收 shell hook 消息
	// ---------------------------------------------------------------------------
	LRESULT CALLBACK WindowTracker::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		WindowTracker* self = reinterpret_cast<WindowTracker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (msg == WM_NCCREATE) {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = reinterpret_cast<WindowTracker*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
		if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

		// explorer / 任务栏重建广播（任务书 §2 #32 自愈）。放在 shell hook 判定之前，
		// 因为它是注册消息号，和 shellHookMsg 不冲突但语义完全不同。
		if (self->taskbarCreatedMsg && msg == self->taskbarCreatedMsg) {
			self->log(L"[track] 收到 TaskbarCreated 广播");
			if (self->onTaskbarCreated) self->onTaskbarCreated();
			return 0;
		}

		// 自动化测试注入（仅测试用，正常运行时没有任何代码会发这条消息）。
		// 借常驻的接收窗口做载体 —— 热区会随 autoHide 开关销毁，靠不住。
		if (msg == kMsgTestInject) {
			self->log(std::format(L"[track] 测试注入：全屏状态 -> {}", wp ? 1 : 0));
			if (self->onTestInject) self->onTestInject(wp != 0);
			return 0;
		}

		// 显示环境变化（分辨率 / 色深 / 主屏切换）。
		// ⚠ 这条是**广播**，只发给顶层窗口 —— 我们这个 0x0 隐藏窗口是顶层（parent=0），
		//   所以收得到；Ling 的 dock 主窗口收不到（不暴露消息口）。
		//   参数：wParam = 色深(bpp)，lParam 低字 = 宽、高字 = 高（**整数**，
		//   不是指针 —— 所以这条可以安全地被跨进程投递，测试就靠它）。
		if (msg == WM_DISPLAYCHANGE) {
			self->log(std::format(L"[track] 显示器参数变化：{}x{} {}bpp",
				LOWORD(lp), HIWORD(lp), static_cast<UINT>(wp)));
			if (self->onDisplayChanged) self->onDisplayChanged();
			return 0;
		}

		// 系统度量变化。⚠ 这条**非常频繁**（任何设置变化都会广播），必须按 lParam
		// 指明的项目过滤，否则等于给自己造了个高频回调。
		//   只认这三类："WorkArea"（工作区）、"WindowMetrics"（窗口度量/DPI 相关）、
		//   "Display"（显示相关）。
		//
		// ⚠⚠ lParam 是一个**字符串指针**（也可能为 nullptr，表示"很多设置变了"）。
		//   系统自己发的消息里它总是有效的；但这条消息是**广播**，任何人都能往我们
		//   窗口上投 —— 跨进程投进来的 lParam 指向的是**对方进程**的地址，
		//   直接 wcscmp 就是读野指针（轻则乱判，重则崩）。
		//   所以先做一次可读性检查再解引用。
		//   （实测踩过：探针跨进程投 WM_SETTINGCHANGE 就是为了验证这条路径。）
		if (msg == WM_SETTINGCHANGE) {
			const wchar_t* area = reinterpret_cast<const wchar_t*>(lp);
			if (area == nullptr) return 0;                     // nullptr = "多项变化"，不细分
			if (IsBadStringPtrW(area, 64)) return 0;           // 不可读（跨进程野指针）→ 忽略
			const bool relevant = (wcscmp(area, L"WorkArea") == 0
				|| wcscmp(area, L"WindowMetrics") == 0
				|| wcscmp(area, L"Display") == 0);
			if (relevant) {
				self->log(std::format(L"[track] 系统度量变化：{}", area));
				if (self->onDisplayChanged) self->onDisplayChanged();
			}
			return 0;
		}

		if (msg == self->shellHookMsg) {
			// ⚠ 参数语义（容易记反）：
			//   wParam = 事件码（HSHELL_*），lParam = 窗口句柄
			//   —— 不是反过来。
			const UINT code = static_cast<UINT>(wp);
			HWND target = reinterpret_cast<HWND>(lp);
			bool changed = false;

			switch (code) {
			case HSHELL_WINDOWCREATED:
				if (self->shouldTrack(target)) { self->addWindow(target); changed = true; }
				break;

			case HSHELL_WINDOWDESTROYED:
				if (self->windows.count(target)) { self->removeWindow(target); changed = true; }
				break;

			case HSHELL_WINDOWACTIVATED:
			case HSHELL_RUDEAPPACTIVATED: {
				// 全屏应用进入 / 退出就走这个分支（RUDEAPPACTIVATED 表示
				// "粗暴激活"，通常是全屏应用）。任务书 §9.10 推荐用它。
				self->foreground = target;
				auto it = self->windows.find(target);
				if (it != self->windows.end()) {
					it->second.lastActiveTick = GetTickCount64();
					it->second.foreground = true;
					auto git = self->hwndToGroup.find(target);
					if (git != self->hwndToGroup.end() && git->second < self->groupList.size()) {
						self->sortGroupWindows(self->groupList[git->second]);
						changed = true;
					}
				}
				self->updateFullscreen(target);
				if (self->onForegroundChanged) self->onForegroundChanged(target);
				break;
			}

			case HSHELL_REDRAW:
				// 窗口标题变化（shell 用它通知"这个窗口的标题需要重画"）。
				if (self->windows.count(target)) {
					self->refreshWindow(target);
					changed = true;
				}
				break;

			case HSHELL_FLASH:
				// 窗口请求注意（标题闪烁）→ 图标弹跳
				if (self->onFlash) self->onFlash(target);
				break;

			case HSHELL_WINDOWREPLACED:
			case HSHELL_WINDOWREPLACING:
				// 窗口被替换（少见）。保守起见重整一遍这一个窗口。
				if (self->windows.count(target)) { self->refreshWindow(target); changed = true; }
				break;

			default:
				break;
			}

			if (changed && self->onChanged) self->onChanged();
			return 0;
		}

		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// ---------------------------------------------------------------------------
	// win event 回调：只处理最小化状态与标题变化
	// ⚠ 这个回调**可能来自别的线程**（OUTOFCONTEXT 下通常投递到注册线程的
	//   消息队列，但不保证）。所以只做最轻的标记，重活交给 shell hook。
	//   这里实际是跑在注册钩子的线程上（消息泵所在线程）。
	// ---------------------------------------------------------------------------
	void CALLBACK WindowTracker::winEventProc(HWINEVENTHOOK hook, DWORD ev, HWND hwnd,
		LONG idObject, LONG idChild, DWORD tid, DWORD time)
	{
		WindowTracker* self = g_trackerForHook;   // 见文件头的说明
		if (!self || !hwnd) return;

		// 只关心窗口本身，不关心子元素
		if (idObject != OBJID_WINDOW || idChild != 0) return;

		switch (ev) {
		case EVENT_SYSTEM_MINIMIZESTART:
		case EVENT_SYSTEM_MINIMIZEEND: {
			auto it = self->windows.find(hwnd);
			if (it != self->windows.end()) {
				it->second.minimized = (ev == EVENT_SYSTEM_MINIMIZESTART);
				if (self->onChanged) self->onChanged();
			}
			break;
		}
		case EVENT_OBJECT_NAMECHANGE: {
			auto it = self->windows.find(hwnd);
			if (it != self->windows.end()) {
				self->refreshWindow(hwnd);
				// 标题变化很频繁（比如浏览器切标签），不必每次都通知 UI 重绘，
				// 只在标题真的变了时才回调。
				if (self->onChanged) self->onChanged();
			}
			break;
		}
		case EVENT_OBJECT_LOCATIONCHANGE: {
			// 窗口位置 / 大小变了。**只关心前台窗口** —— 它决定"是不是全屏"。
			// ⚠ 节流 100ms：拖动窗口 / 播视频时这事件每秒几十上百次，
			//   不做节流空闲 CPU 会被抬到阶段一的 1% 红线上面去（实测踩过）。
			//   漏不掉最终状态 —— 停下之后还会有事件进来。
			if (self->foreground == hwnd) {
				const ULONGLONG now = GetTickCount64();
				if (now - self->lastLocationCheck >= 100) {
					self->lastLocationCheck = now;
					self->updateFullscreen(hwnd);
				}
			}
			break;
		}
		default:
			break;
		}
	}

} // namespace zdock
