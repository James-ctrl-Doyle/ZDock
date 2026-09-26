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

		void paint();

	protected:
		void layout() override;

	private:
		Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
		winrt::Windows::UI::Composition::CompositionDrawingSurface surface{ nullptr };
		float curScale{ 1.f };
	};

} // namespace zdock
