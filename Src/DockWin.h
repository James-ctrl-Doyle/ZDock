#pragma once
#include <include/WinBase.h>
#include "IconNode.h"
#include "IndicatorNode.h"
#include "WindowTracker.h"
#include "EdgeHotZone.h"
#include "AppBarReserve.h"
#include "PreviewWin.h"
#include <vector>
#include <string>
#include <filesystem>

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
		static constexpr int   kTimerAutoHide = 2;  // 自动隐藏延迟 / 滑动补间（阶段四）
		static constexpr int   kTimerSlide = 3;     // 滑动动画补间（阶段四）
		static constexpr int   kTimerRelayout = 4;  // 显示环境变化后的重排（延迟一拍，见下）
		static constexpr int   kTimerPreview = 5;   // 悬停预览的 300ms 延迟（阶段五）
		/// 悬停多久之后弹预览（任务书 §2 #17：约 300ms）
		static constexpr int   kPreviewDelayMs = 300;
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

		// =====================================================================
		// 阶段四：自动隐藏 / 全屏让位 / 工作区预留
		// =====================================================================

		// ---- 窗口定位（阶段四抽出来，替代原先三份重复代码）----

		/// <summary>
		/// dock 所在监视器的完整矩形（rcMonitor，物理像素）。
		/// ⚠ 定位 dock 一律以它为基准 —— 不要用 SPI_GETWORKAREA，那会被
		/// AppBar 预留改掉，构成自引用反馈回路（见 .cpp 里 dockRectShown 注释）。
		/// </summary>
		RECT monitorRect() const;

		/// <summary>
		/// dock 面板在屏幕上的目标矩形（物理像素，**展开态**）。
		/// 底边 = 屏幕底边 − bottomMargin，水平居中于屏幕。
		/// </summary>
		RECT dockRectShown() const;

		/// <summary>
		/// 应用窗口位置：`offsetY` 是额外的垂直位移（物理像素，正数 = 往下）。
		/// 展开态传 0；隐藏态传"面板高度 + 一点余量"。
		/// 内部会调 setSize/setPosition（都是物理像素，不乘 dpi）。
		/// </summary>
		void applyDockPlacement(int offsetY = 0);

		/// <summary>把窗口摆到"完全滑出屏幕"的隐藏位（顶边 = 屏幕底边）。</summary>
		void applyDockPlacementHidden();

		/// <summary>
		/// 显示环境变化（DPI / 分辨率 / 主屏切换）后的重排：按新 dpi 与监视器
		/// 重算尺寸位置，并把热区、AppBar 一起带过去。保持当前的显示/隐藏状态。
		/// </summary>
		void relayoutForEnvironment();

		// ---- 自动隐藏状态机 ----

		/// <summary>
		/// 从配置刷新自动隐藏相关的缓存，并按需要创建 / 销毁热区窗口。
		/// create() 与 reloadConfig() 后各调一次。
		/// </summary>
		void applyAutoHideConfig();

		/// <summary>执行滑入（展开）。热区触发 / 鼠标回到面板上时调。</summary>
		void slideIn();

		/// <summary>执行滑出（隐藏）。延迟到期后调。</summary>
		void slideOut();

		/// <summary>当前是否处于"已隐藏"（滑出完成）状态。</summary>
		bool hidden() const { return slideState == SlideState::Hidden; }

		/// <summary>
		/// 记一次"还在用 dock"（鼠标在面板上 / 菜单开着 / 悬停中）。
		/// 会取消待执行的滑出。
		/// </summary>
		void keepVisible();

		/// <summary>鼠标离开面板时调：启动 500ms 延迟滑出。</summary>
		void scheduleHide();

		/// <summary>滑动动画的定时器回调（补间）。</summary>
		void tickSlide();

		/// <summary>
		/// 当前是否应该隐藏（自动隐藏开着 且 （全屏应用在前台 且 hideOnFullscreen））。
		/// 注意"鼠标还在面板上"不算 —— 那由 keepVisible 的延迟管。
		/// </summary>
		bool shouldHideNow() const;

		/// <summary>
		/// 鼠标此刻是否压在 dock 窗口上（WindowFromPoint，会考虑 halo 穿透 region）。
		/// ⚠ 只在滑入动画结束这类离散时刻调，**不轮询**（红线 4）。
		/// </summary>
		bool cursorOverDock() const;

		/// <summary>热区窗口该在的屏幕矩形（物理像素）。</summary>
		RECT hotZoneRect() const;

		/// <summary>按当前状态重建 / 移动 / 销毁热区窗口。</summary>
		void syncHotZone();

		/// <summary>全屏状态变化时的处理（隐藏 / 恢复）。</summary>
		void onFullscreenChanged(bool on);

		// ---- 工作区预留（AppBar）----

		/// <summary>按配置注册 / 注销 AppBar。返回是否处于已注册态。</summary>
		bool syncAppBar();

		/// <summary>强杀自愈用的状态文件路径（exe 同目录 zdock-appbar.state）。</summary>
		std::wstring appBarStatePath() const;

		/// <summary>explorer 重启（TaskbarCreated 广播）后的自愈：重注册 AppBar + 跟踪器。</summary>
		void onTaskbarCreated();

		// ---- 拖放（阶段五）----

		/// <summary>
		/// 窗口过程的**子类化**回调（阶段五）。
		///
		/// ⚠ 为什么需要子类化：Ling 的 `WinBase` 只暴露少量可覆写点和一组 winrt 事件，
		///   **没有通用消息钩子**，也不处理 `WM_DROPFILES`。而拖放是"消息进窗口过程"
		///   才能拿到的东西。三条路里子类化是唯一不动 Ling、也不和 dock 自己的
		///   命中测试打架的做法（另两条：改 Ling 加钩子 / 用覆盖窗口收拖放 ——
		///   后者会挡住 dock 的 hover）。
		///
		/// 实测（`build-support/_probe_subclass.cpp`）：子类化后能收到投给该窗口的消息、
		///   Ling 存在 `GWLP_USERDATA` 里的 self 指针不受影响、原 wndProc 转发链正常。
		/// </summary>
		static LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

		/// <summary>处理 WM_DROPFILES：把拖进来的文件用落点那个图标对应的程序打开。</summary>
		void onDropFiles(HDROP drop);

		/// <summary>路径是不是目录（决定"用程序打开"还是"复制进去"）。</summary>
		static bool isDirectoryPath(const std::wstring& path);

		// ---- 悬停预览（阶段五）----

		/// <summary>
		/// 悬停满 kPreviewDelayMs 后弹预览：取该图标对应应用的窗口，
		/// 用 DWM 缩略图显示实时画面（任务书 §2 #17）。
		/// </summary>
		void showPreviewForHover();

		/// <summary>取消待弹出的预览 + 收起已显示的预览（鼠标离开图标时调）。</summary>
		void cancelPreview();

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

		// ---- 阶段四状态 ----
		enum class SlideState { Shown, Sliding, Hidden };

		/// 来自 config.json 的自动隐藏参数（逻辑像素 / 毫秒）
		bool  cfgAutoHide{ false };
		int   cfgHideDelayMs{ 500 };
		int   cfgSlideInMs{ 200 };
		int   cfgSlideOutMs{ 300 };
		bool  cfgHideOnFullscreen{ true };
		bool  cfgReserveWorkArea{ false };

		SlideState slideState{ SlideState::Shown };
		/// 滑动的进度（0 = 完全展开，1 = 完全滑出）与本次滑动的起止/时刻
		float slideT{ 0.f };
		float slideFrom{ 0.f };
		float slideTo{ 0.f };
		ULONGLONG slideStartTick{ 0 };
		int   slideDurMs{ 300 };
		bool  slideTimerOn{ false };

		/// 自动隐藏延迟定时器是否开着（500ms 那条）
		bool  hideDelayTimerOn{ false };
		/// 展开态下窗口的左上角（物理像素）。滑动时以它为基准做偏移。
		POINT shownOrigin{ 0, 0 };

		EdgeHotZone hotZone;
		AppBarReserve appBar;

		/// 悬停预览气泡（独立顶层窗口，DWM 缩略图做画面 —— 见 PreviewWin.h）
		PreviewWin preview;
		/// 300ms 预览延迟的定时器是否挂着
		bool previewTimerOn{ false };

		/// 当前是否有全屏应用在前台。来源 = WindowTracker 的 onFullscreenChanged
		/// （自动化测试也会通过热区的注入通道喂它，见 EdgeHotZone::onTestInject）。
		/// ⚠ 用它而不是直接问 tracker：tracker 的状态更新有自己的时机，
		///   而 dock 的判定必须和"最后一次收到的全屏通知"严格一致，
		///   否则注入测试时会出现"通知说全屏、tracker 说没有"的分裂。
		bool fullscreenNow{ false };

		/// 菜单 / 拖放会话计数（>0 时绝不隐藏）。
		/// 菜单是模态的，弹之前 +1、弹完 −1。
		int menuSessions{ 0 };

		/// 子类化前的原窗口过程（Ling 的）。子类化回调里要 CallWindowProc 转回去。
		WNDPROC origWndProc{ nullptr };

		/// 子类化回调里取 this 用。⚠ 不能读 GWLP_USERDATA —— 那里存的是 **Ling 自己的**
		/// `WinBase*`（`build-support/_probe_subclass.cpp` 实测确认），
		/// 拿它向下转型虽然当下能用，但那是依赖 Ling 的内部布局。
		static DockWin* s_self;
	};

} // namespace zdock
