#include "PreviewWin.h"

#include <include/Node.h>

#include <algorithm>
#include <format>

namespace zdock {

	PreviewWin::~PreviewWin()
	{
		unregisterThumb();
	}

	void PreviewWin::onCreated()
	{
		// 深色底：预览缩略图是 DWM 直接合成在**窗口表面之上**的，
		// 所以节点只要把"底"铺满就行（缩略图会盖住缩略图区那一块）。
		// 这里用近黑不透明 —— 不能半透明：半透明会让缩略图背后透出桌面，很脏。
		auto* bg = body->makeChild<Ling::Node>();
		bg->setPositionType(Ling::Position::Absolute);
		bg->setPosition(Ling::Edge::Left, 0.f);
		bg->setPosition(Ling::Edge::Top, 0.f);
		bg->setSize(kWinW, kWinH);
		bg->setBg(Ling::Color(0x1C1C1CFFu));   // 0xRRGGBBAA
		bg->setBorderRadius(8.f);
		bg->setBorder(1.f, Ling::Color(0xFFFFFF26u));
	}

	void PreviewWin::createOnce()
	{
		if (hwnd) return;
		setSize(kWinW, kWinH);
		setPosition(0, 0);
		// ⚠ 不抢焦点、不进 Alt+Tab、永远置顶 —— 和 dock 主窗口同一套约定。
		createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
		// 给个可识别的标题：Ling 的窗口类名是**全局统一**的（setAppWindowClassName），
		// 所以 dock 主窗口和预览窗口类名相同，只能靠标题区分 ——
		// 探针（和以后排查问题）都靠它。
		if (hwnd) SetWindowTextW(hwnd, L"ZDockPreview");
		layout();
		refresh();
	}

	void PreviewWin::unregisterThumb()
	{
		if (thumb) {
			DwmUnregisterThumbnail(thumb);
			thumb = nullptr;
		}
		target = nullptr;
	}

	void PreviewWin::showFor(HWND hTarget, const std::wstring& title, POINT anchorScreen)
	{
		if (!hwnd) createOnce();
		if (!hwnd || !hTarget || !IsWindow(hTarget)) return;

		// 同一个目标窗口已经显示着 → 什么都不用做（省一次注销/重注册）
		if (visible && target == hTarget) return;

		unregisterThumb();

		const HRESULT hr = DwmRegisterThumbnail(hwnd, hTarget, &thumb);
		if (FAILED(hr) || !thumb) {
			log(std::format(L"[preview] DwmRegisterThumbnail 失败 hr=0x{:08X}（窗口可能已关闭）",
				static_cast<unsigned>(hr)));
			hidePreview();
			return;
		}
		target = hTarget;
		visible = true;

		// ---- 保持源窗口纵横比地放进缩略图区 ----
		// ⚠ DWM 会把画面**拉伸**到 rcDestination（不保持比例），所以得自己算。
		//   源窗口可能是最小化的（拿到的 rect 是 -32000），那时给个兜底尺寸。
		RECT src{};
		GetWindowRect(hTarget, &src);
		int sw = src.right - src.left;
		int sh = src.bottom - src.top;
		if (sw <= 0 || sh <= 0 || IsIconic(hTarget)) { sw = 1600; sh = 900; }

		const float d = (dpi > 0.f) ? dpi : 1.f;
		const float boxW = kThumbW * d;      // 缩略图区的**物理**尺寸
		const float boxH = kThumbH * d;
		const float scale = std::min(boxW / static_cast<float>(sw), boxH / static_cast<float>(sh));
		const float tw = static_cast<float>(sw) * scale;
		const float th = static_cast<float>(sh) * scale;
		// 居中于缩略图区（客户区左上角 = (kPad*d, kPad*d)）
		const float left = kPad * d + (boxW - tw) * 0.5f;
		const float top = kPad * d + (boxH - th) * 0.5f;

		DWM_THUMBNAIL_PROPERTIES props{};
		props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE
			| DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
		props.rcDestination = RECT{
			static_cast<LONG>(std::lround(left)), static_cast<LONG>(std::lround(top)),
			static_cast<LONG>(std::lround(left + tw)), static_cast<LONG>(std::lround(top + th)) };
		props.opacity = 255;
		props.fVisible = TRUE;
		// SOURCECLIENTAREAONLY：只要客户区，别把标题栏和边框也缩进来
		props.fSourceClientAreaOnly = TRUE;

		const HRESULT hr2 = DwmUpdateThumbnailProperties(thumb, &props);
		if (FAILED(hr2)) {
			log(std::format(L"[preview] DwmUpdateThumbnailProperties 失败 hr=0x{:08X}",
				static_cast<unsigned>(hr2)));
			hidePreview();
			return;
		}

		// ---- 摆位置：在锚点上方居中，并夹进屏幕内 ----
		// ⚠ 用**监视器 rcMonitor** 而不是工作区：工作区会被 AppBar 预留改掉，
		//   那是个自引用（见 DockWin::dockRectShown 的注释）。
		const int winW = static_cast<int>(std::lround(kWinW * d));
		const int winH = static_cast<int>(std::lround(kWinH * d));
		int x = anchorScreen.x - winW / 2;
		int y = anchorScreen.y - winH;          // 锚点通常是图标顶边中点

		HMONITOR mon = MonitorFromPoint(anchorScreen, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (mon && GetMonitorInfoW(mon, &mi)) {
			const RECT& m = mi.rcMonitor;
			x = std::max(static_cast<int>(m.left), std::min(x, static_cast<int>(m.right) - winW));
			y = std::max(static_cast<int>(m.top), std::min(y, static_cast<int>(m.bottom) - winH));
		}
		setPosition(x, y);
		show();
		refresh();

		log(std::format(L"[preview] 显示预览：目标=0x{:X} 标题=「{}」 位置=({},{}) 缩略图={:.0f}x{:.0f}",
			reinterpret_cast<unsigned long long>(hTarget), title, x, y, tw, th));
	}

	void PreviewWin::hidePreview()
	{
		unregisterThumb();
		if (hwnd && visible) {
			hide();
		}
		if (visible) log(L"[preview] 收起预览");
		visible = false;
	}

} // namespace zdock
