#pragma once
#include <Windows.h>

#include <string>
#include <vector>

namespace zdock {

	/// <summary>一台显示器的描述（多显示器锚定用，任务书 §2 #31）。</summary>
	struct MonitorDesc
	{
		/// rcMonitor —— ⚠ 用它而不是 rcWork：工作区会被 AppBar 预留改掉，
		/// 拿它当定位基准会变成自引用（dock 自己改工作区、工作区又影响 dock 位置）。
		RECT rect{};
		bool primary{ false };
		/// 设备名，形如 `\\.\DISPLAY1` —— 给设置窗口列出来用。
		std::wstring device;
	};

	namespace monitors {

		/// <summary>
		/// 枚举所有显示器。顺序 = `EnumDisplayMonitors` 的枚举顺序
		/// （**主屏不保证排在第一个**，所以判断主屏要看 `primary` 标志）。
		/// </summary>
		std::vector<MonitorDesc> enumerate();

		/// <summary>主显示器的 rcMonitor。枚举不出来时退化成 SM_CXSCREEN/SM_CYSCREEN。</summary>
		RECT primaryRect();

		/// <summary>
		/// 按配置的 `monitorIndex` 取 rcMonitor：
		/// `-1`（或越界、枚举失败）→ 主显示器。
		/// </summary>
		RECT rectFor(int monitorIndex);

		/// <summary>诊断用：把当前显示器拓扑拼成一行。</summary>
		std::wstring describe();

	} // namespace monitors

} // namespace zdock
