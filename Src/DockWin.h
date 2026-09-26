#pragma once
#include <include/WinBase.h>
#include "IconNode.h"
#include "IndicatorNode.h"
#include "WindowTracker.h"
#include <vector>
#include <string>

namespace zdock {

	struct DockItem
	{
		std::wstring path;
		std::wstring name;
		IconNode* node{ nullptr };
		/// <summary>运行指示器（图标下缘的 4px 圆点）。固定项与临时项都有。</summary>
		IndicatorNode* indicator{ nullptr };
		/// <summary>临时图标：不是用户固定的，而是"有窗口在跑但这个应用没被固定"时自动加的。</summary>
		bool temporary{ false };
		/// <summary>
		/// 该图标对应哪个应用分组（WindowTracker 的 AppGroup::key）。
		/// 固定项按 exe 路径找分组；临时项直接记住自己的键。
		/// 空 = 当前没有对应分组（应用没在跑）。
		/// </summary>
		std::wstring groupKey;
	};

	/// <summary>
	/// ZDock 主窗口（阶段一）：贴主屏底边的半透明圆角面板 + 一行图标。
	///
	/// 两条设计约束直接决定实现形态，改代码时别绕开：
	///  1) 悬停放大要求图标溢出面板，所以**窗口矩形比面板大**（上方留 halo），
	///     那圈透明像素必须在 onHitTest 里返回 HTTRANSPARENT 放行给下面的窗口；
	///  2) 放大动画只动 Composition 的 Scale，不 relayout、不 repaint。
	///
	/// 阶段三起多了一条：窗口跟踪**事件驱动**（shell hook + 少量 win event），
	/// 绝不轮询（红线 4）。所有变化都由 WindowTracker 的回调推进来。
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
		static constexpr int   kMenuPin = 107;        // 把临时图标固定下来
		static constexpr int   kMenuCloseWindow = 108; // 关掉分组里的某个窗口
		static constexpr int   kMenuCloseGroup = 109;  // 关掉分组里所有窗口

		// 运行指示器：图标正下方的 4px 圆点（逻辑像素）。
		// ⚠ 它必须画在**独立节点**上（IndicatorNode），不能挤进 IconNode 的
		//   surface —— 那张 surface 会随悬停放大动画一起缩放，而指示器要恒定大小。
		static constexpr float kIndicatorDia = 4.f;
		static constexpr float kIndicatorGap = 3.f;  // 图标底边到圆点的间距

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

		// ---- 阶段三：窗口跟踪 / 运行状态 ----
		/// <summary>启动跟踪器并挂回调。create() 里调一次。</summary>
		void startTracker();
		/// <summary>
		/// 让图标列表与当前运行的应用对齐：
		///  · 给固定项找分组、挂上运行指示器
		///  · 给"在跑但没被固定"的应用补临时图标
		///  · 去掉已经没有窗口的临时图标
		/// 只在**分组集合**变化时做结构增删；否则只刷指示器（避免每次事件都重建节点）。
		/// </summary>
		void syncWithTracker();
		/// <summary>只刷运行指示器的亮/灭与前台高亮，不动节点结构。</summary>
		void refreshIndicators();
		/// <summary>按图标当前的节点坐标把指示器摆到图标正下方（每次 layout 后调）。</summary>
		void placeIndicators();
		/// <summary>删掉一项的节点（图标 + 指示器）。</summary>
		void destroyItemNode(DockItem& item);
		/// <summary>项数变化后重排布局：重算间距/面板宽度/窗口尺寸与位置（不销毁节点）。</summary>
		void relayoutForItemCount();
		/// <summary>点图标：已运行 → 切窗口（多窗口弹列表）；未运行 → 启动。</summary>
		void clickItem(int index);
		/// <summary>多窗口分组的窗口列表菜单（标题 + 关窗）。</summary>
		void showWindowListMenu(int index);
		/// <summary>把临时图标固定下来（写 config.json + 转正）。</summary>
		void pinItem(int index);
		/// <summary>关闭某分组里的某个窗口（先 WM_CLOSE，超时不强杀）。</summary>
		static void closeWindow(HWND hwnd);
		/// <summary>取某图标对应的分组（没有则 nullptr）。</summary>
		const AppGroup* groupOfItem(size_t index) const;

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

		WindowTracker tracker;
		/// 跟踪器最近一次通知时的"运行中分组键集合"，用来判断要不要做结构增删。
		std::vector<std::wstring> lastRunningKeys;
	};

} // namespace zdock
