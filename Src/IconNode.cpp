#include "IconNode.h"

#include <include/WinBase.h>
#include <include/D2D.h>
// DrawingSurface 的互操作接口（ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop）
// 在这个头里 —— Ling 的 pch.h 也 include 了它；我们不走 pch，所以自己带。
#include <Windows.UI.Composition.Interop.h>

#include <chrono>

using Microsoft::WRL::ComPtr;

namespace zdock {

	IconNode::IconNode(Ling::WinBase* win) : Ling::Node(win)
	{
	}

	IconNode::~IconNode()
	{
	}

	void IconNode::setBitmap(ComPtr<ID2D1Bitmap> bmp)
	{
		bitmap = std::move(bmp);
		paint();
		win->refresh();
	}

	void IconNode::layout()
	{
		Ling::Node::layout();
		// 放大锚点：底边中点 —— 图标从底线向上长，与真实 dock 一致。
		visual.CenterPoint({ w * 0.5f, h, 0.f });
		paint();
	}

	void IconNode::paint()
	{
		const int boxW = static_cast<int>(w);
		const int boxH = static_cast<int>(h);
		if (boxW <= 0 || boxH <= 0) return;

		auto d2d = Ling::D2D::get();
		if (!d2d) return;

		// surface 用源位图的像素尺寸（= 峰值尺寸），visual 的框是基准尺寸，
		// brush 拉伸填框；放大动画到峰值时正好 1:1。
		int pxW = boxW, pxH = boxH;
		if (bitmap) {
			auto sz = bitmap->GetSize();
			pxW = static_cast<int>(sz.width);
			pxH = static_cast<int>(sz.height);
		}

		if (!surface) {
			surface = d2d->createDrawingSurface(win->compositor, static_cast<float>(pxW), static_cast<float>(pxH));
			if (!surface) return;
			auto brush = win->compositor.CreateSurfaceBrush(surface);
			brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::Fill);
			visual.Brush(brush);
		}
		else {
			auto sz = surface.SizeInt32();
			if (sz.Width != pxW || sz.Height != pxH) surface.Resize({ pxW, pxH });
		}

		auto s = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
		ComPtr<ID2D1DeviceContext> ctx;
		POINT offset{};
		if (FAILED(s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(ctx.GetAddressOf()), &offset))) return;
		ctx->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
		ctx->Clear(0);
		if (bitmap) {
			// 位图已按目标像素预处理过，这里 1:1 画即可
			D2D1_RECT_F dst{ 0.f, 0.f, static_cast<float>(pxW), static_cast<float>(pxH) };
			ctx->DrawBitmap(bitmap.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
		}
		else {
			// 取图标失败时的兜底：一个中性灰块（宁可难看，也不要"图标凭空消失"）
			ComPtr<ID2D1SolidColorBrush> brush;
			ctx->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.38f, 1.f), &brush);
			if (brush) {
				D2D1_ROUNDED_RECT rr{ D2D1::RectF(2.f, 2.f, static_cast<float>(pxW) - 2.f, static_cast<float>(pxH) - 2.f), 6.f, 6.f };
				ctx->FillRoundedRectangle(rr, brush.Get());
			}
		}
		s->EndDraw();
	}

	void IconNode::animateScale(float target, int durationMs)
	{
		baseScale = target;
		// 按下期间不动 —— 松手时 setPressed(false) 会把中心缩放开回 baseScale
		if (curPressed) return;

		if (target == curScale) return;
		const float from = curScale;
		curScale = target;

		auto comp = win->compositor;
		auto ease = comp.CreateCubicBezierEasingFunction({ 0.16f, 0.84f }, { 0.44f, 1.f });
		auto anim = comp.CreateScalarKeyFrameAnimation();
		anim.InsertKeyFrame(0.f, from);
		anim.InsertKeyFrame(1.f, target, ease);
		anim.Duration(std::chrono::milliseconds{ durationMs });
		visual.StartAnimation(L"Scale.X", anim);
		visual.StartAnimation(L"Scale.Y", anim);
	}

	void IconNode::resetScale()
	{
		baseScale = 1.f;
		curPressed = false;
		curScale = 1.f;
		visual.StopAnimation(L"Scale.X");
		visual.StopAnimation(L"Scale.Y");
		visual.Scale({ 1.f, 1.f, 1.f });
	}

	// ---------------------------------------------------------------------------
	// 按下反馈：缩到 0.92 倍。任务书 §3 要求约 100ms。
	// ⚠ 不能直接改 curScale —— 那个字段是"悬停鱼眼的目标值"，命中测试在读它。
	//   按下只是一个**视觉上的临时压制**，松手要能回到原来的鱼眼缩放值，
	//   所以按下态走 baseScale 的百分比，不动 curScale 的语义。
	// ---------------------------------------------------------------------------
	void IconNode::setPressed(bool on)
	{
		if (curPressed == on) return;
		curPressed = on;

		const float target = on ? baseScale * 0.92f : baseScale;
		auto comp = win->compositor;
		auto ease = comp.CreateCubicBezierEasingFunction({ 0.16f, 0.84f }, { 0.44f, 1.f });
		auto anim = comp.CreateScalarKeyFrameAnimation();
		anim.InsertKeyFrame(1.f, target, ease);
		anim.Duration(std::chrono::milliseconds{ 100 });
		visual.StartAnimation(L"Scale.X", anim);
		visual.StartAnimation(L"Scale.Y", anim);
	}

	// ---------------------------------------------------------------------------
	// 弹跳：Offset.Y 向上抬一下再落回。只动合成器属性，不 relayout。
	// 用 keyframe 的 ease-out 收尾（任务书 §3：约 600ms、阻尼收尾）。
	// ⚠ offset 用的是**物理像素**（visual 的 Offset 是 DIP，但 Ling 在这个项目里
	//   全程把逻辑像素乘过 dpi 了 —— 这里跟随 IconNode 的既有约定，直接用 h 的比例）。
	// ---------------------------------------------------------------------------
	void IconNode::bounce()
	{
		const float amp = h * 0.28f;      // 弹起高度 ≈ 图标高的 28%
		auto comp = win->compositor;
		auto ease = comp.CreateCubicBezierEasingFunction({ 0.22f, 0.90f }, { 0.36f, 1.f });
		auto anim = comp.CreateVector3KeyFrameAnimation();
		anim.InsertKeyFrame(0.0f, { 0.f, 0.f, 0.f });
		anim.InsertKeyFrame(0.45f, { 0.f, -amp, 0.f }, ease);
		anim.InsertKeyFrame(1.0f, { 0.f, 0.f, 0.f }, ease);
		anim.Duration(std::chrono::milliseconds{ 600 });
		visual.StartAnimation(L"Offset", anim);
	}

	void IconNode::setRunning(bool on)
	{
		// 状态记录即可：指示器由 DockWin 的独立节点绘制（见 IconNode.h 的说明）
		curRunning = on;
	}

	void IconNode::setTemporary(bool on)
	{
		if (isTemporary == on) return;
		isTemporary = on;
		// 临时图标淡化一点，跟固定项区分开（用户一眼能看出"这个还没固定"）
		visual.Opacity(on ? 0.82f : 1.f);
	}

} // namespace zdock
