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
		// 带 kCfg 前缀的几个来自 config.json，在构造时缓存成成员；
		// 其余是纯内部结构参数，不对外暴露。
		static constexpr float kPadX = 14.f;        // 面板左右内边距
		static constexpr float kPadY = 8.f;         // 面板上下内边距
		static constexpr float kHaloH = 72.f;       // 面板上方给放大/标签留的透明区
		static constexpr float kSideSlack = 48.f;   // 面板左右各留的透明区
		static constexpr float kHoverSigma = 1.0f;  // 鱼眼衰减（以图标位距为单位）
		static constexpr int   kTimerHover = 1;     // 判定"鼠标已移出"的兜底定时器
		static constexpr int   kMenuExit = 101;
		static constexpr int   kMenuOpen = 102;
		static constexpr int   kMenuOpenAdmin = 103;
		static constexpr int   kMenuRemove = 104;
		static constexpr int   kMenuReload = 105;
		static constexpr int   kMenuAdd = 106;
		// 悬停定时器周期。它现在唯一的职责是"兜底察觉鼠标走了"（见
		// refreshHoverFromCursor）：鼠标移出窗口未必会给我们消息
		// （比如移向另一个进程的窗口、或者被 region 挖掉的那块隙缝），
		// 所以留一个低频核对。100ms 是"离开到缩回"体感的上限。

		// ---- 来自 config.json 的缓存（逻辑像素 / 其余原样）----
		float cfgIconBase{ 48.f };     // 图标基准边长
		float cfgIconGap{ 12.f };      // 图标间距
		float cfgHoverPeak{ 1.7f };    // 悬停峰值缩放
		int   cfgAnimMs{ 150 };        // 动画时长

		/// <summary>把 config 的值搬进上面那组缓存。create() 最开始调一次。</summary>
		void applyConfig();

		void collectItemsFromConfig();
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
		/// <summary>以管理员身份启动（ShellExecuteW "runas"，会弹 UAC）。</summary>
		void launchAdmin(int index);
		void showContextMenu();
		void showItemContextMenu(int index);
		/// <summary>
		/// 在自己窗口内弹菜单（owner = dock 的 hwnd，不是 Ling 的 message-only 窗口）。
		/// 会临时摘掉 WS_EX_NOACTIVATE 以便拿到前台权。返回选中的命令 id，取消返回 0。
		/// </summary>
		UINT popupMenuHere(HMENU menu, POINT screenPt);
		/// <summary>把某项从 Dock 移除并落盘 config.json，然后重建界面。</summary>
		void removeItem(int index);
		/// <summary>弹文件选择框挑一个 exe/lnk/文件夹加到 Dock 末尾，并落盘。取消则什么都不做。</summary>
		void addItem();
		/// <summary>按 Config 的最新内容重建整个界面（销毁并重造所有子节点）。</summary>
		void rebuild();
		/// <summary>重新读 config.json 后重建（用户手改配置不必重启进程）。</summary>
		void reloadConfig();

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
