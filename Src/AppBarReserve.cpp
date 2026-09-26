#include "AppBarReserve.h"

#include <format>
#include <fstream>
#include <shellapi.h>   // SHAppBarMessage / ABM_*

namespace zdock {

	namespace {
		constexpr const wchar_t* kCbClass = L"ZDock.AppBarReserve.Cb";

		/// 把 RECT 写成 "l,t,r,b" 存盘 / 读回。用文本最省事，也不怕版本变化。
		bool writeRectFile(const std::wstring& path, const RECT& r)
		{
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if (!f) return false;
			f << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom;
			return static_cast<bool>(f);
		}

		bool readRectFile(const std::wstring& path, RECT& out)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			LONG l = 0, t = 0, rr = 0, b = 0;
			if (!(f >> l >> t >> rr >> b)) return false;
			out = RECT{ l, t, rr, b };
			return true;
		}

		void deleteFileQuiet(const std::wstring& path)
		{
			DeleteFileW(path.c_str());
		}
	}

	AppBarReserve::~AppBarReserve()
	{
		unregister_();
	}

	// ---------------------------------------------------------------------------
	// 强杀后的工作区自愈（红线 7）。
	//
	// 状态文件里存的是"我们抢占**之前**的干净工作区"。它还在 = 上次没正常退出。
	// ⚠ 触发条件是"文件存在"，所以正常退出路径必须确保文件被删掉
	//   （见 unregister_ + DockWin 的析构）。
	//
	// ⚠⚠ 返回值语义（实测逼出来的）：清掉工作区数值**不等于**清掉了 shell 里
	//   那条死 AppBar 记录。死记录活在 Shell_TrayWnd 的内存态里，只有重启
	//   explorer 才会消失。本次若照常注册 AppBar，shell 会算成"残留 + 新"，
	//   工作区被占**两道**（实测干净底 1440 → 1257，两层 95px）。
	//   所以这里返回 true 让调用方**跳过本次注册**：宁可这次不预留，
	//   也不能给用户留一个 183px 的洞。
	// ---------------------------------------------------------------------------
	bool AppBarReserve::recoverStaleWorkArea(const std::function<void(const std::wstring&)>& logFn,
		const std::wstring& stateFilePath)
	{
		auto say = [&](const std::wstring& s) { if (logFn) logFn(s); };

		RECT saved{};
		if (!readRectFile(stateFilePath, saved)) return false;   // 没有残迹 → 什么都不做

		RECT cur{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &cur, 0);
		deleteFileQuiet(stateFilePath);

		// 1) 把工作区数值写回"我们抢占前记的干净值"。
		if (cur.left == saved.left && cur.top == saved.top
			&& cur.right == saved.right && cur.bottom == saved.bottom) {
			say(L"[appbar] 发现上次的状态文件，但工作区数值已正常（shell 里可能仍有死记录）");
		}
		else if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &saved, SPIF_SENDCHANGE)) {
			say(std::format(L"[appbar] 强杀自愈：工作区从 ({},{})-({},{}) 恢复到 ({},{})-({},{})",
				cur.left, cur.top, cur.right, cur.bottom,
				saved.left, saved.top, saved.right, saved.bottom));
		}
		else {
			say(std::format(L"[appbar] 强杀自愈：SPI_SETWORKAREA 失败 err={}", GetLastError()));
		}

		// 2) ⚠ 光改数值没用 —— shell 里那条死 AppBar 记录还在，再注册会叠加。
		//    跑一次完整 NEW/SETPOS(空)/REMOVE 循环把死记录扫掉。
		purgeStaleRecord(logFn);

		if (saved.bottom > 0) {
			RECT now{};
			SystemParametersInfoW(SPI_GETWORKAREA, 0, &now, 0);
			say(std::format(L"[appbar] 自愈完成，当前工作区 ({},{})-({},{})",
				now.left, now.top, now.right, now.bottom));
		}
		return true;
	}

	// ---------------------------------------------------------------------------
	// 扫掉 shell 里残留的死 AppBar 记录。
	//
	// ⚠ 实测（`build-support/_probe_stale_lifecycle.py`）：
	//   死记录不是"永久"的 —— 它在下一次**完整的** NEW → SETPOS(空) → REMOVE
	//   循环里会被 shell 顺带清掉（实测 eaten 从 183px 掉回 0~1px）。
	//   而只做 NEW + SETPOS(真位置)（= 我们正常注册的路径）**清不掉**它。
	//   所以这里专门跑一遍完整循环：不为自己要空间（SETPOS 是空矩形），
	//   纯粹是逼 shell 重扫一遍 AppBar 列表、发现老那条的宿主窗口已经没了。
	// ---------------------------------------------------------------------------
	void AppBarReserve::purgeStaleRecord(const std::function<void(const std::wstring&)>& logFn)
	{
		auto say = [&](const std::wstring& s) { if (logFn) logFn(s); };

		HINSTANCE hInst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = hInst;
		wc.lpszClassName = L"ZDock.AppBarReserve.Purge";
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			say(std::format(L"[appbar] 清残留：RegisterClassExW 失败 err={}", GetLastError()));
			return;
		}
		HWND h = CreateWindowExW(0, L"ZDock.AppBarReserve.Purge", L"ZDockPurge", 0,
			0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
		if (!h) {
			say(std::format(L"[appbar] 清残留：建窗口失败 err={}", GetLastError()));
			return;
		}

		APPBARDATA abd{};
		abd.cbSize = sizeof(abd);
		abd.hWnd = h;
		abd.uCallbackMessage = RegisterWindowMessageW(L"ZDockAppBarPurgeNotify");
		// ⚠ SHAppBarMessage 返回 UINT_PTR，别塞进 BOOL（C4244，可能丢数据）
		const UINT_PTR okNew = SHAppBarMessage(ABM_NEW, &abd);

		APPBARDATA empty{};
		empty.cbSize = sizeof(empty);
		empty.hWnd = h;
		empty.uEdge = ABE_BOTTOM;
		empty.rc = RECT{ 0, 0, 0, 0 };
		SHAppBarMessage(ABM_SETPOS, &empty);

		APPBARDATA rm{};
		rm.cbSize = sizeof(rm);
		rm.hWnd = h;
		SHAppBarMessage(ABM_REMOVE, &rm);
		DestroyWindow(h);

		say(std::format(L"[appbar] 清残留：NEW/SETPOS(空)/REMOVE 循环完成（NEW={}）", okNew ? 1 : 0));
	}

	bool AppBarReserve::register_(HWND owner, const RECT& desired, const std::wstring& stateFilePath,
		UINT edge)
	{
		unregister_();
		ownerHwnd = owner;

		HINSTANCE hInst = GetModuleHandleW(nullptr);

		// ---- 回调窗口（收 ABN_POSCHANGED 等通知）----
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &AppBarReserve::wndProc;
		wc.hInstance = hInst;
		wc.lpszClassName = kCbClass;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			log(std::format(L"[appbar] RegisterClassExW 失败 err={}", GetLastError()));
			return false;
		}

		// ⚠ AppBar 的回调窗口必须是**真实窗口**（消息专用窗口收不到 ABN_*）。
		//   和 WindowTracker 一样建个 0x0 隐藏窗口。
		cbHwnd = CreateWindowExW(0, kCbClass, L"ZDockAppBar", 0,
			0, 0, 0, 0, nullptr, nullptr, hInst, this);
		if (!cbHwnd) {
			log(std::format(L"[appbar] 回调窗口创建失败 err={}", GetLastError()));
			return false;
		}

		// ---- 先记下"抢占前的工作区"，供强杀后自愈 ----
		// ⚠ 必须在 ABM_NEW 之前读 —— 之后就已经被我们改过了。
		//   这个文件"存在"本身就等于"上次没正常退出"的标记。
		statePath = stateFilePath;
		if (!stateFilePath.empty()) {
			RECT before{};
			SystemParametersInfoW(SPI_GETWORKAREA, 0, &before, 0);
			if (writeRectFile(stateFilePath, before)) {
				log(std::format(L"[appbar] 已记录抢占前工作区 ({},{})-({},{})",
					before.left, before.top, before.right, before.bottom));
			}
		}

		// ---- ABM_NEW：登记成一个 AppBar ----
		APPBARDATA abd{};
		abd.cbSize = sizeof(abd);
		abd.hWnd = cbHwnd;
		abd.uCallbackMessage = cbMsg = RegisterWindowMessageW(L"ZDockAppBarNotify");
		if (!SHAppBarMessage(ABM_NEW, &abd)) {
			log(L"[appbar] ABM_NEW 失败 —— 保持不预留工作区");
			DestroyWindow(cbHwnd);
			cbHwnd = nullptr;
			return false;
		}
		registeredFlag = true;

		// ---- ABM_QUERYPOS → ABM_SETPOS ----
		// 先问系统"这个位置行不行"（别的 AppBar 可能已经占了这块），
		// 拿到它挤过的位置再 SETPOS。这是标准两步，缺了 QUERYPOS 会和
		// 系统任务栏 / 其他 AppBar 抢同一块空间。
		RECT proposed = desired;
		abd.uEdge = edge;
		abd.rc = proposed;
		SHAppBarMessage(ABM_QUERYPOS, &abd);

		// ⚠ 任务栏在底部时，系统可能把我们的下边界往上推。
		//   这里**认系统给的 rc**，但保留自己算的厚度（高度），
		//   否则会被压成 0 高。厚度 = 原始 desired 的高度。
		const LONG thick = desired.bottom - desired.top;
		switch (edge) {
		case ABE_BOTTOM: abd.rc.top = abd.rc.bottom - thick; break;
		case ABE_TOP:    abd.rc.bottom = abd.rc.top + thick; break;
		case ABE_LEFT:   abd.rc.right = abd.rc.left + (desired.right - desired.left); break;
		case ABE_RIGHT:  abd.rc.left = abd.rc.right - (desired.right - desired.left); break;
		default: break;
		}

		if (!SHAppBarMessage(ABM_SETPOS, &abd)) {
			log(L"[appbar] ABM_SETPOS 失败 —— 撤销注册");
			unregister_();
			return false;
		}

		lastApproved = abd.rc;
		log(std::format(L"[appbar] 已注册 edge={} 批准矩形=({},{})-({},{})",
			edge, abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom));
		return true;
	}

	void AppBarReserve::unregister_()
	{
		if (registeredFlag && cbHwnd) {
			// ⚠ 顺序很关键，实测得出的（见 build-support/_probe_appbar_cleanup.py）：
			//   1) **先 ABM_SETPOS 成一个空矩形（厚度 0）** —— 这条会让 shell 立刻
			//      重算工作区，把我们的占用吐回去。
			//   2) 再 ABM_REMOVE 注销。
			//   如果只做 ABM_REMOVE，在**某些路径下**（比如它失败、或者消息没到
			//   Shell_TrayWnd）工作区会留在缩进状态，用户看着像"任务栏变高了"。
			//   先 SETPOS 空矩形相当于"先松手再摘牌"，两种失败模式都能兜住。
			APPBARDATA empty{};
			empty.cbSize = sizeof(empty);
			empty.hWnd = cbHwnd;
			empty.uEdge = ABE_BOTTOM;
			empty.rc = RECT{ 0, 0, 0, 0 };
			SHAppBarMessage(ABM_SETPOS, &empty);

			APPBARDATA abd{};
			abd.cbSize = sizeof(abd);
			abd.hWnd = cbHwnd;
			SHAppBarMessage(ABM_REMOVE, &abd);
			registeredFlag = false;
			log(L"[appbar] 已注销（先 SETPOS 空矩形再 REMOVE，工作区恢复）");
		}
		// ⚠ 状态文件必须清掉：它还留着就等于"上次没正常退出"，
		//   下次启动会误判成需要自愈。
		if (!statePath.empty()) {
			DeleteFileW(statePath.c_str());
			statePath.clear();
		}
		if (cbHwnd) {
			DestroyWindow(cbHwnd);
			cbHwnd = nullptr;
		}
		ownerHwnd = nullptr;
	}

	bool AppBarReserve::currentRect(RECT& out) const
	{
		if (!registeredFlag || !cbHwnd) return false;
		APPBARDATA abd{};
		abd.cbSize = sizeof(abd);
		abd.hWnd = cbHwnd;
		if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) return false;
		out = abd.rc;
		return true;
	}

	LRESULT CALLBACK AppBarReserve::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		AppBarReserve* self = reinterpret_cast<AppBarReserve*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (msg == WM_NCCREATE) {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = reinterpret_cast<AppBarReserve*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
		if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

		if (self->cbMsg && msg == self->cbMsg) {
			switch (wp) {
			case ABN_POSCHANGED:
			case ABN_FULLSCREENAPP:
			case ABN_STATECHANGE:
				// 系统说"停靠空间被别的 AppBar 改了" / "全屏应用状态变了"。
				// 通知宿主重算位置 —— 这不是轮询，是系统推过来的。
				if (self->onWorkAreaChanged) self->onWorkAreaChanged();
				break;
			default:
				break;
			}
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

} // namespace zdock
