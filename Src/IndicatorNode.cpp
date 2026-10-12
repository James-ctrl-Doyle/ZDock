#include "IndicatorNode.h"

#include <include/WinBase.h>
#include <include/D2D.h>
#include <Windows.UI.Composition.Interop.h>

namespace zdock {

	IndicatorNode::IndicatorNode(Ling::WinBase* win) : Ling::Node(win)
	{
	}

	IndicatorNode::~IndicatorNode() = default;

	void IndicatorNode::setColor(uint32_t argb)
	{
		color = argb;
		if (inited) paint();
	}

	void IndicatorNode::setOn(bool v)
	{
		// ⚠⚠ 这里**不能**早退（`if (on == v) return;`）—— 踩过：
		//   `visual.Opacity` 的默认值是 1（可见），而 `on` 的初值是 false。
		//   早退会让"创建时那次 setOn(false)"什么都不做 → **指示器一直亮着**；
		//   而且此后只要它没被设成 true 过，任何 setOn(false) 都会早退，
		//   **永远关不掉**。用户 2026-10-12 报的"没开 TIM，指示器还亮着"就是这个
		//   （固定项、没运行的应用全都在亮）。
		//   代价只是一次 Composition 属性赋值，可以忽略。
		on = v;
		// 隐藏用 visual 的 Opacity，不是把节点尺寸设 0 —— 后者会触发 relayout。
		visual.Opacity(on ? 1.f : 0.f);
	}

	void IndicatorNode::layout()
	{
		Ling::Node::layout();
		if (w > 0.f && h > 0.f) {
			inited = true;
			paint();
		}
	}

	void IndicatorNode::paint()
	{
		const int px = static_cast<int>(w);
		const int py = static_cast<int>(h);
		if (px <= 0 || py <= 0) return;

		auto d2d = Ling::D2D::get();
		if (!d2d) return;

		if (!surface) {
			surface = d2d->createDrawingSurface(win->compositor,
				static_cast<float>(px), static_cast<float>(py));
			if (!surface) return;
			auto brush = win->compositor.CreateSurfaceBrush(surface);
			brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::Fill);
			visual.Brush(brush);
		}
		else {
			auto sz = surface.SizeInt32();
			if (sz.Width != px || sz.Height != py) surface.Resize({ px, py });
		}

		auto s = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
		Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx;
		POINT offset{};
		if (FAILED(s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
			reinterpret_cast<void**>(ctx.GetAddressOf()), &offset))) return;

		ctx->SetTransform(D2D1::Matrix3x2F::Translation(
			static_cast<float>(offset.x), static_cast<float>(offset.y)));
		ctx->Clear(0);

		// 颜色解包：0xAARRGGBB -> D2D 的 ColorF(r,g,b,a)
		const float a = ((color >> 24) & 0xFF) / 255.f;
		const float r = ((color >> 16) & 0xFF) / 255.f;
		const float g = ((color >> 8) & 0xFF) / 255.f;
		const float b = (color & 0xFF) / 255.f;

		Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
		ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &brush);
		if (brush) {
			const float cx = px * 0.5f;
			const float cy = py * 0.5f;
			const float rad = (px < py ? px : py) * 0.5f;
			ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush.Get());
		}
		s->EndDraw();
	}

} // namespace zdock
