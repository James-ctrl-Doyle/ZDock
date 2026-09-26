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
		curScale = 1.f;
		visual.StopAnimation(L"Scale.X");
		visual.StopAnimation(L"Scale.Y");
		visual.Scale({ 1.f, 1.f, 1.f });
	}

} // namespace zdock
