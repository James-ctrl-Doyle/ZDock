#include "DockWin.h"
#include "AutoStart.h"
#include "Config.h"
#include "MonitorUtil.h"
#include "IconLoader.h"
#include "Log.h"
#include <include/App.h>
#include <include/D2D.h>

#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <commdlg.h>   // GetOpenFileNameW（"添加程序"的文件选择框）
#include <format>

namespace zdock {

	// 子类化回调里取 this 用（单实例程序，静态指针够用）。
	// ⚠ 不读 GWLP_USERDATA —— 那里是 Ling 自己的 WinBase*。
	DockWin* DockWin::s_self = nullptr;

	DockWin::DockWin() = default;

	DockWin::~DockWin()
	{
		// ⚠ 跟踪器的钩子必须在窗口销毁**之前**摘掉：它的回调会用到本对象，
		//   对象没了还留着钩子就是悬空指针。
		tracker.stop();
		// 设置窗口：关掉时会把配置写回 config.json
		settings.close();
		// 热区窗口是独立顶层窗口，也得显式销毁（它不属于 Ling 的窗口体系）
		hotZone.destroy();
		// AppBar 注销：不注销的话工作区会一直缩着，用户得重启 explorer 才好。
		appBar.unregister_();
	}

	float DockWin::iconsAlong() const
	{
		const float n = static_cast<float>(items.size());
		if (n <= 0) return 0.f;
		return n * cfgIconBase + (n - 1) * cfgIconGap;
	}

	float DockWin::panelAlong() const
	{
		if (items.empty()) return 2 * kPadX;
		return iconsAlong() + 2 * kPadX;
	}

	float DockWin::panelAcross() const
	{
		// 面板厚度方向多留一条指示器的空间：指示器画在图标之外（不挤压图标），
		// 但它落在面板范围内，否则会悬在面板外面很难看。
		return cfgIconBase + 2 * kPadY + px(kIndicatorDia + kIndicatorGap) / (dpi > 0.f ? dpi : 1.f);
	}

	void DockWin::slideDir(int& dx, int& dy) const
	{
		dx = 0;
		dy = 0;
		switch (cfgEdge) {
		case DockEdge::Bottom: dy = +1; break;   // 向下滑出
		case DockEdge::Top:    dy = -1; break;   // 向上滑出
		case DockEdge::Left:   dx = -1; break;   // 向左滑出
		case DockEdge::Right:  dx = +1; break;   // 向右滑出
		}
	}

	void DockWin::panelOrigin(float& ox, float& oy) const
	{
		// halo 留在"远离屏幕边"的那一侧 —— 图标朝那个方向放大、溢出到那块留白里。
		switch (cfgEdge) {
		case DockEdge::Bottom: ox = kSideSlack; oy = kHaloH;     break;  // halo 在上
		case DockEdge::Top:    ox = kSideSlack; oy = 0.f;        break;  // halo 在下
		case DockEdge::Left:   ox = 0.f;        oy = kSideSlack; break;  // halo 在右
		case DockEdge::Right:  ox = kHaloH;     oy = kSideSlack; break;  // halo 在左
		}
	}

	bool DockWin::inRect(POINT pt, float x, float y, float w, float h)
	{
		return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
	}

	// ---------------------------------------------------------------------------
	// 把 config.json 的值搬进缓存成员。create() 最开始调一次。
	// 热改配置需要重启进程 —— 阶段二先这样，做设置界面时再谈动态生效。
	// ---------------------------------------------------------------------------
	void DockWin::applyConfig()
	{
		auto* cfg = Config::get();
		cfgIconBase = cfg->iconSize;
		cfgIconGap = cfg->iconGap;
		cfgHoverPeak = cfg->hoverScale;
		cfgAnimMs = cfg->animMs;
		cfgAutoHide = cfg->autoHide;
		cfgHideDelayMs = cfg->autoHideDelayMs;
		cfgSlideInMs = cfg->slideInMs;
		cfgSlideOutMs = cfg->slideOutMs;
		cfgHotZoneThickness = cfg->hotZoneThickness;
		cfgHotZoneSpan = HotZoneSpan::Half;
		if (cfg->hotZoneSpan == L"dock") cfgHotZoneSpan = HotZoneSpan::Dock;
		else if (cfg->hotZoneSpan == L"full") cfgHotZoneSpan = HotZoneSpan::Full;
		cfgHideOnFullscreen = cfg->hideOnFullscreen;
		cfgReserveWorkArea = cfg->reserveWorkArea;

		// 阶段六：位置 / 外观。字符串→枚举的兜底在 Config::load() 里已经做过
		// （认不出的值会记日志并保留默认），这里只做映射。
		cfgEdge = DockEdge::Bottom;
		if (cfg->dockEdge == L"top") cfgEdge = DockEdge::Top;
		else if (cfg->dockEdge == L"left") cfgEdge = DockEdge::Left;
		else if (cfg->dockEdge == L"right") cfgEdge = DockEdge::Right;

		cfgAlign = DockAlign::Center;
		if (cfg->dockAlign == L"start") cfgAlign = DockAlign::Start;
		else if (cfg->dockAlign == L"end") cfgAlign = DockAlign::End;

		cfgOffset = cfg->dockOffset;
		cfgOpacity = cfg->opacity;
		cfgShowIndicator = cfg->showIndicator;
		cfgMonitorIndex = cfg->monitorIndex;
		cfgAutoStart = cfg->autoStart;
	}

	void DockWin::applyLiveConfig()
	{
		applyConfig();

		// 外观类的东西可以直接改节点属性，不必重建节点
		if (panel) {
			const uint32_t c = Config::get()->bgColorValue();
			const uint32_t a = static_cast<uint32_t>(
				std::lround(std::clamp(cfgOpacity, 0.f, 1.f) * 255.f));
			panel->setBg(Ling::Color((c & 0xFFFFFF00u) | a));
			panel->setBorderRadius(Config::get()->cornerRadius);
		}

		// 尺寸 / 位置 / 排列方向 / 指示器位置 —— 一套重排全带走
		relayoutForItemCount();
		refreshIndicators();
		applyAutoHideConfig();
	}

	// ---------------------------------------------------------------------------
	// 图标列表来自 config.json（首次启动时 Config 会写入内置默认表）。
	// ---------------------------------------------------------------------------
	void DockWin::collectItemsFromConfig()
	{
		auto* cfg = Config::get();
		for (const auto& ci : cfg->items) {
			DockItem item;
			item.path = ci.path;
			item.name = ci.name.empty() ? displayNameOf(ci.path) : ci.name;
			// ⚠ 别漏了这一行：pinRight 决定这个节点进左组还是右组，
			//   漏了的话"回收站"会排到左组里、临时图标反而跑到它右边。
			item.pinRight = ci.pinRight;
			items.push_back(std::move(item));
		}
		log(std::format(L"[dock] 配置里 {} 项，生效 {} 项", cfg->items.size(), items.size()));
	}

	void DockWin::create()
	{
		// 配置必须最先载入：applyConfig / collectItemsFromConfig 都读 Config 的内存态。
		// 文件不存在时 load() 会顺手把默认配置写出去，用户能看到有哪些可调项。
		Config::get()->load();
		applyConfig();
		collectItemsFromConfig();

		// 窗口尺寸 = 面板 + 四周留白（留白分布随停靠边换轴，与 relayoutForItemCount 同一套公式）。
		// ⚠ WinBase::setSize 收的是**逻辑**值（内部 ×dpi）；setPosition 收的是**物理**值。
		const float winW = horizontalEdge() ? (panelW() + 2 * kSideSlack) : (panelW() + kHaloH);
		const float winH = horizontalEdge() ? (panelH() + kHaloH) : (panelH() + 2 * kSideSlack);
		setSize(winW, winH);

		// 定位统一走 applyDockPlacement（阶段四抽出来：原先 create/rebuild/
		// relayoutForItemCount 各抄了一份，自动隐藏要在位置上做偏移，必须先收口）。
		applyDockPlacement(0);
		log(std::format(L"[dock] dpi={:.2f} 窗口物理 {}x{} @ ({},{})", dpi, w, h, x, y));

		// 独立顶层窗口：不进 Alt+Tab/任务栏、永远置顶、点击面板不抢焦点。
		// ⚠ 绝不 SetParent 到桌面 —— 那是把 explorer 拖垮的那条路（红线 1/2/3）。
		createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
		log(std::format(L"[dock] hwnd={}", reinterpret_cast<unsigned long long>(hwnd)));
		if (!hwnd) return;

		loadIcons();

		// winrt::event 用 add()/remove()，没有 operator+=
		//
		// ⚠ 这个定时器**只**用来兜底察觉"鼠标已经离开"。
		//   它绝不能用"上次 WM_NCHITTEST 的时间戳"当心跳：WM_NCHITTEST
		//   只在鼠标移动时才来，光标停住就断了，那样会 150ms 一循环地
		//   把图标放大-缩回-放大（用户看到的"来回变大变小"）。
		//   判断依据必须是**光标的真实位置**，见 refreshHoverFromCursor()。
		onTimer.add([this](UINT id) {
			if (id == kTimerAutoHide) {
				// 延迟到期：再确认一次"确实没人用 dock"才滑出。
				// 这段时间里用户可能又移回来了（keepVisible 会 kill 掉这个定时器）。
				killTimer(kTimerAutoHide);
				hideDelayTimerOn = false;
				if (menuSessions > 0 || hoverIndex >= 0) {
					log(std::format(L"[dock] 收起取消：menuSessions={} hoverIndex={}", menuSessions, hoverIndex));
					return;
				}
				if (refreshHoverFromCursor()) {
					log(L"[dock] 收起取消：光标还在 dock / 热区上");
					return;
				}
				slideOut();
				return;
			}
			if (id == kTimerSlide) {
				tickSlide();
				return;
			}
			if (id == kTimerRelayout) {
				// 显示环境变化（DPI / 分辨率 / 主屏切换）后的重排。
				// ⚠ 为什么要延后一拍、不直接在事件回调里做：
				//   Ling 的 `WinBase::dpiChange()` 是「先 onDpiChanged() 回调、**之后**
				//   才 SetWindowPos(系统建议矩形)」—— 在回调里设的位置会被它当场覆盖。
				//   挂个 1ms 的一次性定时器，让重排落在那次 SetWindowPos 之后。
				//   （一次性定时器不是轮询，红线 4 允许。）
				killTimer(kTimerRelayout);
				relayoutForEnvironment();
				return;
			}
			if (id != kTimerHover) return;
			// ⚠⚠ 这里**不能只看 hoverIndex**（原逻辑：hoverIndex < 0 就关掉定时器）。
			//   **从热区唤出的 dock** 的 hoverIndex 始终是 -1 —— 按旧逻辑，定时器一关，
			//   这台 dock 就**完全没有"离开检测"**了，只能靠热区的 onLeave 一个信号，
			//   那个信号一旦漏掉/来晚，dock 就永远赖着展开（用户 2026-10-13 报的
			//   "鼠标移到屏幕中间很久，dock 还是不隐藏"）。
			//   所以：**只要 dock 是展开态、且自动隐藏开着，就一直保持这个兜底检测**。
			const bool needWatch = hoverIndex >= 0
				|| (cfgAutoHide && slideState != SlideState::Hidden);
			if (!needWatch) {
				ensureHoverTimer(false);
				return;
			}
			// 这一句会按"光标到底在不在 dock / 热区上"决定 keepVisible() 还是 scheduleHide()
			refreshHoverFromCursor();
			if (!(cfgAutoHide && slideState != SlideState::Hidden) && hoverIndex < 0) {
				ensureHoverTimer(false);
			}
			});

		onMouseUp.add([this](POINT pt, bool right) {
			if (right) {
				// 右键落点先看图标：命中就给**该项专属**菜单，否则给全局菜单。
				// 用 indexAtVisual 而不是 indexAtHover —— 放大后的那块可见框
				// 才是用户"看着点在图标上"的范围。
				const int idx = indexAtVisual(pt);
				if (idx >= 0) showItemContextMenu(idx);
				else showContextMenu();
				return;
			}
			const int idx = indexAtVisual(pt);
			if (idx >= 0) {
				// 按下反馈的抬起（按下是 onMouseDown 里给的）
				if (items[idx].node) items[idx].node->setPressed(false);
				clickItem(idx);
			}
			});

		// 按下反馈：缩到 0.92（任务书 §3）。只给被按的那个图标。
		onMouseDown.add([this](POINT pt, bool right) {
			if (right) return;
			const int idx = indexAtVisual(pt);
			if (idx >= 0 && items[idx].node) items[idx].node->setPressed(true);
			});

		// 鼠标在面板上动 → 说明用户在用它，取消待执行的滑出（并滑入，如果还是隐藏态）。
		onMouseMove.add([this](POINT) { keepVisible(); });

		// DPI 变化（改系统缩放比、跨不同 DPI 的显示器）→ 重排。
		// ⚠ 两件事都在这里发生，缺一不可：
		//   1) Ling 已经更新了 `dpi` 与节点资源（WinBase::dpiChange 里先做）；
		//   2) 但它的 `dpiChange()` 在回调**之后**还会 SetWindowPos(系统建议矩形)
		//      —— 那个矩形是按"窗口原物理尺寸等比缩放"给的，对内容自适应的 dock
		//      是错的（位置会被推到屏幕中间）。所以这里只挂一个 1ms 的一次性
		//      定时器，把真正的重排推到那条 SetWindowPos 之后执行。
		// ⚠ `px()` 依赖 `dpi`：dpi 变了，窗口尺寸也得重算，光改位置不够
		//   （否则 200% 缩放下图标会只占一半大小）。
		onDpiChanged.add([this] {
			log(std::format(L"[dock] DPI 变化 -> {:.2f}", dpi));
			setTimer(1, kTimerRelayout);
			});
		// 让探针能据日志确认这条订阅挂上了（DPI 变化的真实触发要改系统缩放，
		// 自动化测试造不了；WM_DPICHANGED 的 lParam 是指针，跨进程投递无效）。
		log(L"[dock] 已订阅 DPI 变化（Ling onDpiChanged）");

		// 窗口没了就退进程：否则 DestroyWindow 之后消息循环还在空转，留一个
		// "没窗口没托盘"的僵尸进程（ZPin 那边踩过同款）
		onDestroy.add([] { Ling::App::get()->quit(0); });

		// ---- 拖放（阶段五）----
		// ⚠ 必须子类化：Ling 的 WinBase 没有通用消息钩子，`WM_DROPFILES` 走不到我们手里。
		//   实测（_probe_subclass.cpp）：子类化后能收消息、Ling 的 self 指针不受影响、
		//   原 wndProc 转发链正常。挂在自己的 hwnd 上，不碰任何别的窗口。
		s_self = this;
		origWndProc = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DockWin::subclassProc)));
		if (!origWndProc) {
			log(std::format(L"[drop] 子类化失败 err={} —— 拖放不可用", GetLastError()));
		}
		else {
			DragAcceptFiles(hwnd, TRUE);
			log(L"[drop] 已注册拖放受体（子类化 + DragAcceptFiles）");
		}

		// 设置窗口（阶段六）：改动即时生效 —— 直接走 applyLiveConfig（只重排、不重建）
		settings.onLog = [](const std::wstring& s) { log(s); };
		settings.onLiveChanged = [this] {
			applyLiveConfig();
			// 设置窗口开着的时候别把 dock 收走 —— 用户正盯着它看改动的效果
			if (settings.isOpen()) keepVisible();
			};

		// 开机自启：以配置为准同步一次注册表。
		// ⚠ 放在这儿而不是 main.cpp —— 配置是在 create() 里才载入的，
		//   在 main 里读 Config 会拿到还没载入的默认值。
		//   用户可能直接手改过 config.json（或从别处关掉过自启），这里对齐一次。
		if (autostart::sync(cfgAutoStart)) {
			if (cfgAutoStart) log(L"[autostart] 启动时已确保自启开启");
		}
		else {
			log(L"[autostart] 启动时同步自启失败（注册表不可写？）");
		}

		layout();   // 立刻布一次局，别等第一次 WM_PAINT
		updateHitRegion();   // 初始命中区域 = 面板本体（halo 完全不参与命中）
		placeIndicators();   // 图标坐标已定，把指示器摆到位
		refresh();

		// 跟踪器放最后启动：它一起来就会回调 syncWithTracker()，
		// 那时界面必须已经完整（panel/row/图标都就位）。
		startTracker();

		// 阶段四：自动隐藏与工作区预留都依赖"窗口已经就位 + 跟踪器已能报全屏"，
		// 所以放在最后。默认两个都是关的 → 行为与阶段三完全一致。
		applyAutoHideConfig();

		// 启动即收起（用户 2026-10-12 提）：开了自动隐藏，就别让 dock 一上来
		// 就横在屏幕上等人来碰 —— 之前它总是以展开态启动，要等鼠标扫过一次才收。
		// ⚠ 直接落位、**不播滑出动画**：启动那一下播动画只是拖慢"进入可用状态"。
		// ⚠ 放在 syncAppBar() **之前** —— AppBar 的批准矩形按 slideState 算
		//   （见 syncAppBar 里 `slideState == Hidden ? h : 0`），顺序反了会拿展开态
		//   去注册，工作区被多推一格。
		if (cfgAutoHide && !shouldHideNow()) {
			slideState = SlideState::Hidden;
			slideT = 1.f;
			applyDockPlacementHidden();
			log(L"[dock] 启动即收起（自动隐藏已启用）");
		}
		// ⚠ 先自愈再注册：把上次强杀可能留下的工作区占用清掉（红线 7）。
		//   顺序不能反 —— 否则我们会基于一个"被污染的工作区"去算位置，
		//   而且新注册会与残留叠加（实测工作区会被吃掉两份）。
		//
		// ⚠⚠ 自愈做了两件事（顺序不能变）：
		//   1) 用状态文件里备份的值把工作区 `SPI_SETWORKAREA` 写回干净值；
		//   2) 跑一次 `purgeStaleRecord()`，把 shell 里那条死 AppBar 记录扫掉
		//      —— 光改数值不够，死记录会让接下来的注册算成"残留 + 新"，
		//      工作区被占两道（实测干净底 1440 → 1257，占 183px 而不是 95px）。
		//   两步都在 recoverStaleWorkArea 里完成了，之后照常注册即可，功能不降级。
		const bool staleFound =
			AppBarReserve::recoverStaleWorkArea([](const std::wstring& s) { log(s); }, appBarStatePath());
		if (staleFound) log(L"[appbar] 上次强杀残留已清理，本次正常注册");
		syncAppBar();
	}

	void DockWin::onCreated()
	{
		// 面板背景：半透明 + 圆角。
		// ⚠ 它必须和图标**平级**而不是图标的父节点 —— 面板的圆角是几何 clip，
		// 图标放大后要溢出面板，挂在面板下面就一起被剪掉了。
		panel = body->makeChild<Ling::Node>();
		panel->setPositionType(Ling::Position::Absolute);
		// 背景色与圆角都来自 config.json。
		// ⚠ bgColor 只贡献 RGB，**透明度由 opacity 决定**（任务书 §3：0.5~0.95 可调）。
		//   两者字节序都是 0xRRGGBBAA（与 Ling::Color 一致）。
		{
			const uint32_t c = Config::get()->bgColorValue();
			const uint32_t a = static_cast<uint32_t>(
				std::lround(std::clamp(cfgOpacity, 0.f, 1.f) * 255.f));
			panel->setBg(Ling::Color((c & 0xFFFFFF00u) | a));
		}
		panel->setBorderRadius(Config::get()->cornerRadius);
		panel->setBorder(1.f, Ling::Color(0xFFFFFF14));

		row = body->makeChild<Ling::Node>();
		row->setPositionType(Ling::Position::Absolute);
		rowTemp = body->makeChild<Ling::Node>();
		rowTemp->setPositionType(Ling::Position::Absolute);
		rowRight = body->makeChild<Ling::Node>();
		rowRight->setPositionType(Ling::Position::Absolute);

		// 面板 / 图标行的位置、尺寸、排列方向全部随停靠边 —— 收口在这一个函数里
		applyNodeLayout();

		// 指示器**不放进 row** —— row 是 Flex 容器，多出来的节点会被当成第二个
		// 图标参与排版。它们挂在 body 上，位置在 layout 完成后按图标坐标算。
		//
		// ⚠ 固定项按 pinRight 分到两个容器（左组 / 右组），这样"右组"能排到
		//   临时图标后面去（见 rowTemp 的说明）。
		for (size_t i = 0; i < items.size(); ++i) {
			auto& it = items[i];
			Ling::Node* host = it.pinRight ? rowRight : row;
			auto* node = host->makeChild<IconNode>();
			node->setSize(cfgIconBase, cfgIconBase);
			// 间距加在**沿边方向**：横向边是右间距，纵向边是下间距。
			// ⚠ 判断"是不是本段最后一个"（段间距由 applyNodeLayout 补），
			//   不能直接看"是不是 items 最后一个"。
			if (!isLastInSegment(i)) {
				if (horizontalEdge()) node->setMarginRight(cfgIconGap);
				else node->setMarginBottom(cfgIconGap);
			}
			it.node = node;

			auto* dot = body->makeChild<IndicatorNode>();
			dot->setPositionType(Ling::Position::Absolute);
			dot->setSize(kIndicatorDia, kIndicatorDia);
			dot->setOn(false);
			it.indicator = dot;
		}
	}

	void DockWin::loadIcons()
	{
		auto* ctx = Ling::D2D::get()->deviceContext.Get();
		if (!ctx) {
			log(L"[dock] D2D deviceContext 为空，图标无法加载");
			return;
		}
		// 源位图按**峰值尺寸**取：放大到峰值时 1:1 采样，不会糊。
		const int targetPx = static_cast<int>(std::lround(px(cfgIconBase) * cfgHoverPeak));
		for (auto& item : items) {
			if (!item.node) continue;
			log(std::format(L"[dock] 提取图标：{}（{}）", item.name, item.path));
			auto bmp = loadShellIcon(ctx, item.path, targetPx);
			log(std::format(L"[dock]   位图 {}", bmp ? L"ok" : L"**失败，将画兜底灰块**"));
			item.node->setBitmap(std::move(bmp));
		}
		log(std::format(L"[dock] 图标加载完毕 targetPx={}", targetPx));
	}

	int DockWin::indexAtHover(POINT pt) const
	{
		const float half = px(cfgIconGap) * 0.5f;
		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node || node->w <= 0.f) continue;
			if (pt.x >= node->x - half && pt.x <= node->x + node->w + half) return static_cast<int>(i);
		}
		return -1;
	}

	int DockWin::indexAtVisual(POINT pt) const
	{
		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node || node->w <= 0.f) continue;
			const float s = node->scale();
			const float left = node->x + node->w * 0.5f - node->w * s * 0.5f;
			const float top = node->y + node->h - node->h * s;
			if (pt.x >= left && pt.x < left + node->w * s && pt.y >= top && pt.y < node->h + node->y) return static_cast<int>(i);
		}
		return -1;
	}

	// ---------------------------------------------------------------------------
	// 按**光标的真实位置**重算 hover。返回 true 表示 hover 发生了变化。
	//
	// 为什么必须这样，而不是靠消息驱动：
	//   · WM_NCHITTEST 只在鼠标**移动**时才有 —— 光标停住不动时它完全不来，
	//     所以"上次命中时间戳"不能当心跳（那正是图标来回缩放的病根）。
	//   · WM_MOUSELEAVE 只保证"离开窗口"这一次，而我们的窗口形状是被
	//     SetWindowRgn 动态挖过的（halo 区域不属于窗口）—— 鼠标从图标上
	//     移到面板外、但仍在窗口矩形里的那些像素上时，不会触发 MOUSELEAVE，
	//     却应该判定为"离开了图标"。
	//   · 所以这里用 GetCursorPos + WindowFromPoint 直接问系统：
	//     "光标的 client 坐标是多少、这个点还算不算我的窗口"。
	//     这是**事件驱动 + 单次查询**，不是轮询（红线 4）；查的也是自身
	//     UI 状态，不碰也不注入任何外部进程。
	// ---------------------------------------------------------------------------
	bool DockWin::refreshHoverFromCursor()
	{
		if (!hwnd) return false;

		POINT pt{};
		if (!GetCursorPos(&pt)) return false;

		int idx = -1;
		// 光标必须真的落在 dock 的 client 区里才算悬停。
		// ⚠ WindowFromPoint 会忽略被 region 挖掉的像素，正好符合我们的定义：
		//   halo 那圈透明区不算"在 dock 上"。
		HWND under = WindowFromPoint(pt);
		const bool overDock = (under == hwnd);
		// ⚠⚠ 热区也算"贴着 dock"（理由见 `cursorOnHotZone` 的说明）：热区是独立窗口
		//   且压在最上层，鼠标停在停靠边唤出 dock 时 WindowFromPoint 命中的是**热区**
		//   而不是 dock。只看 overDock 会把它误判成"鼠标已离开" → dock 刚升起 500ms
		//   就被 scheduleHide 收回去（用户 2026-10-12 报的"贴底边 dock 不升起"，
		//   日志里其实是"滑入 200ms 后紧跟滑出"）。`cursorOverDock()` 早就有这条判定，
		//   这里漏了 —— 两处口径必须一致，所以现在共用 `cursorOnHotZone()`。
		const bool overHotZone = cursorOnHotZone(pt);
		if (overDock) {
			POINT client = pt;
			ScreenToClient(hwnd, &client);
			idx = indexAtHover(client);
		}
		// 阶段四：自动隐藏的"离开"判定就挂在这里。
		// ⚠ 这里是**事件驱动 + 单次查询**（由 WM_NCHITTEST / 定时器兜底触发），
		//   不是轮询：我们只是借"系统刚好在问命中"这个时机顺便看了一眼光标。
		//   任务书红线 4 的例外条款明确允许"自动隐藏延迟这类自身 UI 状态"。 
		if (cfgAutoHide && slideState != SlideState::Hidden) {
			if (overDock || overHotZone || menuSessions > 0) keepVisible();
			else scheduleHide();
		}

		if (idx == hoverIndex) return false;

		hoverIndex = idx;
		applyHover(idx);
		// ⚠⚠ 兜底定时器要在**两种情况**下都开着：
		//   ① 鼠标在图标上（hoverIndex >= 0）—— 本来就有；
		//   ② **dock 处于展开态且开了自动隐藏** —— 从**热区**唤出的场景下 hoverIndex
		//      始终是 -1，只认 ① 的话就完全没有"离开检测"（见 kTimerHover 处理的注释）。
		if (idx >= 0 || (cfgAutoHide && slideState != SlideState::Hidden)) {
			ensureHoverTimer(true);
		}
		return true;
	}

	void DockWin::applyHover(int index)
	{
		// 诊断：ZDOCK_VERBOSE_HOVER=1 时把每次 hover 变更记下来。
		// 抖动问题的排查靠它 —— [hit] 那行有去重，看不出"反复 applyHover"。
		static const bool verboseHover = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_HOVER", buf, 8) > 0;
			}();
		if (verboseHover) {
			log(std::format(L"[hover] apply index={} (prev={}) tick={}", index, hoverIndex, GetTickCount64()));
		}

		// ⚠⚠ 衰减距离必须用**图标在屏幕上的实际位置差**，不能用 items 的数组下标差。
		//   数组顺序 ≠ 屏幕排列顺序：
		//     · `pinRight` 的项（回收站）在 items 里紧跟配置顺序，却显示在最右边；
		//     · 运行中的临时项被插在中间段；
		//     · 三段容器（row / rowTemp / rowRight）各自定位。
		//   用下标差会算出"回收站离 Chrome 只有一格"（d=1 → 放大到 1.26 倍），
		//   于是悬停 Chrome 时最右边的回收站跟着一起放大 —— 用户 2026-10-12 报的
		//   "多个图标一起放大"就是这个。
		//   一格 = 图标边长 + 间距（逻辑值，经 px() 转物理，与 node->x/y 同口径）。
		const float slot = px(cfgIconBase + cfgIconGap);
		Ling::Node* ref = nullptr;
		if (index >= 0 && static_cast<size_t>(index) < items.size()) ref = items[index].node;

		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node) continue;
			float target = 1.f;
			if (ref && slot > 0.f) {
				// 沿边方向取值：横向停靠看 x，纵向停靠看 y
				const float delta = horizontalEdge() ? (node->x - ref->x) : (node->y - ref->y);
				const float d = std::abs(delta) / slot;
				target = 1.f + (cfgHoverPeak - 1.f) * std::exp(-(d / kHoverSigma) * (d / kHoverSigma));
			}
			node->animateScale(target, cfgAnimMs);
		}
		// 图标一放大就多占一块可见区域，命中区域跟着变
		updateHitRegion();
	}

	void DockWin::ensureHoverTimer(bool want)
	{
		if (want == hoverTimerOn) return;
		hoverTimerOn = want;
		if (want) setTimer(120, kTimerHover);
		else killTimer(kTimerHover);
	}

	// ---------------------------------------------------------------------------
	// 命中区域 = 面板 + 当前处于放大状态的图标框。
	//
	// ⚠ 为什么必须用窗口 region 而不是 onHitTest 返回 HTTRANSPARENT：
	//   HTTRANSPARENT 的"继续往 z 序下方问"只在**同一线程**的窗口之间成立
	//   （MSDN WM_NCHITTEST 明说；阶段一实测确认：返回 HTTRANSPARENT 后
	//   WindowFromPoint 仍然判给 dock，下层窗口收不到点击）。
	//   窗口 region 是真正的"这块不属于我"：区域外的像素连窗口都不算，
	//   点击直接落到下面的窗口，跨进程有效。
	//
	// 区域只在 hover 变化时重算（不是每帧）：动画期间图标的逻辑缩放已经落到目标值
	//   （IconNode::animateScale 先更新 curScale，动画只是视觉），所以一次算准即可。
	// ---------------------------------------------------------------------------
	void DockWin::updateHitRegion()
	{
		if (!hwnd) return;
		auto toWinRect = [](float x, float y, float w, float h) {
			RECT r{};
			r.left = static_cast<LONG>(std::floor(x));
			r.top = static_cast<LONG>(std::floor(y));
			r.right = static_cast<LONG>(std::ceil(x + w));
			r.bottom = static_cast<LONG>(std::ceil(y + h));
			return r;
			};

		// ⚠⚠ region **必须留足余量**，不能按"当前 scale"精确算。
		//
		//   背景：`SetWindowRgn` 是**会裁剪绘制**的（region 决定系统允许在窗口内绘制的
		//   区域）。而图标放大/缩小是 150ms 的 Composition 缓动动画 ——
		//   **视觉滞后于 `node->scale()` 的返回值**：
		//     · 旧代码用 `if (s <= 1.002f) continue;` 跳过没放大的图标，于是"缩小的那一瞬"
		//       （scale() 已经回到 1.0、但视觉还在峰值大小）那块**完全不进 region**；
		//     · 放大时虽然 region 先变大，但鼠标快速划过时 hover 会在动画没结束时反复变，
		//       前一个还在放大的图标同样会掉出 region。
		//   → 图标被 region 切掉一块（用户 2026-10-12 报的"快速滑动鼠标时图标不完整"，
		//     截图里 Chrome 右上角缺了个口子）。
		//
		//   **与其追着 scale 跑，不如固定留出一个峰值图标的余量**：视觉永远不会被裁，
		//   也不需要每帧重算 region。
		//   ⚠ 多出来的空白没有副作用：那里的 hitTest 返回 HTTRANSPARENT，而
		//     `WindowFromPoint` 会**跳过**这类区域（拿到的仍是下层窗口），
		//     所以 hover / "鼠标还在不在 dock 上"的判定都不受影响。
		const float margin = px(cfgIconBase) * (cfgHoverPeak - 1.f) + px(cfgIconGap);
		RECT panelRect = toWinRect(panel->x - margin, panel->y - margin,
			panel->w + margin * 2.f, panel->h + margin * 2.f);
		HRGN region = CreateRectRgnIndirect(&panelRect);
		if (!region) return;
		// ⚠ 不再逐个图标做并集：余量已经把放大后的图标整个包住了，
		//   而"逐图标精确并集"正是当年会导致动画中失配的做法。
		// 成功后 region 所有权归系统，不要再 DeleteObject
		SetWindowRgn(hwnd, region, TRUE);
	}

	void DockWin::launch(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		const auto& item = items[index];

		if (isShellObjectPath(item.path)) {
			// shell 虚拟对象（回收站 / 此电脑 / 控制面板……）没有 exe 路径，
			// 也不能拿字符串直接 ShellExecute —— 要解析成 PIDL，再让 shell
			// 按"虚拟项"去调用（SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST）。
			PIDLIST_ABSOLUTE pidl{};
			if (SUCCEEDED(SHParseDisplayName(item.path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
				SHELLEXECUTEINFOW sei{};
				sei.cbSize = sizeof(sei);
				sei.fMask = SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
				sei.lpVerb = L"open";
				sei.lpIDList = pidl;
				sei.nShow = SW_SHOWNORMAL;
				if (!ShellExecuteExW(&sei)) {
					log(std::format(L"[dock] 打开 {} 失败 err={}", item.path, GetLastError()));
				}
				CoTaskMemFree(pidl);
			}
			else {
				log(std::format(L"[dock] 解析 shell 对象失败：{}", item.path));
			}
		}
		else {
			ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		hoverIndex = -1;
		applyHover(-1);
		ensureHoverTimer(false);
		// 启动完鼠标多半已经移走了（比如新窗口占了前台）→ 走延迟隐藏那条路
		scheduleHide();
	}

	// ---------------------------------------------------------------------------
	// 自己在窗口内弹菜单。
	//
	// ⚠ 不能用 Ling::App::popupMenu()：它把 owner 传成 msgHwnd —— 那是个
	//   HWND_MESSAGE 的消息专用窗口。消息专用窗口没有真实窗口层级，
	//   SetForegroundWindow 必然失败、TrackPopupMenuEx 找不到可归属的 owner，
	//   于是**菜单根本不显示、直接返回 0**（托盘场景下恰好能用，因为托盘菜单
	//   是 shell 驱动的另一条路；窗口内右键就不行了）。
	//
	// 这里改用 dock 自己的真实窗口当 owner。它带 WS_EX_NOACTIVATE，直接
	// SetForegroundWindow 也会失败，所以先用 SetWindowPos(HWND_TOPMOST) 把
	// 前台锁推开再拉——这是 MS KB 135788 那套做法的变体，目的是让菜单能收到
	// "点了别处"从而正常收起。
	// ---------------------------------------------------------------------------
	UINT DockWin::popupMenuHere(HMENU menu, POINT screenPt)
	{
		if (!menu || !hwnd) return 0;

		// 菜单开着期间绝不自动隐藏（任务书 §5：无悬停 / 拖放 / **菜单会话**）。
		// 菜单是模态的，TrackPopupMenuEx 会阻塞在这里，所以必须用计数而不是"回来再置 false"
		// ——期间可能被别的路径重新进入。
		++menuSessions;
		// 顺手取消待执行的滑出（不然 500ms 后菜单还开着、dock 却滑走了）
		if (hideDelayTimerOn) {
			killTimer(kTimerAutoHide);
			hideDelayTimerOn = false;
		}

		// WS_EX_NOACTIVATE 的窗口拿不到前台权，先临时允许激活一下。
		// 只改这一个窗口，不做 AttachThreadInput（那会把自己的输入队列
		// 挂到别的线程上，出问题很难查）。
		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
		SetForegroundWindow(hwnd);

		const UINT cmd = TrackPopupMenuEx(
			menu,
			TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
			screenPt.x, screenPt.y, hwnd, nullptr);

		// 还原 NOACTIVATE：点图标启动程序时不该把 dock 变成活动窗口
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
		DestroyMenu(menu);

		if (menuSessions > 0) --menuSessions;
		// 菜单关了 → 重新按"鼠标现在在不在"决定隐藏
		scheduleHide();
		return cmd;
	}

	void DockWin::showContextMenu()
	{
		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuAdd, L"添加程序…");
		AppendMenuW(menu, MF_STRING, kMenuReload, L"重新载入配置");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 ZDock");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ZDock 0.1.27 · 高精度驱动 120fps");

		POINT pt{};
		GetCursorPos(&pt);
		const UINT cmd = popupMenuHere(menu, pt);
		switch (cmd) {
		case kMenuExit:     Ling::App::get()->quit(0); break;
		case kMenuReload:   reloadConfig();            break;
		case kMenuAdd:      addItem();                 break;
		case kMenuSettings: settings.open();           break;
		default: break;
		}
	}

	// ---------------------------------------------------------------------------
	// 添加一项到 Dock 末尾。
	//
	// ⚠ 用 GetOpenFileNameW（comdlg32）选文件。它是模态的，且**会临时抢前台** ——
	//   和右键菜单一个道理，我们的窗口带 WS_EX_NOACTIVATE，所以先把该标志摘掉，
	//   否则对话框可能被压在别的窗口后面、用户以为"点了没反应"。
	//   这也解释了为什么 openFileName 的 owner 必须传 hwnd 而不是 nullptr。
	// ---------------------------------------------------------------------------
	void DockWin::addItem()
	{
		wchar_t file[MAX_PATH * 2]{};

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = hwnd;
		ofn.lpstrFilter = L"程序 (*.exe;*.lnk)\0*.exe;*.lnk\0所有文件\0*.*\0\0";
		ofn.lpstrFile = file;
		ofn.nMaxFile = static_cast<DWORD>(std::size(file));
		ofn.lpstrTitle = L"选择要加到 Dock 的程序";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

		const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
		const BOOL picked = GetOpenFileNameW(&ofn);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

		if (!picked) {
			// 用户取消：ComDlg 会往自己的 buffer 写 CDERR_*，这里只记一行，别弹框
			log(std::format(L"[dock] 添加程序：用户取消（CommDlgExtendedError={}）",
				static_cast<unsigned>(CommDlgExtendedError())));
			return;
		}

		auto* cfg = Config::get();
		const std::wstring path{ file };
		// 已经在列表里就不重复加（否则会出现两个一模一样的图标，删起来也麻烦）
		for (const auto& it : cfg->items) {
			if (_wcsicmp(it.path.c_str(), path.c_str()) == 0) {
				log(L"[dock] 添加程序：该程序已在 Dock 里，忽略");
				return;
			}
		}

		const auto backup = cfg->items;
		cfg->items.push_back(ItemConfig{ path, {} });
		if (!cfg->save()) {
			cfg->items = backup;
			log(L"[dock] 添加程序：写盘失败，已回退");
			return;
		}

		log(std::format(L"[dock] 已添加 {}，重建界面", path));
		rebuild();
	}

	// ---------------------------------------------------------------------------
	// 图标级右键菜单。命令 id 与全局菜单**共用一张表**（都在同一个消息循环上），
	// 所以两项用同一组 id：kMenuOpen / kMenuOpenAdmin / kMenuRemove。
	// ---------------------------------------------------------------------------
	void DockWin::showItemContextMenu(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		const std::wstring name = items[index].name;
		log(std::format(L"[menu] 图标菜单 index={} ({})", index, name));

		const bool temporary = items[index].temporary;

		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		// 首行是个不可点的标题（显示是哪个图标）。用 MF_DISABLED 而不是 MF_GRAYED：
		// 灰掉的标题看着像"功能不可用"，而这里只是标题。
		AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, name.c_str());
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		if (temporary) {
			// 临时图标：只能固定或关闭它的全部窗口（它本来就不在配置里）
			AppendMenuW(menu, MF_STRING, kMenuPin, L"固定到 Dock");
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, kMenuCloseGroup, L"关闭全部窗口");
		}
		else {
			AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开");
			// shell 虚拟对象（回收站等）没有"以管理员身份打开"这回事
			if (!isShellObjectPath(items[index].path)) {
				AppendMenuW(menu, MF_STRING, kMenuOpenAdmin, L"以管理员身份打开");
			}
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, kMenuRemove, L"从 Dock 移除");
		}

		// ---- dock 栏自身的命令 ----
		// 用户反馈：原来只有右键点"图标之间的缝隙"才能弹出 dock 菜单，
		// 那个可点范围只有几个像素、很难点中。所以把 dock 级命令也挂到图标菜单下面，
		// 中间用分隔符隔开（上半段是这个程序的，下半段是整个 dock 的）。
		//
		// ⚠ 这里**不放"退出 ZDock"**：混在"移除 / 关闭全部窗口"旁边太容易误点，
		//   而误点的代价是关掉整个 dock。退出仍然只在空白处菜单里。
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
		AppendMenuW(menu, MF_STRING, kMenuAdd, L"添加程序…");
		AppendMenuW(menu, MF_STRING, kMenuReload, L"重新载入配置");

		POINT pt{};
		GetCursorPos(&pt);
		const UINT cmd = popupMenuHere(menu, pt);
		switch (cmd) {
		case kMenuOpen:       launch(index);      break;
		case kMenuSettings:   openSettings();     break;
		case kMenuAdd:        addItem();          break;
		case kMenuReload:     reloadConfig();     break;
		case kMenuOpenAdmin:  launchAdmin(index); break;
		case kMenuRemove:     removeItem(index);  break;
		case kMenuPin:        pinItem(index);     break;
		case kMenuCloseGroup: {
			// 关闭这个分组的全部窗口。先拷 hwnd 列表，避免关闭过程中分组结构变化。
			if (const AppGroup* g = groupOfItem(static_cast<size_t>(index))) {
				const std::vector<HWND> wins = g->windows;
				for (HWND wh : wins) closeWindow(wh);
			}
			break;
		}
		default: break;
		}
	}

	void DockWin::launchAdmin(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		const auto& path = items[index].path;

		// ShellExecuteW 的 "runas" 会弹 UAC。用户取消时返回 <= 32（不是异常），
		// 记一行日志即可，别弹框打扰（该用户明确讨厌 MessageBox）。
		const HINSTANCE r = ShellExecuteW(nullptr, L"runas", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<INT_PTR>(r) <= 32) {
			log(std::format(L"[dock] runas 启动被拒/失败（{}），code={}", path, reinterpret_cast<INT_PTR>(r)));
		}

		hoverIndex = -1;
		applyHover(-1);
		ensureHoverTimer(false);
		scheduleHide();
	}

	// ---------------------------------------------------------------------------
	// 从 Dock 移除一项并落盘。顺序很重要：
	//   1) 先改内存 + 写 config（写失败就整体回退，别让界面和磁盘不一致）
	//   2) 再重建界面（重建会重新读 Config，所以必须先写完）
	// ---------------------------------------------------------------------------
	void DockWin::removeItem(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;

		auto* cfg = Config::get();
		if (index < static_cast<int>(cfg->items.size())) {
			const auto backup = cfg->items;   // 写失败要回退，先留一份
			cfg->items.erase(cfg->items.begin() + index);
			if (!cfg->save()) {
				cfg->items = backup;
				log(L"[dock] 移除项写盘失败，已回退（Dock 不变）");
				return;
			}
		}

		log(std::format(L"[dock] 已移除第 {} 项，重建界面", index));
		rebuild();
	}

	// ---------------------------------------------------------------------------
	// 用 config.json 的最新内容重建整个界面。
	// 走"销毁所有子节点 + 重新走一遍 onCreated 的那套构造"，
	// 而不是逐项增量增删 —— 阶段二的项数很少，重建更不容易漏状态。
	// ---------------------------------------------------------------------------
	void DockWin::rebuild()
	{
		if (!hwnd) return;

		// 先复位悬停状态：节点马上要全部消失，留着 hoverIndex 会去访问野指针
		hoverIndex = -1;
		ensureHoverTimer(false);
		items.clear();
		panel = nullptr;
		row = nullptr;
		rowTemp = nullptr;
		rowRight = nullptr;

		// body 是 Ling 的根节点，清空它 = 抹掉 panel / row / 所有 IconNode
		if (body) body->removeAllChildren();

		applyConfig();
		collectItemsFromConfig();

		// 项数变了 → 面板宽度也变了 → 窗口尺寸/位置都要跟着改
		const float winW = panelW() + 2 * kSideSlack;
		const float winH = kHaloH + panelH();
		setSize(winW, winH);
		applyDockPlacement(0);

		onCreated();

		loadIcons();
		layout();
		updateHitRegion();
		placeIndicators();
		refresh();
		log(std::format(L"[dock] 重建完成，{} 项", items.size()));

		// 重建之后分组信息要重新对一遍（项集可能变了）
		syncWithTracker();
	}

	// ---------------------------------------------------------------------------
	// 重新读 config.json 并按新值重建。用户手改了配置后不必重启进程。
	// ⚠ 注意顺序：先 load 再 rebuild —— rebuild 内部会再 applyConfig/collect，
	//   它读的是 Config 的内存态，所以 load 必须在前。
	// ---------------------------------------------------------------------------
	void DockWin::reloadConfig()
	{
		Config::get()->load();
		rebuild();
		// 阶段四：自动隐藏开关 / 热区 / 工作区预留都要跟着新配置重来一遍
		// ⚠ 顺序：先 applyAutoHideConfig（可能把窗口摆回展开态），再 syncAppBar。
		applyAutoHideConfig();
		syncAppBar();
	}

	// ===========================================================================
	// 阶段三：窗口跟踪 / 运行指示 / 临时图标 / 点击切换 / 分组列表
	// ===========================================================================

	void DockWin::startTracker()
	{
		tracker.onLog = [](const std::wstring& s) { log(s); };

		// 分组表变了 → 结构可能要增删（有无新应用在跑）+ 指示器要重刷。
		tracker.onChanged = [this] { syncWithTracker(); };

		// 前台换了 → 图标高亮（当前应用在用的那个更亮一点）
		tracker.onForegroundChanged = [this](HWND) { refreshIndicators(); };

		// 窗口请求注意 → 对应图标弹一下
		tracker.onFlash = [this](HWND hwnd) {
			const DWORD pid = [hwnd] { DWORD p = 0; GetWindowThreadProcessId(hwnd, &p); return p; }();
			if (!pid) return;
			for (auto& item : items) {
				const AppGroup* g = groupOfItem(&item - items.data());
				if (!g) continue;
				const bool hit = std::find(g->windows.begin(), g->windows.end(), hwnd) != g->windows.end();
				if (hit && item.node) { item.node->bounce(); break; }
			}
			};

		// 全屏变化 → 阶段四：让位（隐藏）/ 恢复
		tracker.onFullscreenChanged = [this](bool on) { onFullscreenChanged(on); };

		// explorer 重启自愈（任务书 §2 #32）
		tracker.onTaskbarCreated = [this] { onTaskbarCreated(); };

		// 显示环境变化（分辨率 / 主屏切换 / 系统度量）→ 重排。
		// ⚠ 这条是**广播**，靠跟踪器那个 0x0 隐藏顶层窗口收（Ling 不暴露消息口）。
		//   DPI 变化走另一条路（Ling 的 onDpiChanged），见 create()。
		tracker.onDisplayChanged = [this] {
			// 也延后一拍：WM_DISPLAYCHANGE 之后系统还会继续调整（任务栏重排、
			// 工作区刷新），立刻重排会算到中间态。
			setTimer(1, kTimerRelayout);
			};

		// 自动化测试注入：探针 PostMessage 把"全屏状态"喂进来，
		// 免得为了造一个真全屏前台窗口去 SetForegroundWindow（那会抢用户焦点）。
		// 载体是跟踪器的常驻接收窗口（热区在 autoHide 关闭时不存在，靠不住）。
		tracker.onTestInject = [this](bool on) { onFullscreenChanged(on); };

		if (!tracker.start()) {
			log(L"[dock] 窗口跟踪器启动失败，运行指示 / 临时图标不可用");
			return;
		}
		tracker.rebuildAll();
	}

	// ---------------------------------------------------------------------------
	// 取某图标对应的分组。
	//   固定项：按 exe 路径找（tracker 内部用小写路径做键）
	//   临时项：直接用它记住的 groupKey
	// ---------------------------------------------------------------------------
	const AppGroup* DockWin::groupOfItem(size_t index) const
	{
		if (index >= items.size()) return nullptr;
		const auto& item = items[index];
		if (!item.groupKey.empty()) return tracker.findGroup(item.groupKey);
		return tracker.findByExePath(item.path);
	}

	// ---------------------------------------------------------------------------
	// 把图标列表和跟踪器的分组表对齐。
	//
	// ⚠ **绝不调用 rebuild()** —— rebuild 结尾又会调回本函数，那就成了
	//   "sync → rebuild → sync → …" 的无限循环（第一版就是这么写的，
	//   日志里刷了 23 次"重建完成"）。结构增删一律**就地**做节点增删。
	//
	// 两类变化：
	//   · 固定项：不增删节点，只更新 groupKey / 运行状态
	//   · 临时项：在跑但没被固定的应用 → 补图标；窗口全关了 → 删图标
	//
	// 结构真变了才重新摆布局（面板宽度会变），否则只刷指示器 ——
	// 每次事件都重建节点会把悬停动画一直打断。
	// ---------------------------------------------------------------------------
	void DockWin::syncWithTracker()
	{
		if (!hwnd || !body) return;

		// 当前所有在运行的分组键
		std::vector<std::wstring> running;
		running.reserve(tracker.groups().size());
		for (const auto& g : tracker.groups()) {
			if (g.running()) running.push_back(g.key);
		}

		// 固定项是否覆盖了这个运行分组（覆盖了就不该再出临时图标）。
		// ⚠ 判据用**exe 路径**逐个分组比，不要"先取一个分组键、再拿键去比对"：
		//   带 AUMID 的分组键是 `aumid:`（键里没有路径），而且同一个 exe 完全可能
		//   同时存在两个分组（一个带 AUMID、一个不带）。用户 2026-10-11 报的
		//   "固定之后又多出一个 chrome 图标"就是前者漏判造成的。
		auto pinnedCovers = [this](const AppGroup& g) {
			if (g.exePath.empty()) return false;
			for (const auto& item : items) {
				if (item.temporary || item.path.empty()) continue;
				if (_wcsicmp(item.path.c_str(), g.exePath.c_str()) == 0) return true;
			}
			return false;
			};

		bool structural = false;

		// 1) 已存在的临时项：分组没了就删掉（连同它的节点）
		for (size_t i = items.size(); i-- > 0;) {
			auto& item = items[i];
			if (!item.temporary) {
				// 固定项：只刷新 groupKey（应用可能刚起来 / 刚退出）
				if (const AppGroup* g = tracker.findByExePath(item.path)) item.groupKey = g->key;
				else item.groupKey.clear();
				continue;
			}
			const bool stillRunning = std::find(running.begin(), running.end(), item.groupKey) != running.end();
			if (!stillRunning) {
				destroyItemNode(item);
				items.erase(items.begin() + i);
				structural = true;
			}
		}

		// 2) 新出现的、没被固定的运行分组 → 补临时图标（就地造节点）
		for (const auto& g : tracker.groups()) {
			if (!g.running()) continue;
			if (pinnedCovers(g)) continue;
			bool exists = false;
			for (const auto& item : items) {
				if (item.temporary && item.groupKey == g.key) { exists = true; break; }
			}
			if (exists) continue;

			DockItem item;
			item.path = g.exePath;              // UWP 可能为空
			item.name = g.displayName.empty() ? L"应用" : g.displayName;
			item.temporary = true;
			item.groupKey = g.key;

			// 造型树上的节点。图标按峰值尺寸取（和固定项同一套规则）。
			// 临时图标一律进 rowTemp —— 它排在左固定组和右固定组之间
			auto* node = rowTemp->makeChild<IconNode>();
			node->setSize(cfgIconBase, cfgIconBase);
			node->setTemporary(true);
			item.node = node;

			auto* dot = body->makeChild<IndicatorNode>();
			dot->setPositionType(Ling::Position::Absolute);
			dot->setSize(kIndicatorDia, kIndicatorDia);
			dot->setOn(false);
			item.indicator = dot;

			// ⚠ 节点造完必须立刻加载位图，否则新临时图标是个空白方块
			//   （loadIcons() 只在 create/rebuild 里跑，这里走的是增量路径）。
			if (auto* ctx = Ling::D2D::get()->deviceContext.Get(); ctx && !item.path.empty()) {
				const int targetPx = static_cast<int>(std::lround(px(cfgIconBase) * cfgHoverPeak));
				node->setBitmap(loadShellIcon(ctx, item.path, targetPx));
			}

			items.push_back(std::move(item));
			structural = true;
		}

		const bool runningChanged = (running != lastRunningKeys);
		lastRunningKeys = running;

		if (structural) {
			// 项数变了 → 面板宽度变 → 窗口尺寸/位置/间距全要重算。
			// 不去动 Config，只是把内存里的 items 重新映射到布局。
			relayoutForItemCount();
		}

		// 结构没变（或已重排完）都要刷运行态
		for (auto& item : items) {
			if (!item.node) continue;
			if (item.temporary) {
				item.node->setTemporary(true);
				item.node->setRunning(true);
			}
			else {
				const AppGroup* g = groupOfItem(static_cast<size_t>(&item - items.data()));
				item.node->setRunning(g && g->running());
			}
		}
		refreshIndicators();

		// 诊断：ZDOCK_VERBOSE_ITEMS=1 时把 items 逐条打出来。
		// ⚠ 为什么要逐条：这类"同一个应用出现两个图标 / 固定项认不出自己的运行分组"
		//   的问题，日志里那行**总数**（"Dock 共 N 项，其中临时 K"）会被机器上
		//   其它正在跑的应用搅乱，分不清是谁多了。逐条打 path / temporary / groupKey
		//   就能一眼看出是哪一步的匹配断了（固定项的 key 为空 = 没匹配上运行分组）。
		static const bool verboseItems = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_ITEMS", buf, 8) > 0;
			}();
		if (verboseItems) {
			for (size_t i = 0; i < items.size(); ++i) {
				const auto& it = items[i];
				log(std::format(L"[items] #{} {} '{}' key='{}' path={}",
					i, it.temporary ? L"TEMP  " : L"PINNED", it.name, it.groupKey, it.path));
			}
		}

		if (runningChanged || structural) {
			int tempCount = 0;
			for (const auto& it : items) { if (it.temporary) ++tempCount; }
			log(std::format(L"[dock] 分组变化：运行 {} 组，Dock 共 {} 项（其中临时 {}）{}{}",
				running.size(), items.size(), tempCount,
				structural ? L"，已重排布局" : L"",
				runningChanged ? L"（运行集合变化）" : L""));
		}
	}

	// ---------------------------------------------------------------------------
	// 删掉一项的节点（图标 + 指示器）。项数变化时调用。
	// ---------------------------------------------------------------------------
	void DockWin::destroyItemNode(DockItem& item)
	{
		// Ling 的节点由父节点持有，摘下来（detachChild 返回的 unique_ptr 就地析构）即销毁。
		if (item.indicator && body) body->removeChild(item.indicator);

		// ⚠⚠ 图标节点挂在**哪个容器**里要挨个试，不能一律 `row->removeChild()`：
		//   临时项在 rowTemp、pinRight 的固定项在 rowRight、其余才在 row。
		//   原来只问 row —— 对临时项来说 detachChild 在 row 的孩子里找不到它、
		//   **静默返回 nullptr，节点就赖在 rowTemp 上了**。
		//   后果很难看出来：那个图标**照样画得出来**，但 items 里已经没有它 →
		//   面板宽度按"少一项"算 → 多出来的图标被挤出面板、糊在旁边
		//   （用户报的"运行中再开程序，图标会叠在一起"就是这个）。
		if (item.node) {
			bool removed = false;
			for (Ling::Node* host : { row, rowTemp, rowRight }) {
				if (host && host->detachChild(item.node)) { removed = true; break; }
			}
			if (!removed) {
				log(std::format(L"[dock] ⚠ 没找到要销毁的图标节点（{}）—— 容器归属不对？", item.name));
			}
		}
		item.node = nullptr;
		item.indicator = nullptr;
	}

	// ---------------------------------------------------------------------------
	// 项数变化后重排：重算每个图标的间距、面板宽度、窗口尺寸与位置。
	//
	// ⚠ 复用 onCreated 的布局规则，但**不销毁节点** —— 只更新已有节点的
	//   尺寸/边距，再 layout 一次。新加的临时图标已经在 row 里了（Flex 行）。
	// ---------------------------------------------------------------------------
	void DockWin::relayoutForItemCount()
	{
		// row 是 Flex 容器；间距是每个节点的沿边外边距。项数变了要重设一遍：
		// 除**本段**最后一项外都要有间距（段间距由 applyNodeLayout 补）。
		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node) continue;
			node->setSize(cfgIconBase, cfgIconBase);
			const float gap = isLastInSegment(i) ? 0.f : cfgIconGap;
			// ⚠ 间距要加在**沿边方向**：横向边 → 右间距；纵向边 → 下间距。
			//   不然纵向停靠时图标会挤成一列没有间隙。
			if (horizontalEdge()) {
				node->setMarginRight(gap);
				node->setMarginBottom(0.f);
			}
			else {
				node->setMarginRight(0.f);
				node->setMarginBottom(gap);
			}
		}

		// 面板 / 行 / 排列方向（位置与尺寸都随停靠边）
		applyNodeLayout();

		// 窗口尺寸 = 面板 + 四周留白。⚠ 留白怎么分布也随停靠边换轴：
		//   横向边（bottom/top）→ 左右各 kSideSlack，上方或下方留 kHaloH
		//   纵向边（left/right）→ 上下各 kSideSlack，左方或右方留 kHaloH
		const float winW = horizontalEdge() ? (panelW() + 2 * kSideSlack) : (panelW() + kHaloH);
		const float winH = horizontalEdge() ? (panelH() + kHaloH) : (panelH() + 2 * kSideSlack);

		// 窗口尺寸变化的**同时**要把位置摆回"贴边 + 按对齐居中"，
		// 否则窗口会以左下角为锚、越走越偏。
		setSize(winW, winH);

		// ⚠⚠ **只摆一次**，绝不能"先摆展开位、再摆回隐藏位"。
		//
		//   踩过（2026-10-13 用户报"切换应用时 dock 弹出然后马上又隐藏"，
		//   复现步骤：点一下资源管理器窗口 → 分组变化 → 重排）：
		//     旧写法是先 `applyDockPlacement(0)`（展开位）、再 `applyDockPlacementHidden()`。
		//     那是**两次 SetWindowPos**，中间那一帧的"展开位"会被 DWM 合成出来 ——
		//     用户看到的就是"弹一下又缩回去"。而窗口位置这种几何状态**没有中间态可接受**。
		//
		//   所以：**先按当前滑动状态算出该在哪，然后一次摆到位**。
		const bool wantHidden = (slideState == SlideState::Hidden
			|| (slideState == SlideState::Sliding && slideTo > 0.f));
		if (wantHidden) {
			// 隐藏态（或正在往隐藏走）：落位到隐藏终点，避免半路跳变
			killTimer(kTimerSlide);
			slideTimerOn = false;
			setAnimTick(false);
			slideState = SlideState::Hidden;
			slideT = 1.f;
			applyDockPlacementHidden();
		}
		else {
			// 展开态 / 正在滑入：落位到展开位
			if (slideState == SlideState::Sliding) {
				killTimer(kTimerSlide);
				slideTimerOn = false;
				slideState = SlideState::Shown;
				slideT = 0.f;
			}
			applyDockPlacement(0);
		}

		layout();
		updateHitRegion();
		placeIndicators();
		refresh();
	}

	// ---------------------------------------------------------------------------
	// 只刷指示器（亮/灭 + 前台高亮），不动结构。
	// ---------------------------------------------------------------------------
	void DockWin::refreshIndicators()
	{
		for (size_t i = 0; i < items.size(); ++i) {
			auto& item = items[i];
			if (!item.indicator) continue;
			const AppGroup* g = groupOfItem(i);
			const bool running = item.temporary || (g && g->running());
			// ⚠ 指示器开关（showIndicator）是"**显示**"层面的开关，
			//   而 setOn 的语义本来就是这个 —— 但它平时靠 running 驱动。
			//   这里把配置关掉时一律置 off，不必真的销毁节点。
			item.indicator->setOn(cfgShowIndicator && running);
			if (running) {
				// 前台应用用满色强调色，后台运行用半透明同色系。
				// ⚠ 别调太暗：指示器只有 4px，面板底色又是深灰，太暗了根本看不见
				//   （第一版用 0x99 试过，截图里几乎辨认不出）。
				const bool fg = g && g->isForeground();
				item.indicator->setColor(fg ? 0xFF4CC2FF : 0xDD4CC2FF);
			}
			if (item.node) item.node->setRunning(running);
		}
		placeIndicators();
	}

	// ---------------------------------------------------------------------------
	// 指示器定位：图标正下方居中的 4px 圆点。
	// ⚠ 必须在 layout 之后调 —— 图标坐标是 Flex 算出来的，布局前是 0。
	// ---------------------------------------------------------------------------
	// ---------------------------------------------------------------------------
	// 指示器定位：图标正下方居中的 4px 圆点。
	//
	// ⚠ 两个坑，都踩过：
	//  1) **坐标系**：Ling 的 Node::x/y 是**绝对坐标**（相对窗口客户区），不是
	//     相对父节点 —— 实测 row->x == row 里第一个 IconNode->x == 77。
	//     所以直接用 item.node->x/y 就行，别再叠加 row 的偏移。
	//  2) **单位**：node->x/y/w/h 是**物理像素**（yoga 输出已经乘过 dpi），
	//     而 setPosition / setSize 内部会**再乘一次 dpi**（见 Ling Node.cpp）。
	//     所以这里要给 setPosition 喂**逻辑像素** = 物理值 / dpi。
	//     第一版把物理值直接喂进去，位置被放大 1.24 倍跑到窗口外，看着像"没画"。
	// ⚠ 必须在 layout 之后调 —— 图标坐标是 Flex 算出来的，布局前是 0。
	// ---------------------------------------------------------------------------
	void DockWin::placeIndicators()
	{
		// ---------------------------------------------------------------------
		// 诊断：图标的**布局框**必须是 1:1。
		// `IconNode` 用 `Stretch(Fill)` 把正方形位图（101x101）填进这个框 —— 框一旦
		// 不是正方形，图标就会被**横向拉长**（用户 2026-10-12 报的"图标撕裂"就是
		// 一个图标看起来比别的宽一倍）。正常路径下它应该恒等；一旦不等，
		// 这条日志会点名是哪个图标、实际宽高多少 —— 直接指向 flex 布局的问题。
		// ⚠ 只在"结果变化"时打一条汇总，避免每帧刷屏。
		// ---------------------------------------------------------------------
		{
			static std::wstring lastBad;
			std::wstring bad;
			for (const auto& it : items) {
				const auto* nd = it.node;
				if (!nd || nd->w <= 0.f || nd->h <= 0.f) continue;
				if (std::abs(nd->w - nd->h) > 1.0f) {
					bad += std::format(L"「{}」{:.0f}x{:.0f} ", it.name, nd->w, nd->h);
				}
			}
			if (bad != lastBad) {
				lastBad = bad;
				if (bad.empty()) {
					log(L"[icon] 所有图标布局框都是 1:1");
				}
				else {
					log(std::format(L"[icon] ⚠ 有图标布局框不是 1:1（会被横向拉伸变形）：{}", bad));
				}
			}
		}
		static const bool verbose = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_IND", buf, 8) > 0;
			}();
		const float d = (dpi > 0.f) ? dpi : 1.f;
		for (size_t i = 0; i < items.size(); ++i) {
			auto& item = items[i];
			if (!item.indicator || !item.node) continue;
			// 物理像素 → 逻辑像素
			const float nx = item.node->x / d;
			const float ny = item.node->y / d;
			const float nw = item.node->w / d;
			const float nh = item.node->h / d;

			// 指示器画在**面板厚度方向的末端**（图标之后那一格）——
			// `panelAcross()` 就是为它多留的：横向停靠留在下方、纵向停靠留在右侧。
			// 这样四个停靠边用同一套留白逻辑，不用为每个边单独调偏移。
			float dx = 0.f, dy = 0.f;
			if (horizontalEdge()) {
				dx = nx + nw * 0.5f - kIndicatorDia * 0.5f;
				dy = ny + nh + kIndicatorGap;
			}
			else {
				dx = nx + nw + kIndicatorGap;
				dy = ny + nh * 0.5f - kIndicatorDia * 0.5f;
			}
			item.indicator->setPosition(Ling::Edge::Left, dx);
			item.indicator->setPosition(Ling::Edge::Top, dy);
			if (verbose) {
				log(std::format(L"[ind] #{} node物理=({:.1f},{:.1f},{:.1f},{:.1f}) -> dot逻辑=({:.1f},{:.1f}) 面板逻辑=({:.1f},{:.1f},{:.1f},{:.1f})",
					i, item.node->x, item.node->y, item.node->w, item.node->h, dx, dy,
					panel->x / d, panel->y / d, panel->w / d, panel->h / d));
			}
		}
	}

	// ---------------------------------------------------------------------------
	// 点图标：
	//   有窗口在跑 → 切到那个应用（多窗口时给列表菜单，让用户挑）
	//   没在跑       → 启动它
	//   UWP 临时图标没有 exe 路径 → 只能切窗口，启动它得走 shell:AppsFolder
	// ---------------------------------------------------------------------------
	void DockWin::clickItem(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;

		const AppGroup* g = groupOfItem(static_cast<size_t>(index));

		if (g && g->running()) {
			if (g->windows.size() == 1) {
				WindowTracker::activateWindow(g->windows[0]);
			}
			else {
				showWindowListMenu(index);
			}
		}
		else if (!items[index].temporary) {
			launch(index);
		}
		else {
			log(std::format(L"[dock] 临时图标 {} 没有可执行的启动路径（UWP 暂不支持启动）", items[index].name));
		}

		hoverIndex = -1;
		applyHover(-1);
		ensureHoverTimer(false);
		scheduleHide();
	}

	// ---------------------------------------------------------------------------
	// 多窗口分组的窗口列表：每行是一个窗口标题，右键可以关它。
	// ⚠ 命令 id 从 200 起编号，避免和固定菜单项冲突。
	// ---------------------------------------------------------------------------
	void DockWin::showWindowListMenu(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		const AppGroup* g = groupOfItem(static_cast<size_t>(index));
		if (!g || g->windows.empty()) return;

		constexpr UINT kBase = 200;   // 窗口行的命令 id 基址

		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, g->displayName.c_str());
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		for (size_t i = 0; i < g->windows.size(); ++i) {
			const HWND wh = g->windows[i];
			auto it = [&] {
				// 标题可能带 & 会被当成助记符，转义掉
				std::wstring t;
				const int n = GetWindowTextLengthW(wh);
				if (n > 0) {
					t.resize(static_cast<size_t>(n) + 1, L'\0');
					const int got = GetWindowTextW(wh, t.data(), n + 1);
					t.resize(static_cast<size_t>(std::max(0, got)));
				}
				if (t.empty()) t = L"(无标题窗口)";
				std::wstring esc;
				esc.reserve(t.size() + 4);
				for (wchar_t c : t) { if (c == L'&') esc += L"&&"; else esc += c; }
				return esc;
				}();
			AppendMenuW(menu, MF_STRING, kBase + static_cast<UINT>(i), it.c_str());
		}
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuCloseGroup, L"关闭全部窗口");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuPin, items[index].temporary ? L"固定到 Dock" : L"已在 Dock 中（固定项）");
		if (!items[index].temporary) {
			// 已固定的项把"固定"这条置灰
			EnableMenuItem(menu, kMenuPin, MF_BYCOMMAND | MF_GRAYED);
		}

		POINT pt{};
		GetCursorPos(&pt);
		const UINT cmd = popupMenuHere(menu, pt);
		if (cmd == 0) return;

		if (cmd == kMenuPin) { pinItem(index); return; }

		// 重新取一次分组 —— 菜单期间可能已经有窗口关了，下标会错位
		const AppGroup* g2 = groupOfItem(static_cast<size_t>(index));
		if (!g2) return;

		if (cmd == kMenuCloseGroup) {
			// 关全部：先拷一份 hwnd 列表，避免关闭过程中分组被改
			const std::vector<HWND> wins = g2->windows;
			log(std::format(L"[dock] 关闭分组 {} 的全部 {} 个窗口", g2->displayName, wins.size()));
			for (HWND wh : wins) closeWindow(wh);
			return;
		}

		if (cmd >= kBase) {
			const size_t i = cmd - kBase;
			if (i < g2->windows.size()) {
				WindowTracker::activateWindow(g2->windows[i]);
			}
		}
	}

	// ---------------------------------------------------------------------------
	// 把临时图标固定下来：写进 config.json（从"运行时自动出现"变成"用户固定"）。
	// 之后重建界面，这个图标就变成固定项了。
	// ---------------------------------------------------------------------------
	void DockWin::pinItem(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		auto& item = items[index];
		if (!item.temporary) return;
		if (item.path.empty()) {
			log(L"[dock] 该应用没有 exe 路径（UWP），暂不支持固定");
			return;
		}

		auto* cfg = Config::get();
		for (const auto& it : cfg->items) {
			if (_wcsicmp(it.path.c_str(), item.path.c_str()) == 0) return;   // 已经在里面
		}

		const auto backup = cfg->items;
		cfg->items.push_back(ItemConfig{ item.path, item.name });
		if (!cfg->save()) {
			cfg->items = backup;
			log(L"[dock] 固定图标写盘失败，已回退");
			return;
		}
		log(std::format(L"[dock] 已固定 {}", item.path));
		rebuild();
	}

	// ---------------------------------------------------------------------------
	// 关窗口：先 WM_CLOSE（给程序机会提示保存），不立刻强杀。
	// ⚠ 不做 SendMessageTimeout 轮询等待 —— 那会把 dock 的 UI 线程卡住。
	//   程序不响应就让它留着，用户可以再点一次或在任务栏处理。
	// ---------------------------------------------------------------------------
	void DockWin::closeWindow(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}

	// ===========================================================================
	// 阶段四：窗口定位 / 自动隐藏 / 全屏让位 / 工作区预留
	// ===========================================================================

	// ---------------------------------------------------------------------------
	// dock 所在监视器的**完整矩形**（rcMonitor，物理像素）。
	// ⚠ 定位 dock 一律以它为基准，不要用 SPI_GETWORKAREA —— 工作区会被
	//   AppBar 预留改掉，拿它当基准等于自引用（见 dockRectShown 的注释）。
	// ---------------------------------------------------------------------------
	RECT DockWin::monitorRect() const
	{
		// 阶段六：配了显示器序号就锚那一台（任务书 §2 #31）；
		// -1（默认）= 跟随窗口当前所在的显示器 —— 也就是加这个配置项之前的老行为。
		if (cfgMonitorIndex >= 0) {
			const RECT r = monitors::rectFor(cfgMonitorIndex);
			if (r.right > r.left && r.bottom > r.top) return r;
		}

		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
		// 兜底：主监视器
		return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}

	// ---------------------------------------------------------------------------
	// dock 面板在屏幕上的目标矩形（**展开态**，物理像素）。
	// 与阶段一以来的算法一致：底边 = 屏幕底边 − bottomMargin，水平居中于屏幕。
	//
	// ⚠⚠ 基准必须是**监视器的 rcMonitor**，不能用 SPI_GETWORKAREA。
	//   踩过的坑（实测）：AppBar 预留会缩进"工作区"，而工作区正是我们用来
	//   定位 dock 的东西 —— 这就是个**自引用反馈回路**。工作区被抬一次，
	//   下一次算出来的 dock 位置就跟着上爬一次，于是每注册一次就多占一层：
	//   实测干净工作区底 1440，一次注册后变 1257（爬了 183px = 两层 95px）。
	//   改成锚 rcMonitor 后，dock 位置恒定，AppBar 只负责把工作区让出 95px。
	// ---------------------------------------------------------------------------
	RECT DockWin::dockRectShown() const
	{
		const RECT mon = monitorRect();   // rcMonitor：不随 AppBar 预留变化

		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelHpx = static_cast<int>(px(panelH()));
		const int margin = static_cast<int>(px(Config::get()->bottomMargin));
		const int off = static_cast<int>(px(cfgOffset));
		const int monW = mon.right - mon.left;
		const int monH = mon.bottom - mon.top;

		int panelLeft = mon.left;
		int panelTop = mon.top;

		if (horizontalEdge()) {
			// 沿停靠边的方向是 X（bottom/top）：先按对齐定左右，交叉轴由停靠边定上下
			switch (cfgAlign) {
			case DockAlign::Start:  panelLeft = mon.left + off; break;
			case DockAlign::Center: panelLeft = mon.left + (monW - panelWpx) / 2 + off; break;
			case DockAlign::End:    panelLeft = mon.right - panelWpx - off; break;
			}
			panelTop = (cfgEdge == DockEdge::Bottom)
				? mon.bottom - margin - panelHpx
				: mon.top + margin;
		}
		else {
			// 沿停靠边的方向是 Y（left/right）
			switch (cfgAlign) {
			case DockAlign::Start:  panelTop = mon.top + off; break;
			case DockAlign::Center: panelTop = mon.top + (monH - panelHpx) / 2 + off; break;
			case DockAlign::End:    panelTop = mon.bottom - panelHpx - off; break;
			}
			panelLeft = (cfgEdge == DockEdge::Left)
				? mon.left + margin
				: mon.right - panelWpx - margin;
		}

		// 窗口矩形比面板大（留白是透明 halo，够图标放大 / 弹标签用），
		// 所以窗口左上角要按"halo 在哪一侧"往外让。
		float ox = 0.f, oy = 0.f;
		panelOrigin(ox, oy);

		RECT r{};
		r.left = panelLeft - static_cast<int>(px(ox));
		r.top = panelTop - static_cast<int>(px(oy));
		r.right = r.left + static_cast<LONG>(w);
		r.bottom = r.top + static_cast<LONG>(h);
		return r;
	}

	// ---------------------------------------------------------------------------
	// 把窗口摆到"展开态 + offsetY 的垂直偏移"。
	// offsetY 单位 = 物理像素，正数 = 往下（屏幕外方向在顶部，这里是往上滑所以用负数？——）
	// ⚠ 本 dock 贴**底边**，滑出方向是**往下**：offsetY 为正 = 往下 = 往屏幕外。
	//   面板高度加一点余量作为总位移，滑完之后整个窗口都在屏幕外。
	// ---------------------------------------------------------------------------
	void DockWin::applyNodeLayout()
	{
		if (!panel || !row) return;

		float ox = 0.f, oy = 0.f;
		panelOrigin(ox, oy);

		panel->setPosition(Ling::Edge::Left, ox);
		panel->setPosition(Ling::Edge::Top, oy);
		panel->setSize(panelW(), panelH());

		// 行在面板内的偏移：**沿边方向**用 kPadX，**厚度方向**用 kPadY。
		// 纵向停靠时两者换轴 —— 这一步最容易写反（面板宽是厚度、高是沿边长度）。
		const float padAlong = horizontalEdge() ? kPadX : kPadY;
		const float padAcross = horizontalEdge() ? kPadY : kPadX;

		// 三个行容器**各自沿边定位**，依次排开：
		//   左固定项 | 临时项（正在运行的应用）| 右固定项
		// 段的长度 = n*icon + (n-1)*gap；段与段之间再补一个 gap。
		size_t nLeft = 0, nTemp = 0, nRight = 0;
		segmentCounts(nLeft, nTemp, nRight);

		float along = 0.f;
		bool first = true;
		auto place = [&](Ling::Node* r, size_t n) {
			if (!r || n == 0) return;
			const float len = n * cfgIconBase + (n - 1) * cfgIconGap;
			if (!first) along += cfgIconGap;
			first = false;

			if (horizontalEdge()) {
				r->setPosition(Ling::Edge::Left, ox + padAlong + along);
				r->setPosition(Ling::Edge::Top, oy + padAcross);
				r->setSize(len, cfgIconBase);
				r->setFlexDirection(Ling::FlexDirection::Row);
			}
			else {
				r->setPosition(Ling::Edge::Left, ox + padAcross);
				r->setPosition(Ling::Edge::Top, oy + padAlong + along);
				r->setSize(cfgIconBase, len);
				r->setFlexDirection(Ling::FlexDirection::Column);
			}
			// 交叉轴对齐：让图标靠"面板内侧"，靠屏幕边那条留给运行指示器
			r->setAlignItems(Ling::Align::FlexEnd);
			along += len;
			};

		place(row, nLeft);
		place(rowTemp, nTemp);
		place(rowRight, nRight);
	}

	bool DockWin::isLastInSegment(size_t i) const
	{
		const auto& cur = items[i];
		for (size_t j = i + 1; j < items.size(); ++j) {
			const auto& n = items[j];
			if (cur.pinRight == n.pinRight && cur.temporary == n.temporary) return false;
		}
		return true;
	}

	void DockWin::segmentCounts(size_t& left, size_t& temp, size_t& right) const
	{
		left = temp = right = 0;
		for (const auto& it : items) {
			if (it.temporary) ++temp;
			else if (it.pinRight) ++right;
			else ++left;
		}
	}

	void DockWin::applyDockPlacement(int offset)
	{
		// ⚠ offset 的语义是"沿**滑出方向**的位移"（0 = 完全展开），
		//   方向由停靠边决定 —— 不再只加在 Y 上（左右停靠要加在 X 上）。
		const RECT r = dockRectShown();
		shownOrigin.x = r.left;
		shownOrigin.y = r.top;

		int dx = 0, dy = 0;
		slideDir(dx, dy);
		setPosition(r.left + dx * offset, r.top + dy * offset);
	}

	int DockWin::curSlideOffset() const
	{
		if (!hwnd) return 0;
		RECT cur{};
		GetWindowRect(hwnd, &cur);
		int dx = 0, dy = 0;
		slideDir(dx, dy);
		// slideDir 是单位向量，所以位移就是对应轴上的差值
		return dy ? (cur.top - shownOrigin.y) : (cur.left - shownOrigin.x);
	}

	int DockWin::slideDistance() const
	{
		const RECT mon = monitorRect();
		const RECT r = dockRectShown();
		// 各方向都是"从展开位置到整个窗口离开屏幕"的距离
		switch (cfgEdge) {
		case DockEdge::Bottom: return mon.bottom - r.top;
		case DockEdge::Top:    return r.bottom - mon.top;
		case DockEdge::Left:   return r.right - mon.left;
		case DockEdge::Right:  return mon.right - r.left;
		}
		return static_cast<int>(h);
	}

	// ---------------------------------------------------------------------------
	// 把窗口摆到"完全滑出屏幕"的隐藏位（顶边 = 屏幕底边）。
	// ⚠ 和 applyDockPlacement(h) 的区别：h 是窗口高（物理像素），把顶边放到
	//   `shownOrigin.y + h`，那只是"刚好出屏"；用屏幕底边更稳（多出的 halo 也一起出去），
	//   而且这正是 slideOut 补间的终点，两处保持一致。
	// ---------------------------------------------------------------------------
	void DockWin::applyDockPlacementHidden()
	{
		// 沿滑出方向推到底 —— 用 slideDistance() 而不是窗口高/宽，
		// 四个停靠边都成立（左右停靠时"出屏距离"是窗口宽，不是高）。
		applyDockPlacement(slideDistance());
	}

	// ---------------------------------------------------------------------------
	// 显示环境变化后的重排（任务书 §9.9：显示器 / DPI 变化要能自适应）。
	//
	// 触发源有两个，都汇到这里：
	//   · `onDpiChanged`（Ling 自己的事件，WM_DPICHANGED）—— 改缩放比、跨屏拖动；
	//   · `WindowTracker::onDisplayChanged`（WM_DISPLAYCHANGE / WM_SETTINGCHANGE）
	//     —— 改分辨率、换主屏、显示设置变化。
	//
	// 要做的事：按**新的 dpi / 新的监视器**重算尺寸与位置，热区与 AppBar 也要跟着走。
	// ⚠ 别只重算位置：`px()` 依赖 `dpi`，dpi 变了窗口尺寸也得重算，否则图标会
	//   在大屏上显示成小尺寸（或者反过来）。
	// ---------------------------------------------------------------------------
	void DockWin::openSettings()
	{
		settings.open();
		// 设置窗口开着的时候 dock 必须留在屏幕上 —— 用户要一边改一边看效果
		keepVisible();
	}

	void DockWin::relayoutForEnvironment()
	{
		if (!hwnd) return;

		const RECT mon = monitorRect();
		log(std::format(L"[dock] 显示环境变化 -> 重排（dpi={:.2f} 监视器=({},{})-({},{})）",
			dpi, mon.left, mon.top, mon.right, mon.bottom));

		// ⚠ relayoutForItemCount 内部是 applyDockPlacement(0)，也就是**展开态**。
		//   如果此刻 dock 是隐藏的（自动隐藏 / 全屏让位），必须再推回屏幕外，
		//   否则它会突然从屏幕底边冒出来。
		relayoutForItemCount();
		// ⚠ 这里原来还抄了一份"把窗口摆回隐藏位 / 展开位"的逻辑，现在收口进
		//   relayoutForItemCount() 内部了（那份漏给了 syncWithTracker，导致切换应用时
		//   隐藏的 dock 会上浮）。别在这里再抄一遍。

		syncHotZone();
		// AppBar 的批准矩形是**屏幕坐标**的，监视器变了必须重报一次
		syncAppBar();
	}

	// ---------------------------------------------------------------------------
	// 从配置刷新自动隐藏缓存 + 按需创建 / 销毁热区窗口。
	// ---------------------------------------------------------------------------
	void DockWin::applyAutoHideConfig()
	{
		hotZone.onEnter = [this] {
			// 热区被碰到 → 立刻滑入（并取消任何待执行的滑出）
			keepVisible();
			slideIn();
			};
		hotZone.onLeave = [this] {
			// 鼠标离开热区 → 开始"离开后延迟收起"的计时。
			// ⚠ 不能省这一条：光标**直接从热区移开**（不经过 dock 面板）时，dock
			//   收不到任何鼠标消息 —— 没有它，dock 一旦被热区唤出就会一直赖着展开。
			//   到期后 onTimer(kTimerAutoHide) 还会再确认一次（光标若在 dock 上则
			//   refreshHoverFromCursor 会 keepVisible，不会误收）。
			log(L"[hotzone] 鼠标离开热区");
			scheduleHide();
			};
		hotZone.onLog = [](const std::wstring& s) { log(s); };

		if (cfgAutoHide) {
			const RECT hz = hotZoneRect();
			if (!hotZone.alive()) hotZone.create(hz);
			else hotZone.moveTo(hz);
			log(std::format(L"[dock] 自动隐藏已启用（延迟 {}ms / 滑入 {}ms / 滑出 {}ms / 全屏让位 {} / 热区 {}x{}px 范围={}）",
				cfgHideDelayMs, cfgSlideInMs, cfgSlideOutMs, cfgHideOnFullscreen ? 1 : 0,
				hz.right - hz.left, hz.bottom - hz.top,
				cfgHotZoneSpan == HotZoneSpan::Full ? L"整条边"
				: (cfgHotZoneSpan == HotZoneSpan::Dock ? L"仅面板宽" : L"半屏")));
			syncHotZoneEnabled();
		}
		else {
			// 关掉自动隐藏要立刻销毁热区 ——
			// 不销毁的话屏幕边上会留一条看不见却吃点击的窗口。
			hotZone.destroy();
			// ⚠ 条件里必须有 `!shouldHideNow()`：如果此刻是**全屏让位**导致的隐藏
			//   （hideOnFullscreen 开着、全屏应用在前台），dock 必须继续留在屏幕外。
			//   早期版本无条件摆回展开态，会让全屏应用底下突然冒出一条 dock。
			if (slideState != SlideState::Shown && !shouldHideNow()) {
				slideState = SlideState::Shown;
				slideT = 0.f;
				applyDockPlacement(0);
				log(L"[dock] 自动隐藏关闭，回到展开态");
			}
			log(L"[dock] 自动隐藏未启用");
		}
	}

	// ---------------------------------------------------------------------------
	// 热区窗口该在的屏幕矩形（物理像素）。
	// 任务书 §4：贴停靠边的**3px 厚**细窗，宽取 `max(dock 宽, 屏宽/2)`，居中。
	// ---------------------------------------------------------------------------
	RECT DockWin::hotZoneRect() const
	{
		// ⚠ 同样锚监视器，不锚工作区 —— 否则热区会跟着 AppBar 预留一起上爬，
		//   越爬越高，最后鼠标够不到。
		const RECT mon = monitorRect();
		const int thick = std::max(1, cfgHotZoneThickness);   // 物理像素，可配（1~20）

		// 面板在屏幕上的位置（窗口矩形 + halo 偏移反推）
		const RECT wr = dockRectShown();
		float ox = 0.f, oy = 0.f;
		panelOrigin(ox, oy);
		const int panelLeft = wr.left + static_cast<int>(px(ox));
		const int panelTop = wr.top + static_cast<int>(px(oy));
		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelHpx = static_cast<int>(px(panelH()));

		RECT r{};
		if (horizontalEdge()) {
			// 横向细条，贴屏幕的上边或下边。
			// ⚠ 沿边范围以**面板中心**为中心（不是屏幕中心）—— 这样 dock 换成
			//   align=start/end 之后热区会跟着走，用户仍然能对着 dock 划过去。
			const int monW = mon.right - mon.left;
			const int wantW = (cfgHotZoneSpan == HotZoneSpan::Full) ? monW : (cfgHotZoneSpan == HotZoneSpan::Dock ? panelWpx : std::max(panelWpx, monW / 2));   // Full=整条边 / Dock=仅面板宽 / 默认 Half
			const int cx = panelLeft + panelWpx / 2;
			// ⚠ 都先收成 int 再比 —— LONG 和 int 在 Windows 上是不同类型，
			//   直接 std::max(LONG, int) 会模板推导失败（C2672）。
			const int l = std::max(static_cast<int>(mon.left), cx - wantW / 2);
			const int rr = std::min(static_cast<int>(mon.right), l + wantW);
			r.left = l;
			r.right = rr;
			if (cfgEdge == DockEdge::Bottom) {
				r.bottom = mon.bottom;
				r.top = r.bottom - thick;
			}
			else {
				r.top = mon.top;
				r.bottom = r.top + thick;
			}
		}
		else {
			// 纵向细条，贴屏幕的左边或右边
			const int monH = mon.bottom - mon.top;
			const int wantH = (cfgHotZoneSpan == HotZoneSpan::Full) ? monH : (cfgHotZoneSpan == HotZoneSpan::Dock ? panelHpx : std::max(panelHpx, monH / 2));
			const int cy = panelTop + panelHpx / 2;
			const int t = std::max(static_cast<int>(mon.top), cy - wantH / 2);
			const int b = std::min(static_cast<int>(mon.bottom), t + wantH);
			r.top = t;
			r.bottom = b;
			if (cfgEdge == DockEdge::Left) {
				r.left = mon.left;
				r.right = r.left + thick;
			}
			else {
				r.right = mon.right;
				r.left = r.right - thick;
			}
		}
		return r;
	}

	void DockWin::syncHotZone()
	{
		if (cfgAutoHide) {
			const RECT hz = hotZoneRect();
			if (!hotZone.alive()) hotZone.create(hz);
			else hotZone.moveTo(hz);
		}
		else {
			hotZone.destroy();
		}
		syncHotZoneEnabled();
	}

	// ---------------------------------------------------------------------------
	// 热区是否参与命中。
	// 判据取 `shouldHideNow()`（= 因全屏让位而收起）—— 此时 dock 在屏幕外，
	// 热区若还接收鼠标，就等于把全屏游戏 / 视频贴着屏幕边缘的那几像素点击吃掉了。
	// ⚠ 隐藏而非销毁：探针仍可用 PostMessage 驱动（见 EdgeHotZone::setEnabled）。
	// ---------------------------------------------------------------------------
	void DockWin::syncHotZoneEnabled()
	{
		if (!hotZone.alive()) return;
		hotZone.setEnabled(!shouldHideNow());
	}

	// ---------------------------------------------------------------------------
	// 当前是否"该隐藏"：自动隐藏开着，且（全屏应用在前台 且 开了全屏让位）。
	// ⚠ 这里**不含**"鼠标在不在面板上"的判定 —— 那由 keepVisible / 延迟管，
	//   否则鼠标一离开就会立刻隐藏，任务书 §5 要的是"约 500ms 后"。
	// ---------------------------------------------------------------------------
	bool DockWin::shouldHideNow() const
	{
		// ⚠ 这里**不**检查 cfgAutoHide：全屏让位（hideOnFullscreen）是独立开关，
		//   用户可以"不要自动隐藏、但要全屏时让位"。
		//   之前这里写成 `if (!cfgAutoHide) return false;`，等于让让位功能
		//   偷偷依赖了自动隐藏 —— 关掉自动隐藏就一起失效了。
		return cfgHideOnFullscreen && fullscreenNow;
	}

	// ---------------------------------------------------------------------------
	// 鼠标此刻是否压在 dock 面板上。
	// ⚠ 只在"滑入动画刚结束"这类离散时刻调，**不做轮询**（红线 4）。
	//   用 WindowFromPoint 而不是 GetCursorPos+自算矩形：
	//   它会考虑窗口 region（我们做过 halo 穿透），比几何判定更准。
	// ---------------------------------------------------------------------------
	bool DockWin::cursorOverDock() const
	{
		if (!hwnd) return false;
		POINT pt{};
		if (!GetCursorPos(&pt)) return false;
		if (WindowFromPoint(pt) == hwnd) return true;

		// ⚠ 热区是**另一个窗口**（贴边那条 1~20px 的细窗，见 EdgeHotZone.h），
		//   而且压在最上层 —— 从热区唤出 dock 时，光标还压在热区上、并不在 dock 上。
		//   "光标停在停靠边的热区里"必须算成"贴着 dock"。
		// ⚠ 这条判定**两处都要用**（所以抽成 cursorOnHotZone）：
		//   ① 这里 —— tickSlide 结尾问"该不该收"；
		//   ② refreshHoverFromCursor() —— 鼠标事件里问"鼠标还在不在 dock 上"。
		//   0.1.18 之前只有 ① 有，② 漏了。之前一直没暴露是因为热区 alpha=0 收不到
		//   真实鼠标；alpha 修好、热区真能收到鼠标之后，② 的漏判立刻显形 ——
		//   dock 升起约 500ms 又被 scheduleHide 收回（用户 2026-10-12 报的现象）。
		return cursorOnHotZone(pt);
	}

	// ---------------------------------------------------------------------------
	// 光标是否压在贴边热区上（判定口径见头文件里的长注释）。
	// ---------------------------------------------------------------------------
	bool DockWin::cursorOnHotZone(const POINT& pt) const
	{
		if (!cfgAutoHide || !hotZone.alive()) return false;
		const RECT hz = hotZoneRect();
		return PtInRect(&hz, pt) != 0;
	}

	// ---------------------------------------------------------------------------
	// 记一次"用户还在用 dock"：取消待执行的滑出 + 若已隐藏则滑入。
	// ---------------------------------------------------------------------------
	void DockWin::keepVisible()
	{
		if (hideDelayTimerOn) {
			killTimer(kTimerAutoHide);
			hideDelayTimerOn = false;
		}
		if (slideState != SlideState::Shown && !shouldHideNow()) slideIn();
	}

	// ---------------------------------------------------------------------------
	// 鼠标离开面板：启动延迟滑出（500ms）。到期后 onTimer 再确认一次。
	// ⚠ 这是红线 4 明确豁免的"自身 UI 状态短定时器"，不是轮询外部状态。
	// ---------------------------------------------------------------------------
	void DockWin::scheduleHide()
	{
		// ⚠ 诊断日志：这条链一旦断掉，dock 就会"赖着不收起"。2026-10-13 用户报
		//   "鼠标移到屏幕中间很久，dock 还是不隐藏" —— 靠这几条定位断在哪一环。
		if (!cfgAutoHide) return;
		// ⚠ 已经收起就没什么可安排的 —— 不然兜底检测每 120ms 跑一次，
		//   会变成"设定时器 → 到期 → slideOut 早退(已隐藏) → 又设"的空转。
		if (slideState == SlideState::Hidden) return;
		if (menuSessions > 0) {
			log(std::format(L"[dock] scheduleHide 被跳过：菜单会话未结束（menuSessions={}）", menuSessions));
			return;
		}
		if (shouldHideNow()) { slideOut(); return; }   // 全屏应用在前台：立刻让位
		if (hideDelayTimerOn) return;
		hideDelayTimerOn = true;
		setTimer(cfgHideDelayMs, kTimerAutoHide);
		log(std::format(L"[dock] scheduleHide：{}ms 后收起（menuSessions={}）", cfgHideDelayMs, menuSessions));
	}

	// ---------------------------------------------------------------------------
	// 滑入（展开）。时长 cfgSlideInMs（默认 200ms）。
	// ---------------------------------------------------------------------------
	void DockWin::slideIn()
	{
		// ⚠ 不检查 cfgAutoHide：滑动是**动作**，由调用方决定要不要调
		//   （自动隐藏、全屏让位退出都会用），配置门控在各调用点。
		if (!hwnd) return;
		// ⚠ 全屏应用在前台时**保持隐藏**（任务书 §3："全屏应用前台 → 保持隐藏"）。
		//   热区碰一下也要拦住 —— 否则全屏游戏/视频时鼠标扫过屏幕底边，
		//   dock 会从全屏画面底下拱出来。
		if (shouldHideNow()) return;
		if (slideState == SlideState::Shown && slideT <= 0.f) return;
		// ⚠⚠ 已经在往"展开"方向滑了 → **别重启动画**。
		//   滑动中每收到一次鼠标事件都会走到这里（refreshHoverFromCursor → keepVisible → slideIn），
		//   重启会把 slideStartTick 归零、重新计满一遍时长 —— 结果是**动画永远走不完**：
		//   用户 2026-10-12 报的"滑上来还是好慢"，日志里是一次滑入被触发上百次、
		//   而"滑动结束"一条都没有（149 : 0）。
		//   反向（正在滑出时又要滑入）**必须**继续往下走，所以判据要看 slideTo 的方向。
		if (slideState == SlideState::Sliding && slideTo <= 0.f) return;

		// 从当前实际位置开始补间（避免滑动中途反向时跳变）
		slideFrom = static_cast<float>(curSlideOffset());
		slideTo = 0.f;
		slideT = slideFrom;
		slideDurMs = cfgSlideInMs;
		slideStartTick = GetTickCount64();
		slideState = SlideState::Sliding;
		setAnimTick(true);
		// ⚠⚠ 有高精度驱动时**不要**再开 `WM_TIMER` —— 两者交错会把实际帧率推到 ~170fps
		//   （实测 6.0ms/帧），纯粹浪费 CPU，也超出了约定的 ~120fps。
		//   只有在**没有**注入驱动器时（onAnimationTickNeeded 为空，例如探针/裁剪版）
		//   才退回 `WM_TIMER` 那条老路。
		if (!onAnimationTickNeeded && !slideTimerOn) {
			slideTimerOn = true;
			setTimer(kSlideTimerMs, kTimerSlide);
		}
		log(std::format(L"[dock] 滑入开始（{:.0f}px / {}ms）", slideFrom, slideDurMs));
	}

	// ---------------------------------------------------------------------------
	// 滑出（隐藏）。时长 cfgSlideOutMs（默认 300ms，ease-out）。
	// ⚠ 滑出的位移 = 面板高度 + halo 余量，保证整个窗口（含上方 halo）都在屏幕外。
	//   用窗口的高 h 而不是面板高：halo 那圈虽然透明，但它是窗口的一部分。
	// ---------------------------------------------------------------------------
	void DockWin::slideOut()
	{
		// ⚠ 同样不检查 cfgAutoHide（理由见 slideIn）。
		if (!hwnd) return;
		if (slideState == SlideState::Hidden && slideT >= 1.f) return;
		// ⚠⚠ 同理（见 slideIn）：已经在往"隐藏"方向滑了就别重启，否则同样永远走不完。
		//   实测这条触发得最狠 —— 隐藏态下 scheduleHide 的定时器到期会反复调 slideOut，
		//   每次都以 `184 → 184px` 重启，日志里每 200ms 刷一条，滑动永不结束。
		if (slideState == SlideState::Sliding && slideTo > 0.f) return;

		slideFrom = static_cast<float>(curSlideOffset());
		// 目标：整个窗口滑到停靠边之外（同样锚监视器，不受工作区影响）
		const float full = static_cast<float>(slideDistance());
		slideTo = full;
		slideT = (slideTo > 0.f) ? (slideFrom / slideTo) : 1.f;
		slideDurMs = cfgSlideOutMs;
		slideStartTick = GetTickCount64();
		slideState = SlideState::Sliding;
		setAnimTick(true);
		// ⚠⚠ 有高精度驱动时**不要**再开 `WM_TIMER` —— 两者交错会把实际帧率推到 ~170fps
		//   （实测 6.0ms/帧），纯粹浪费 CPU，也超出了约定的 ~120fps。
		//   只有在**没有**注入驱动器时（onAnimationTickNeeded 为空，例如探针/裁剪版）
		//   才退回 `WM_TIMER` 那条老路。
		if (!onAnimationTickNeeded && !slideTimerOn) {
			slideTimerOn = true;
			setTimer(kSlideTimerMs, kTimerSlide);
		}
		log(std::format(L"[dock] 滑出开始（{:.0f} → {:.0f}px / {}ms）", slideFrom, slideTo, slideDurMs));
	}

	// ---------------------------------------------------------------------------
	// 滑动补间。
	//
	// ⚠ 用**定时器补间 + SetWindowPos**，不用 Composition 动画：
	//   Composition 动的是节点，动不了窗口在屏幕上的位置。而"滑出屏幕"必须动窗口。
	//
	// ⚠⚠ 定时器间隔是 `kSlideTimerMs = 10`（**不是 16**，见 DockWin.h 里的长注释）：
	//   `WM_TIMER` 会把 elapse 向上对齐到 15.625ms 的整数倍，写 16 会掉到 2 拍 = 31ms
	//   （≈32fps），写 10 才能拿到 1 拍 = 15.6ms（≈64fps）。这是"滑动发卡"的真根因。
	// ---------------------------------------------------------------------------
	// ---------------------------------------------------------------------------
	// 高精度动画驱动的开关（0.1.27）。
	//
	// ⚠ 滑动补间原来靠 `setTimer(10)` + `WM_TIMER`，而 `WM_TIMER` 的节拍被系统时钟
	//   钉在 **15.625ms**（实测天花板 ~64fps）。要跑 ~120fps 必须换驱动器 ——
	//   main.cpp 用 `SetWaitableTimer(8ms)` + `MsgWaitForMultipleObjects` 驱动
	//   `animationTick()`。这里只负责"滑动期间开、结束立刻关"：
	//   空闲时每 8ms 醒一次会把 CPU 占用顶上去（本项目红线是空闲 CPU 接近 0）。
	// ---------------------------------------------------------------------------
	void DockWin::setAnimTick(bool on)
	{
		if (animTickOn == on) return;
		animTickOn = on;
		if (onAnimationTickNeeded) onAnimationTickNeeded(on);
	}

	// main 的消息循环按 8ms 周期调用；只有滑动期间才真的做事。
	void DockWin::animationTick()
	{
		if (slideState != SlideState::Sliding) {
			setAnimTick(false);
			return;
		}
		tickSlide();
	}

	void DockWin::tickSlide()
	{
		if (!hwnd || slideState != SlideState::Sliding) {
			if (slideTimerOn) { killTimer(kTimerSlide); slideTimerOn = false; }
			setAnimTick(false);
			return;
		}

		// ---- 滑动性能诊断（ZDOCK_VERBOSE_SLIDE=1）----
		// 量化"卡不卡"：记每帧实际间隔与位置。
		// ⚠ 2026-10-12 就是靠它定位到"滑动发卡"的真根因：`WM_TIMER` 的 elapse 会被
		//   **向上对齐到系统时钟节拍（默认 15.625ms）的整数倍** —— 原来写 16ms 恰好跨过
		//   一拍，实际掉到 2 拍 = 31ms（≈32fps，肉眼就是"卡"）。改成 10ms 后稳定拿到
		//   15.6ms（≈64fps），步长变异系数从 0.29 降到 0.03。
		//   详见 DockWin.h 里 `kSlideTimerMs` 的注释，机制实证见 `_probe_timer_resolution.py`。
		static const bool verboseSlide = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_SLIDE", buf, 8) > 0;
			}();
		static ULONGLONG s_lastTick = 0;
		static int s_frames = 0;
		static ULONGLONG s_maxGap = 0;
		static float s_offsets[64]{};   // 每帧的位移，用来核对"匀速"（应等步长递增）
		static int s_offsetN = 0;
		static double s_moveTotal = 0.0, s_moveMax = 0.0;
		static const double s_qpcFreq = [] {
			LARGE_INTEGER f{};
			QueryPerformanceFrequency(&f);
			return (f.QuadPart > 0) ? static_cast<double>(f.QuadPart) : 1.0;
			}();
		// 对照实验：跳过"移动窗口"这一步（ZDOCK_SLIDE_NO_MOVE=1）。
		// 用来分辨"每帧 24ms"到底花在 SetWindowPos 上，还是在定时器 / 消息循环本身 ——
		// 这决定了优化方向是"换动画载体"还是"提高定时精度"。
		static const bool s_skipMove = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_SLIDE_NO_MOVE", buf, 8) > 0;
			}();

		const ULONGLONG nowTick = GetTickCount64();
		if (verboseSlide) {
			if (s_lastTick) s_maxGap = (std::max)(s_maxGap, nowTick - s_lastTick);
			s_lastTick = nowTick;
			++s_frames;
		}

		// 进度 0..1。
		// ⚠ **匀速**（linear）—— 用户 2026-10-12 要求"改成匀速平移"。
		//   原来是 ease-out cubic（`1-(1-t)^3`）：起步快、末段几乎不动，
		//   配合 32fps 的跳帧观感上末段像在"挪"。现在 e = lin 直接线性推进。
		float lin = (slideDurMs > 0)
			? static_cast<float>(nowTick - slideStartTick) / static_cast<float>(slideDurMs)
			: 1.f;
		if (lin > 1.f) lin = 1.f;
		const float e = lin;

		const float dist = slideFrom + (slideTo - slideFrom) * e;
		if (verboseSlide && s_offsetN < 64) s_offsets[s_offsetN++] = dist;
		{
			// 位移沿**滑出方向**施加（bottom=+Y / top=−Y / left=−X / right=+X）
			int dx = 0, dy = 0;
			slideDir(dx, dy);
			const int d = static_cast<int>(std::lround(dist));
			LARGE_INTEGER q0{}, q1{};
			QueryPerformanceCounter(&q0);
			if (!s_skipMove) setPosition(shownOrigin.x + dx * d, shownOrigin.y + dy * d);
			QueryPerformanceCounter(&q1);
			if (verboseSlide) {
				const double ms = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / s_qpcFreq;
				s_moveTotal += ms;
				s_moveMax = (std::max)(s_moveMax, ms);
			}
		}

		if (lin >= 1.f) {
			killTimer(kTimerSlide);
			slideTimerOn = false;
			setAnimTick(false);
			slideT = (slideTo > 0.f) ? 1.f : 0.f;
			slideState = (slideTo > 0.f && std::abs(slideTo) > 1.f) ? SlideState::Hidden : SlideState::Shown;
			log(std::format(L"[dock] 滑动结束 -> {}", slideState == SlideState::Hidden ? L"隐藏" : L"展开"));
			// ⚠ 滑入完成后，若 dock 是展开态且开着自动隐藏，**必须**把兜底检测打开 ——
			//   从**热区**唤出的路径下 hoverIndex 永远是 -1，没有这一步就没人去开它，
			//   也就没人察觉"鼠标已经走了"（用户 2026-10-13 报的"很久不隐藏"）。
			if (cfgAutoHide && slideState != SlideState::Hidden) ensureHoverTimer(true);
			if (verboseSlide && s_frames > 0) {
				const ULONGLONG total = nowTick - slideStartTick;
				std::wstring seq;
				for (int i = 0; i < s_offsetN; ++i) {
					if (!seq.empty()) seq += L",";
					seq += std::format(L"{:.0f}", s_offsets[i]);
				}
				log(std::format(L"[slide] 汇总：{} 帧 / 实际 {}ms / 目标 {}ms / 最大帧间隔 {}ms / 平均 {:.1f}ms | 位移序列 {}",
					s_frames, total, slideDurMs, s_maxGap,
					static_cast<double>(total) / s_frames, seq));
				log(std::format(L"[slide] 移动窗口耗时：平均 {:.2f}ms / 最大 {:.2f}ms（{}）",
					s_moveTotal / s_frames, s_moveMax,
					s_skipMove ? L"本次跳过移动" : L"含移动"));
				s_lastTick = 0;
				s_frames = 0;
				s_maxGap = 0;
				s_offsetN = 0;
				s_moveTotal = 0.0;
				s_moveMax = 0.0;
			}
			// ⚠ 滑入结束后必须补一次"该不该收"的判断。
			//   触发滑入的来源不一定伴随鼠标在 dock 上（典型：全屏应用退出时
			//   无条件 slideIn）。不补这一下，dock 会一直停着不走，直到下次
			//   鼠标扫过才收 —— 实测这个漏判会留下一个"赖着不走的 dock"。
			if (slideState == SlideState::Shown) {
				if (shouldHideNow()) slideOut();
				else if (!cursorOverDock()) scheduleHide();
			}
		}
	}

	// ---------------------------------------------------------------------------
	// 全屏应用进入 / 退出（跟踪器事件驱动）。
	//  进入 → 若开了让位，立刻滑出（不等 500ms 延迟）
	//  退出 → 滑入，并把鼠标还给用户（不抢焦点，只是把 dock 放回来）
	// ---------------------------------------------------------------------------
	void DockWin::onFullscreenChanged(bool on)
	{
		fullscreenNow = on;
		log(std::format(L"[dock] 全屏应用 {}", on ? L"进入" : L"退出"));
		// ⚠ 这里以前有一句 `if (!cfgAutoHide) return;` —— 那让"全屏让位"
		//   偷偷依赖了自动隐藏。现在两者彻底解耦：只要 hideOnFullscreen 开着，
		//   自动隐藏关着也照样让位。

		// ⚠ 热区**不销毁**，但要让位时**停用命中**：
		//   1) 它全透明、不抢焦点、不进 Alt+Tab，全屏应用看不出来；
		//   2) 真按了它也没用 —— slideIn() / keepVisible() 都被
		//      shouldHideNow() 挡住，全屏期间 dock 不会拱出来；
		//   3) 探针的注入通道靠的是 hwnd 存活，`setEnabled(false)` 只 Hide
		//      不销毁，所以通道仍在；
		//   4) 而"藏着但可命中"会**吃掉全屏游戏/视频贴边那几像素的点击**
		//      （用户 2026-10-12 明确要求"全屏时不干扰"）。
		syncHotZoneEnabled();
		if (on) {
			if (cfgHideOnFullscreen) slideOut();
		}
		else {
			// 全屏退出：把 dock 放回来。但**不抢鼠标**——如果用户鼠标不在
			// dock 上，滑入结束后 tickSlide 会补一次 scheduleHide 把它收回去。
			slideIn();
		}
	}

	// ===========================================================================
	// 工作区预留（AppBar）
	// ===========================================================================

	// ---------------------------------------------------------------------------
	// 工作区预留（AppBar）
	// ---------------------------------------------------------------------------

	/// 强杀自愈用的状态文件路径（exe 同目录）。
	std::wstring DockWin::appBarStatePath() const
	{
		wchar_t buf[MAX_PATH * 2]{};
		GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
		std::filesystem::path p{ buf };
		return (p.parent_path() / L"zdock-appbar.state").wstring();
	}

	// ---------------------------------------------------------------------------
	// 按配置注册 / 注销 AppBar。
	// ⚠ 只在状态**变化**时才动系统：每次重建都 ABM_NEW/ABM_REMOVE 会让
	//   所有窗口的工作区反复抖动（用户能看到任务栏/窗口闪）。
	// ---------------------------------------------------------------------------
	bool DockWin::syncAppBar()
	{
		if (!hwnd) return false;

		appBar.onLog = [](const std::wstring& s) { log(s); };
		appBar.onWorkAreaChanged = [this] {
			// 系统改了停靠空间 → 重算 dock 位置（同时把热区跟过去）
			applyDockPlacement(slideState == SlideState::Hidden ? static_cast<int>(h) : 0);
			syncHotZone();
			};

		// 请求的矩形：只留面板那一块（不含 halo / slack），否则会向系统多要空间
		auto desiredPanelRect = [this] {
			RECT r = dockRectShown();
			r.left += static_cast<LONG>(px(kSideSlack));
			r.right -= static_cast<LONG>(px(kSideSlack));
			r.top = r.bottom - static_cast<LONG>(px(panelH()));
			return r;
			};

		if (cfgReserveWorkArea) {
			// 已经注册过：重报一次位置即可（register_ 内部会先注销再注册，
			// 这对 AppBar 是标准做法 —— 改位置就必须走 NEW/SETPOS 这套）。
			appBar.register_(hwnd, desiredPanelRect(), appBarStatePath(), ABE_BOTTOM);
			// ⚠ 注册之后工作区变了，得重新摆一次窗口位置（否则 dock 会还停在旧工作区上）
			applyDockPlacement(slideState == SlideState::Hidden ? static_cast<int>(h) : 0);
			syncHotZone();
			return appBar.registered();
		}

		if (appBar.registered()) {
			appBar.unregister_();
			// 注销后工作区恢复，窗口位置也要跟着回
			applyDockPlacement(slideState == SlideState::Hidden ? static_cast<int>(h) : 0);
			syncHotZone();
		}
		return false;
	}

	// ---------------------------------------------------------------------------
	// explorer 重启自愈（任务书 §2 功能表 #32）。
	//
	// explorer 崩了/被重启时，Windows 会广播一条 `TaskbarCreated` 消息
	// （**注意没有空格**，是 RegisterWindowMessageW(L"TaskbarCreated")）。
	// 任务栏重建意味着：AppBar 的协调关系没了、shell hook 也可能失效，
	// 所以这里重新走一遍注册。
	//
	// ⚠ 我们不在别人的消息循环里，收不到广播 → 由 main 的窗口过程转进来
	//   （见 main.cpp / setOnShellBroadcast）。
	// ---------------------------------------------------------------------------
	void DockWin::onTaskbarCreated()
	{
		log(L"[dock] 收到 TaskbarCreated 广播（explorer 重启），自愈中…");

		// 1) 跟踪器重建（shell hook 的接收窗口还在，但分组表要重扫一遍，
		//    否则重启前那些窗口的记录会留着）
		if (tracker.groups().empty() || true) {
			// 重新注册一次，确保 shell hook 是活的
			tracker.stop();
			if (!tracker.start()) {
				log(L"[dock] 跟踪器重启失败");
			}
		}
		tracker.rebuildAll();

		// 2) AppBar 重新登记（系统的 AppBar 协调表在任务栏重建后是空的）
		if (appBar.registered()) {
			appBar.unregister_();
		}
		syncAppBar();

		syncWithTracker();
		log(L"[dock] 自愈完成");
	}

	// ===========================================================================
	// 阶段五：拖文件到图标上打开（任务书 §2 #20）
	// ===========================================================================

	LRESULT CALLBACK DockWin::subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		DockWin* self = s_self;
		if (self && msg == WM_DROPFILES) {
			self->onDropFiles(reinterpret_cast<HDROP>(wp));
			return 0;
		}
		// 测试注入通道：打开设置窗口（探针点不到菜单，见头文件里的说明）
		if (self && msg == kMsgOpenSettings) {
			self->openSettings();
			return 0;
		}
		// ⚠ 其余消息**必须**原样转回 Ling 的窗口过程 —— 不转的话 dock 会
		//   整个失去输入与绘制处理（子类化是"包一层"，不是"换一个"）。
		if (self && self->origWndProc) {
			return CallWindowProcW(self->origWndProc, hwnd, msg, wp, lp);
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	bool DockWin::isDirectoryPath(const std::wstring& path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	void DockWin::onDropFiles(HDROP drop)
	{
		if (!drop) return;

		// 落点（客户区坐标）→ 命中哪个图标。
		// ⚠ 用 indexAtVisual 而不是 indexAtHover：用户是"看着图标"拖过去的，
		//   放大后的那块可见框才是他以为的目标。
		POINT pt{};
		DragQueryPoint(drop, &pt);
		const int idx = indexAtVisual(pt);

		// 收集拖进来的文件。目录先跳过 —— 本阶段只做"文件"（拖文件夹进来
		// 语义不明确：是复制整个目录树？还是只当成一个路径参数？先不做，
		// 但记一行日志说明为什么没反应）。
		const UINT total = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		std::vector<std::wstring> files;
		for (UINT i = 0; i < total; ++i) {
			const UINT len = DragQueryFileW(drop, i, nullptr, 0);
			if (!len) continue;
			std::wstring buf(len + 1, L'\0');
			DragQueryFileW(drop, i, buf.data(), len + 1);
			buf.resize(len);
			if (isDirectoryPath(buf)) {
				log(std::format(L"[drop] 跳过目录（只支持文件）：{}", buf));
				continue;
			}
			files.push_back(std::move(buf));
		}

		if (idx < 0) {
			log(std::format(L"[drop] 落点不在图标上（client={},{}）→ 忽略（共 {} 项）",
				pt.x, pt.y, files.size()));
			DragFinish(drop);
			return;
		}

		const std::wstring target = items[idx].path;
		if (files.empty()) {
			log(std::format(L"[drop] 拖到「{}」但没有可用的文件", target));
			DragFinish(drop);
			return;
		}

		if (isDirectoryPath(target)) {
			// ---- 拖到文件夹图标 → 复制进去（任务书 #20）----
			// ⚠ pFrom / pTo 都是**双 \0 结尾**的多字符串，不是普通字符串。
			std::wstring from;
			for (const auto& f : files) {
				from += f;
				from.push_back(L'\0');
			}
			from.push_back(L'\0');
			std::wstring to = target;
			to.push_back(L'\0');
			to.push_back(L'\0');

			SHFILEOPSTRUCTW op{};
			op.hwnd = hwnd;
			op.wFunc = FO_COPY;
			op.pFrom = from.c_str();
			op.pTo = to.c_str();
			// FOF_ALLOWUNDO：能撤销（进回收站语义）；不弹"是否创建目录"确认框
			op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
			const int rc = SHFileOperationW(&op);
			if (rc == 0 && !op.fAnyOperationsAborted) {
				log(std::format(L"[drop] 已复制 {} 个文件到「{}」", files.size(), target));
			}
			else {
				log(std::format(L"[drop] 复制到「{}」失败 rc={} aborted={}",
					target, rc, op.fAnyOperationsAborted ? 1 : 0));
			}
		}
		else {
			// ---- 拖到程序图标 → 用该程序打开 ----
			// 多文件时把它们组成一个引号包裹、空格分隔的参数串。
			// （带空格的路径必须加引号，否则会被拆成多个参数。）
			std::wstring args;
			for (size_t i = 0; i < files.size(); ++i) {
				if (i) args += L' ';
				args += L'"';
				args += files[i];
				args += L'"';
			}
			const std::filesystem::path tp{ target };
			const std::wstring workDir = tp.parent_path().wstring();
			const HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(),
				args.c_str(), workDir.empty() ? nullptr : workDir.c_str(), SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(r) > 32) {
				log(std::format(L"[drop] 已用「{}」打开 {} 个文件", target, files.size()));
			}
			else {
				log(std::format(L"[drop] 用「{}」打开失败 code={}",
					target, static_cast<long long>(reinterpret_cast<INT_PTR>(r))));
			}
		}

		DragFinish(drop);
		// 拖放刚结束，鼠标多半还在 dock 上 —— 别立刻把它收走
		keepVisible();
	}

	LRESULT DockWin::onHitTest(const POINT pos)
	{
		POINT pt = pos;
		ScreenToClient(hwnd, &pt);

		// 命中测试本身照旧返回真实的可点区域（面板 + 放大后的图标可见框），
		// 让系统把坐标算对。
		LRESULT result = HTTRANSPARENT;
		if (panel && inRect(pt, panel->x, panel->y, panel->w, panel->h)) result = HTCLIENT;
		else {
			const int idx = indexAtHover(pt);
			if (idx >= 0 && indexAtVisual(pt) >= 0) result = HTCLIENT;
		}

		// hover 的判定**不在**这里做 —— 统一交给 refreshHoverFromCursor()。
		// 这里只借"系统刚好在问命中"这个时机顺便同步一次，省一次多余的重算。
		refreshHoverFromCursor();

		// 命中结果的诊断日志：只在 ZDOCK_VERBOSE_HIT=1 时开，且只在"结果变了"或
		// "点位挪了 20px 以上"时记一行 —— 不然每次鼠标移动都写盘
		// （红线 4 的精神：别给自己造轮询）
		static const bool verboseHit = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_HIT", buf, 8) > 0;
			}();
		if (verboseHit) {
			static POINT lastPt{ -1000, -1000 };
			static LRESULT lastResult{ 12345 };
			if (result != lastResult || std::abs(pt.x - lastPt.x) > 20 || std::abs(pt.y - lastPt.y) > 20) {
				log(std::format(L"[hit] client=({},{}) -> {} hover={} panel=({},{},{},{})",
					pt.x, pt.y, result == HTCLIENT ? L"HTCLIENT" : L"HTTRANSPARENT", hoverIndex,
					(int)(panel ? panel->x : 0), (int)(panel ? panel->y : 0),
					(int)(panel ? panel->w : 0), (int)(panel ? panel->h : 0)));
				lastPt = pt;
				lastResult = result;
			}
		}

		// halo：合成窗口的透明区域照样参与命中测试，不放行就会挡住桌面的点击（红线 6）
		return result;
	}

} // namespace zdock
