#pragma once
#include <Windows.h>
#include <dwmapi.h>

#include <include/WinBase.h>

#include <functional>
#include <string>

namespace zdock {

	/// <summary>
	/// 悬停预览气泡（任务书 §2 #17）：鼠标在图标上停留约 300ms 后弹出，
	/// 显示该应用主窗口的**实时画面**；鼠标离开图标即收起。
	///
	/// 画面来源用 **DWM 缩略图**（`DwmRegisterThumbnail` / `DwmUpdateThumbnailProperties`），
	/// 它让 DWM 直接合成目标窗口的画面 —— **零截图成本**，也不用定时刷新
	/// （任务书 §9.7 明确要求"刷新由事件驱动，不要定时截屏"）。
	///
	/// ⚠ 宿主形态这个最贵的假设已经实测过（`build-support/_probe_dwm_thumb_ling.cpp`）：
	///   真 Ling 窗口带 `WS_EX_NOREDIRECTIONBITMAP`、非 layered，
	///   `DwmRegisterThumbnail` 返回 S_OK，且画面**确实合成进来了**
	///   （宿主截图里数到源窗口的全部像素）。所以这条路可用。
	///
	/// 它是一个**独立顶层窗口**，不是 dock 窗口的一部分 ——
	/// dock 窗口只有面板那么高，预览要浮在图标**上方**，只能另开一个窗口。
	/// </summary>
	class PreviewWin : public Ling::WinBase
	{
	public:
		// ⚠ 不能写 `override`：Ling 的 `WinBase::~WinBase()` **不是虚函数**
		//   （实测 C3668）。所以这个类只能**值语义**持有（DockWin 的成员），
		//   绝不能通过 `WinBase*` 删除 —— 那不会调到这里的析构、DWM 缩略图句柄就漏了。
		~PreviewWin();

		/// <summary>建窗口（只建一次，之后靠 show/hide 复用 —— 别每次悬停都建）。</summary>
		void createOnce();

		/// <summary>
		/// 显示 `target` 窗口的预览。`wantTopLeft` 是**期望的窗口左上角（屏幕物理像素）**——
		/// 由调用方按停靠边算好"预览该浮在哪一侧"，这里只负责夹进屏幕内。
		/// </summary>
		void showFor(HWND target, const std::wstring& title, POINT wantTopLeft);

		/// <summary>收起（注销缩略图 + 隐藏窗口）。</summary>
		void hidePreview();

		bool showing() const { return visible; }

		/// <summary>日志回调（复用 DockWin 的 Log）。</summary>
		std::function<void(const std::wstring&)> onLog;

		void onCreated() override;

		/// 缩略图区尺寸（逻辑像素）。窗口 = 缩略图区 + 四周留白 + 底部标题条。
		static constexpr float kThumbW = 240.f;
		static constexpr float kThumbH = 150.f;
		static constexpr float kPad = 8.f;
		static constexpr float kTitleH = 22.f;
		static constexpr float kWinW = kThumbW + 2 * kPad;
		static constexpr float kWinH = kThumbH + 2 * kPad + kTitleH;

	private:
		void log(const std::wstring& s) const { if (onLog) onLog(s); }
		void unregisterThumb();

		HWND target{ nullptr };
		HTHUMBNAIL thumb{ nullptr };
		bool visible{ false };
	};

} // namespace zdock
