#include "MonitorUtil.h"
#include "Log.h"

#include <format>

namespace zdock::monitors {

	namespace {

		/// 把一台显示器的信息填进 MonitorDesc。
		bool fillFrom(HMONITOR mon, MonitorDesc& out)
		{
			MONITORINFOEXW mi{};
			mi.cbSize = sizeof(mi);
			if (!GetMonitorInfoW(mon, &mi)) return false;
			out.rect = mi.rcMonitor;                     // ⚠ rcMonitor，不是 rcWork
			out.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
			out.device = mi.szDevice;
			return true;
		}

	} // namespace

	std::vector<MonitorDesc> enumerate()
	{
		std::vector<MonitorDesc> out;

		// ⚠ 回调签名要用 MONITORENUMPROC；MONITORINFOEXW 才能拿到 szDevice 与主屏标志
		//   （MONITORINFO 没有 szDevice 字段，拿不到设备名）。
		auto cb = [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
			auto* vec = reinterpret_cast<std::vector<MonitorDesc>*>(lp);
			MonitorDesc d;
			if (fillFrom(mon, d)) vec->push_back(std::move(d));
			return TRUE;   // 继续枚举
			};
		EnumDisplayMonitors(nullptr, nullptr, cb,
			reinterpret_cast<LPARAM>(&out));

		return out;
	}

	RECT primaryRect()
	{
		for (const auto& d : enumerate()) {
			if (d.primary) return d.rect;
		}
		// 兜底：连主屏标志都拿不到时用系统指标（虚拟屏左上角原点在主屏）。
		// ⚠ 这只有在"主屏不是虚拟屏原点"的多屏布局下才不准，属于极端兜底。
		RECT r{};
		r.right = GetSystemMetrics(SM_CXSCREEN);
		r.bottom = GetSystemMetrics(SM_CYSCREEN);
		return r;
	}

	RECT rectFor(int monitorIndex)
	{
		if (monitorIndex < 0) return primaryRect();

		const auto all = enumerate();
		if (monitorIndex >= static_cast<int>(all.size())) {
			// 越界（比如拔掉了一块屏、或用户手改了个不存在的序号）→ 回主屏。
			// 这里不写日志，因为 `monitorRect()` 会被布局频繁调用，写日志会刷屏；
			// 真正需要让用户知道的是设置窗口那边（它会显示实际生效的显示器）。
			return primaryRect();
		}
		return all[static_cast<size_t>(monitorIndex)].rect;
	}

	std::wstring describe()
	{
		const auto all = enumerate();
		std::wstring s = std::format(L"共 {} 台", all.size());
		for (size_t i = 0; i < all.size(); ++i) {
			const auto& d = all[i];
			s += std::format(L" | [{}] {} {}x{} @({},{}){}",
				i, d.device,
				d.rect.right - d.rect.left, d.rect.bottom - d.rect.top,
				d.rect.left, d.rect.top,
				d.primary ? L" 主屏" : L"");
		}
		return s;
	}

} // namespace zdock::monitors
