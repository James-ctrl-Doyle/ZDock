#pragma once
#include <include/Node.h>
#include <wrl/client.h>
#include <d2d1_1.h>

namespace zdock {

	/// <summary>
	/// Dock 图标节点：自己画一张位图到 CompositionDrawingSurface，之后所有放大
	/// 都交给 GPU（只动 visual.Scale），不 relayout、不 repaint —— 这是 §9.3 的
	/// 性能红线，写错了就会变成"看着现代、实测吃 CPU"。
	///
	/// 关键约定：surface 按**放大后的峰值尺寸**绘制，而 visual.Size 是基准尺寸，
	/// 由 brush 拉伸填进基准框。这样放大到峰值时是 1:1 采样，不会糊。
	/// </summary>
	class IconNode : public Ling::Node
	{
	public:
		IconNode(Ling::WinBase* win);
		~IconNode();

		/// <summary>设定源位图（建议已是峰值像素尺寸）。立即重绘一次。</summary>
		void setBitmap(Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp);

		/// <summary>当前缩放系数（命中测试与鱼眼计算用）。</summary>
		float scale() const { return curScale; }

		/// <summary>
		/// 动画到目标缩放：只起 Composition keyframe，UI 线程不参与。
		/// 动画锚点在底边中点（图标自下而上长大），在 layout 里设 CenterPoint。
		/// </summary>
		void animateScale(float target, int durationMs);

		/// <summary>不带动画直接落位（初始化 / DPI 变化后复位用）。</summary>
		void resetScale();

		/// <summary>
		/// 运行指示器：图标下缘的小圆点。
		/// ⚠ 指示器**不画在这张 surface 上** —— surface 会随放大动画一起缩放，
		///   而任务书要求指示器是固定的 4px 圆点。它由 DockWin 用一个独立的
		///   sibling 节点绘制（见 DockWin::IndicatorNode）。
		///   这里只记录状态，供命中测试与重建时读取。
		/// </summary>
		void setRunning(bool on);

		/// <summary>按下反馈：缩到 0.92 倍，约 100ms（任务书 §3）。</summary>
		void setPressed(bool on);

		/// <summary>
		/// 弹跳动画：图标向上弹一次再落回（注意请求 / 启动反馈用，任务书 §3 约 600ms）。
		/// 只动 Composition 的 Offset，不 relayout。
		/// </summary>
		void bounce();

		/// <summary>临时图标（未固定的运行中应用）用淡化表现，区别于固定项。</summary>
		void setTemporary(bool on);

		/// <summary>是否处于"临时图标"状态（命中与菜单行为要区分）。</summary>
		bool temporary() const { return isTemporary; }

		/// <summary>是否在运行（DockWin 用来决定要不要显示指示器）。</summary>
		bool running() const { return curRunning; }

		void paint();

	protected:
		void layout() override;

	private:
		Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
		winrt::Windows::UI::Composition::CompositionDrawingSurface surface{ nullptr };
		float curScale{ 1.f };
		float baseScale{ 1.f };      // 不含按下反馈的"目标"缩放
		bool curRunning{ false };
		bool curPressed{ false };
		bool isTemporary{ false };
	};

} // namespace zdock
