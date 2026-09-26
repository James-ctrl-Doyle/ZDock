#pragma once
#include <include/WinBase.h>
#include "IconNode.h"
#include <vector>
#include <string>

namespace zdock {

	struct DockItem
	{
		std::wstring path;
		std::wstring name;
		IconNode* node{ nullptr };
	};

	/// <summary>
	/// ZDock 主窗口（阶段一）：贴主屏底边的半透明圆角面板 + 一行图标。
	///
	/// 两条设计约束直接决定实现形态，改代码时别绕开：
	///  1) 悬停放大要求图标溢出面板，所以**窗口矩形比面板大**（上方留 halo），
	///     那圈透明像素必须在 onHitTest 里返回 HTTRANSPARENT 放行给下面的窗口；
	///  2) 放大动画只动 Composition 的 Scale，不 relayout、不 repaint。
	/// </summary>
	class DockWin : public Ling::WinBase
	{
	public:
		DockWin();
		~DockWin();

		/// <summary>建窗口 + 内容。构造函数里不做（那时 dpi/尺寸还没配好）。</summary>
		void create();

	protected:
		void onCreated() override;
		LRESULT onHitTest(const POINT pos) override;

	private:
		// ---- 布局常量：逻辑像素（物理值由 Ling 的 setter 乘 dpi）----
		static constexpr float kIconBase = 48.f;    // 图标基准边长
		static constexpr float kIconGap = 12.f;     // 图标间距（≈25%）
		static constexpr float kPadX = 14.f;        // 面板左右内边距
		static constexpr float kPadY = 8.f;         // 面板上下内边距
		static constexpr float kHaloH = 72.f;       // 面板上方给放大/标签留的透明区
		static constexpr float kSideSlack = 48.f;   // 面板左右各留的透明区
		static constexpr float kBottomMargin = 6.f; // 距工作区底边
		static constexpr float kHoverPeak = 1.7f;   // 悬停峰值缩放
		static constexpr float kHoverSigma = 1.0f;  // 鱼眼衰减（以图标位距为单位）
		static constexpr int   kAnimMs = 150;
		static constexpr int   kTimerHover = 1;     // 判定"鼠标已移出"的短定时器
		static constexpr int   kMenuExit = 101;
		// 悬停定时器周期。它现在唯一的职责是"兜底察觉鼠标走了"（见
		// refreshHoverFromCursor）：鼠标移出窗口未必会给我们消息
		// （比如移向另一个进程的窗口、或者被 region 挖掉的那块隙缝），
		// 所以留一个低频核对。100ms 是"离开到缩回"体感的上限。

		void collectDefaultItems();
		void buildContent();
		void loadIcons();

		int  indexAtHover(POINT pt) const;   // 未缩放命中带（含横向容差）
		int  indexAtVisual(POINT pt) const;  // 当前缩放后的可见框（点击用）
		/// <summary>
		/// 按**当前真实光标位置**重算 hover，返回是否发生了变化。
		///
		/// ⚠ 这是修"图标来回缩放"的那个 bug 的核心：绝不能用
		///   "上次 WM_NCHITTEST 的时间戳"当心跳 —— WM_NCHITTEST 只在鼠标**移动**时
		///   才来，光标停住不动时它就断了。原来靠它判定"鼠标已经走了"，会是
		///   150ms 后误判离开（图标缩回）→ 缩回改变命中区域 → 系统补发一次
		///   WM_NCHITTEST（时间戳刷新、图标又放大）→ 再断 → 无限循环，
		///   在用户眼里就是"图标来回变大变小"。
		/// </summary>
		bool refreshHoverFromCursor();
		void applyHover(int index);
		void ensureHoverTimer(bool want);
		void updateHitRegion();
		void launch(int index);
		void showContextMenu();

		float px(float logical) const { return logical * dpi; }
		float panelW() const;
		float panelH() const;
		float iconsW() const;
		static bool inRect(POINT pt, float x, float y, float w, float h);

		std::vector<DockItem> items;
		Ling::Node* panel{ nullptr };
		Ling::Node* row{ nullptr };
		int hoverIndex{ -1 };
		bool hoverTimerOn{ false };
	};

} // namespace zdock
