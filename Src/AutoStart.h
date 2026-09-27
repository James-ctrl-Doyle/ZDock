#pragma once
#include <string>

namespace zdock {

	/// <summary>
	/// 开机自启（任务书 §2 #26）：往
	/// `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 写一个名为 `ZDock` 的值。
	///
	/// 为什么是 HKCU Run：
	///  · **不需要管理员权限**（任务书 §2 #26 也明确写的" HKCU Run 项"）；
	///  · 比"启动文件夹放快捷方式"好收拾 —— 关掉自启就是删一个值，
	///    不用去用户目录里找残留的 .lnk。
	///
	/// ⚠ 值必须是 **exe 的完整路径**，而且**路径带空格时要加引号** ——
	///   不加的话 Windows 会把空格前的部分当程序名，开机直接启动失败
	///   （而且失败是静默的，用户只会觉得"自启没生效"）。
	/// </summary>
	namespace autostart {

		/// 注册表项（HKCU 下）。
		inline constexpr const wchar_t* kRunKey =
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
		inline constexpr const wchar_t* kValueName = L"ZDock";

		/// 当前进程的 exe 完整路径。
		std::wstring exePath();

		/// 我们期望写进注册表的值（带引号的 exe 路径）。
		std::wstring expectedValue();

		/// 注册表里现在的值；没有这一项就返回空串。
		std::wstring currentValue();

		/// 现在是否已注册自启（只要有值就算，不比对路径 —— 用户挪了 exe 位置
		/// 时"有值但路径旧"也算开着，`sync()` 会把它改成新路径）。
		bool isEnabled();

		/// 写入 / 删除注册表值。返回是否成功。
		bool set(bool on);

		/// 以 `want` 为准同步注册表：已处于目标状态就什么都不做。
		/// 返回 true 表示"同步之后确实处于目标状态"。
		///
		/// ⚠ 调用时机有两处：程序启动时（把上次的配置落到注册表）和设置窗口改动时。
		bool sync(bool want);

	} // namespace autostart

} // namespace zdock
