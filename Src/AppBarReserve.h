#pragma once
#include <Windows.h>
#include <shellapi.h>   // ABE_BOTTOM / APPBARDATA（默认参数里用到 ABE_BOTTOM）
#include <functional>
#include <string>

namespace zdock {

	/// <summary>
	/// 工作区预留（AppBar，任务书 §2 功能表 #30 / §9.6）。
	///
	/// 作用：向系统申请"停靠边这块屏幕空间归我"，系统会把**工作区**缩进，
	/// 于是普通窗口最大化时自动避开 dock，不会被 dock 盖住。
	///
	/// ⚠ 三条硬约束（红线 + 任务书）：
	///  1. **默认关闭** —— 不开时 dock 就是一块浮在普通窗口之上的 topmost 窗口，
	///     不占工作区。是否占用由用户决定。
	///  2. 一切走 `SHAppBarMessage`：ABM_NEW / ABM_QUERYPOS / ABM_SETPOS /
	///     ABM_WINDOWPOSCHANGED / ABM_GETTASKBARPOS。
	///     **绝不碰桌面窗口、绝不改系统任务栏状态**（红线 7：别把 explorer / 任务栏搞坏）。
	///  3. 程序被强杀后系统必须能自然恢复（红线 7）。
	///     ⚠⚠ **实测推翻了"进程一死 shell 就自动注销 AppBar"这个想当然**：
	///        强杀（taskkill /F）之后工作区**一直保持缩进**，等 15 秒也不恢复。
	///        因为 `ABM_REMOVE` 是应用自己的责任，shell 只是**记着**这条记录、
	///        从不检查宿主窗口是否还活着。
	///        而且这条死记录会**叠加**：下次启动照常注册时，shell 算成
	///        "残留 + 新"，工作区被占两道（实测干净底 1440 → 1257 =
	///        两层 95px，日志里"抢占前的值"自己也已经是 1345）。
	///        好消息是它**不是永久**的 —— 下一次
	///        `ABM_NEW → ABM_SETPOS(空) → ABM_REMOVE` 完整循环就会把它扫掉
	///        （实测 eaten 从 183px 掉回 0~1px）。
	///        应对见 recoverStaleWorkArea / purgeStaleRecord。
	///
	/// 本类只负责"申请 / 撤销 / 更新位置"，**不管 dock 窗口的坐标** ——
	/// 位置由调用方（DockWin）算好传进来，避免两处各算一份。
	/// </summary>
	class AppBarReserve
	{
	public:
		AppBarReserve() = default;
		~AppBarReserve();

		/// <summary>
		/// 注册为 AppBar 并把请求的位置报给系统。
		/// `edge` 目前固定用 ABE_BOTTOM（dock 贴底）。
		/// `desired` 是调用方希望的**物理像素**屏幕矩形（dock 面板的矩形）。
		/// `stateFilePath` 用于记录"抢占前的工作区"，供强杀后自愈（见 recoverStaleWorkArea）。
		/// 返回是否注册成功。
		/// </summary>
		bool register_(HWND owner, const RECT& desired, const std::wstring& stateFilePath,
			UINT edge = ABE_BOTTOM);

		/// <summary>撤销 AppBar 注册（关掉预留 / 退出时调）。</summary>
		void unregister_();

		/// <summary>
		/// 启动时清理"上次进程被强杀留下的工作区占用"（红线 7）。
		///
		/// ⚠ 实测结论（`build-support/_probe_appbar_recover.py` / `_probe_stale_lifecycle.py`）：
		///   **shell 不会**因为 AppBar 的宿进程死掉就自动注销它 —— 工作区会一直保持
		///   缩进（等 15 秒也不恢复）。原因是 `ABM_REMOVE` 是应用自己的责任。
		///
		///   而且死记录会**叠加**：实测第二次启动时，`SPI_GETWORKAREA` 读到的
		///   "抢占前的值"本身就已经是 1345（被残留占着），注册完变 1257
		///   （占两道 95px）。日志证据：
		///     已记录抢占前工作区 (0,38)-(2560,1440)  → 已注册 → 工作区底 1345
		///     [强杀]
		///     已记录抢占前工作区 (0,38)-(2560,1345)  → 已注册 → 工作区底 1257  ← 叠加
		///
		/// 所以采用**状态文件**方案：`register_()` 抢占工作区**之前**先把当时的
		/// `SPI_GETWORKAREA` 存一份；正常退出时删掉它。
		/// 下次启动若发现这个文件**还在**，就说明上次是被强杀的 —— 用它把工作区
		/// `SPI_SETWORKAREA` 恢复回去，然后删文件。
		///
		/// ⚠ 关键补充（实测）：**光恢复数值不够**，shell 里那条死记录还在，
		///   再注册就会叠加。清理死记录要在**另一次完整的 NEW → SETPOS(空) →
		///   REMOVE 循环**里顺带完成（实测 `eaten` 从 183px 掉回 0~1px；
		///   见 `_probe_stale_lifecycle.py` 第 4 步）。
		///   所以本函数在恢复工作区之后，**还会跑一次 `purgeStaleRecord()`**
		///   把 shell 里的死记录扫掉，然后调用方才正常注册 —— 功能不降级。
		///
		/// 为什么这样是安全的：只恢复**我们自己**存下来的值，且只在自己留下残迹时动。
		/// 绝不会去改系统任务栏 / 别的 AppBar 的工作区。
		/// </summary>
		/// <param name="logFn">日志回调（此时还没有对象，所以走参数）。</param>
		/// <param name="stateFilePath">状态文件全路径（exe 同目录）。</param>
		/// <returns>true = 发现了上次留下的残留（调用方**必须**在注册前调用
		///          `purgeStaleRecord()`，否则工作区会被占两道）。</returns>
		static bool recoverStaleWorkArea(const std::function<void(const std::wstring&)>& logFn,
			const std::wstring& stateFilePath);

		/// <summary>
		/// 扫掉 shell 里残留的**死 AppBar 记录**。
		///
		/// ⚠ 实测（`_probe_stale_lifecycle.py` / `_probe_stale_register.py`）：
		///   死记录活到下次"NEW → SETPOS(空) → REMOVE"完整循环时才被 shell 清掉。
		///   只做 NEW + SETPOS(真位置)（也就是我们正常的注册路径）**清不掉**它。
		///
		/// 做法：建一个一次性的最小 AppBar 宿主窗口，走完
		/// `ABM_NEW → ABM_QUERYPOS → ABM_SETPOS(空) → ABM_REMOVE` 全套再销毁。
		/// 它自己不会留下任何占用（SETPOS 是空矩形），纯粹是"逼 shell 重扫一遍"。
		/// </summary>
		static void purgeStaleRecord(const std::function<void(const std::wstring&)>& logFn);

		bool registered() const { return registeredFlag; }

		/// <summary>工作区被系统改动时回调（宿主据此重算 dock 位置）。</summary>
		std::function<void()> onWorkAreaChanged;

		/// <summary>日志回调（复用宿主的 Log）。</summary>
		std::function<void(const std::wstring&)> onLog;

		/// <summary>取系统当前批准的停靠矩形（未注册时返回 false）。</summary>
		bool currentRect(RECT& out) const;

	private:
		static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
		void log(const std::wstring& s) const { if (onLog) onLog(s); }

		/// ABM_NEW 需要一个"回调窗口"来收 ABN_* 通知。
		/// ⚠ 用 dock 主窗口不行 —— Ling 的 WinBase::winProc 是静态私有的，
		///   拿不到消息口（和 WindowTracker 同一个原因）。自建一个隐藏窗口。
		HWND cbHwnd{ nullptr };
		UINT cbMsg{ 0 };          ///< ABN_* 通知走这个注册消息号
		HWND ownerHwnd{ nullptr };
		bool registeredFlag{ false };
		RECT lastApproved{};
		/// "抢占前的工作区"状态文件全路径（强杀自愈用，正常退出会被删掉）
		std::wstring statePath;
	};

} // namespace zdock
