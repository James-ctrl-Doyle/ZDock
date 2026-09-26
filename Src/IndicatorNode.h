#pragma once
#include <include/Node.h>
#include <wrl/client.h>
#include <d2d1_1.h>

namespace zdock {

	/// <summary>
	/// 运行指示器：图标下缘的一个 4px 圆点（任务书 §3，强调色 #4CC2FF）。
	///
	/// ⚠ 它必须是**独立节点**，不能画在 IconNode 的 surface 上 ——
	///   那张 surface 会随悬停放大动画一起缩放，而指示器要求恒定大小。
	///   独立节点的 visual 不参与图标的 Scale 动画，天然满足。
	/// </summary>
	class IndicatorNode : public Ling::Node
	{
	public:
		IndicatorNode(Ling::WinBase* win);
		~IndicatorNode();

		/// <summary>设定指示器状态。on=false 时整个节点隐藏（而不是画透明像素）。</summary>
		void setOn(bool on);

		/// <summary>指示器颜色（0xAARRGGBB）。默认强调色 #4CC2FF。</summary>
		void setColor(uint32_t argb);

	protected:
		void layout() override;

	private:
		void paint();

		winrt::Windows::UI::Composition::CompositionDrawingSurface surface{ nullptr };
		bool on{ false };
		uint32_t color{ 0xFF4CC2FF };
		bool inited{ false };
	};

} // namespace zdock
