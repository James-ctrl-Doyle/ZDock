#include "DockWin.h"
#include "Config.h"
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

	DockWin::DockWin() = default;

	DockWin::~DockWin()
	{
		// ⚠ 跟踪器的钩子必须在窗口销毁**之前**摘掉：它的回调会用到本对象，
		//   对象没了还留着钩子就是悬空指针。
		tracker.stop();
		// 热区窗口是独立顶层窗口，也得显式销毁（它不属于 Ling 的窗口体系）
		hotZone.destroy();
		// AppBar 注销：不注销的话工作区会一直缩着，用户得重启 explorer 才好。
		appBar.unregister_();
	}

	float DockWin::panelW() const
	{
		const float n = static_cast<float>(items.size());
		if (n <= 0) return 2 * kPadX;
		return n * cfgIconBase + (n - 1) * cfgIconGap + 2 * kPadX;
	}

	float DockWin::panelH() const
	{
		// 面板底部多留一条指示器的空间：指示器画在图标下缘之外（不挤压图标），
		// 但它落在面板范围内，否则会悬在面板外面很难看。
		return cfgIconBase + 2 * kPadY + px(kIndicatorDia + kIndicatorGap) / (dpi > 0.f ? dpi : 1.f);
	}

	float DockWin::iconsW() const
	{
		const float n = static_cast<float>(items.size());
		if (n <= 0) return 0.f;
		return n * cfgIconBase + (n - 1) * cfgIconGap;
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
		cfgHideOnFullscreen = cfg->hideOnFullscreen;
		cfgReserveWorkArea = cfg->reserveWorkArea;
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

		const float winW = panelW() + 2 * kSideSlack;   // 逻辑
		const float winH = kHaloH + panelH();
		setSize(winW, winH);                            // 内部 ×dpi，之后本对象的 w/h 是物理像素

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
				if (menuSessions > 0 || hoverIndex >= 0) return;
				if (refreshHoverFromCursor()) return;   // 光标还在上面
				slideOut();
				return;
			}
			if (id == kTimerSlide) {
				tickSlide();
				return;
			}
			if (id != kTimerHover) return;
			if (hoverIndex < 0) {
				ensureHoverTimer(false);
				return;
			}
			if (!refreshHoverFromCursor()) return;   // 鼠标还在，什么都不用做
			// 已经缩回并且光标确实不在了：定时器可以停了
			if (hoverIndex < 0) ensureHoverTimer(false);
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

		// 窗口没了就退进程：否则 DestroyWindow 之后消息循环还在空转，留一个
		// "没窗口没托盘"的僵尸进程（ZPin 那边踩过同款）
		onDestroy.add([] { Ling::App::get()->quit(0); });

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
		panel->setPosition(Ling::Edge::Left, kSideSlack);
		panel->setPosition(Ling::Edge::Top, kHaloH);
		panel->setSize(panelW(), panelH());
		// 背景色与圆角都来自 config.json（"#RRGGBBAA" 已在 Config 里校验过格式）
		panel->setBg(Ling::Color(Config::get()->bgColorValue()));
		panel->setBorderRadius(Config::get()->cornerRadius);
		panel->setBorder(1.f, Ling::Color(0xFFFFFF14));

		row = body->makeChild<Ling::Node>();
		row->setPositionType(Ling::Position::Absolute);
		row->setPosition(Ling::Edge::Left, kSideSlack + kPadX);
		row->setPosition(Ling::Edge::Top, kHaloH + kPadY);
		row->setSize(iconsW(), cfgIconBase);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::FlexEnd);

		// 指示器**不放进 row** —— row 是 Flex 容器，多出来的节点会被当成第二个
		// 图标参与排版。它们挂在 body 上，位置在 layout 完成后按图标坐标算。
		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = row->makeChild<IconNode>();
			node->setSize(cfgIconBase, cfgIconBase);
			if (i + 1 < items.size()) node->setMarginRight(cfgIconGap);
			items[i].node = node;

			auto* dot = body->makeChild<IndicatorNode>();
			dot->setPositionType(Ling::Position::Absolute);
			dot->setSize(kIndicatorDia, kIndicatorDia);
			dot->setOn(false);
			items[i].indicator = dot;
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
			if (overDock || menuSessions > 0) keepVisible();
			else scheduleHide();
		}

		if (idx == hoverIndex) return false;

		hoverIndex = idx;
		applyHover(idx);
		// 鼠标确实在图标上 → 确保兜底定时器开着；移开了 → 交给调用方关
		if (idx >= 0) ensureHoverTimer(true);
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

		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node) continue;
			float target = 1.f;
			if (index >= 0) {
				const float d = std::abs(static_cast<float>(i) - static_cast<float>(index));
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

		RECT panelRect = toWinRect(panel->x, panel->y, panel->w, panel->h);
		HRGN region = CreateRectRgnIndirect(&panelRect);
		if (!region) return;

		for (const auto& item : items) {
			auto* node = item.node;
			if (!node || node->w <= 0.f) continue;
			const float s = node->scale();
			if (s <= 1.002f) continue;   // 没放大就不占额外区域
			const float w = node->w * s;
			const float h = node->h * s;
			const float x = node->x + node->w * 0.5f - w * 0.5f;   // 以底边中点为锚
			const float y = node->y + node->h - h;
			RECT r = toWinRect(x, y, w, h);
			HRGN one = CreateRectRgnIndirect(&r);
			if (one) {
				CombineRgn(region, region, one, RGN_OR);
				DeleteObject(one);
			}
		}
		// 成功后 region 所有权归系统，不要再 DeleteObject
		SetWindowRgn(hwnd, region, TRUE);
	}

	void DockWin::launch(int index)
	{
		if (index < 0 || index >= static_cast<int>(items.size())) return;
		const auto& item = items[index];
		ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

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
		AppendMenuW(menu, MF_STRING, kMenuAdd, L"添加程序…");
		AppendMenuW(menu, MF_STRING, kMenuReload, L"重新载入配置");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 ZDock");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ZDock 0.1.4 · 自动隐藏 / 工作区预留");

		POINT pt{};
		GetCursorPos(&pt);
		const UINT cmd = popupMenuHere(menu, pt);
		switch (cmd) {
		case kMenuExit:   Ling::App::get()->quit(0); break;
		case kMenuReload: reloadConfig();            break;
		case kMenuAdd:    addItem();                 break;
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
			AppendMenuW(menu, MF_STRING, kMenuOpenAdmin, L"以管理员身份打开");
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, kMenuRemove, L"从 Dock 移除");
		}

		POINT pt{};
		GetCursorPos(&pt);
		const UINT cmd = popupMenuHere(menu, pt);
		switch (cmd) {
		case kMenuOpen:       launch(index);      break;
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

		// 固定项覆盖了哪些键（这些不该再出临时图标）
		std::vector<std::wstring> pinned;
		for (const auto& item : items) {
			if (item.temporary) continue;
			if (const AppGroup* g = tracker.findByExePath(item.path)) pinned.push_back(g->key);
		}

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
			if (std::find(pinned.begin(), pinned.end(), g.key) != pinned.end()) continue;
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
			auto* node = row->makeChild<IconNode>();
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
		// Ling 的节点由父节点持有，removeChild 会销毁它
		if (item.indicator && body) body->removeChild(item.indicator);
		if (item.node && row) row->removeChild(item.node);
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
		// row 是 Flex 行；间距是每个节点的 marginRight。项数变了要重设一遍：
		// 除最后一项外都要有间距。
		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = items[i].node;
			if (!node) continue;
			node->setSize(cfgIconBase, cfgIconBase);
			node->setMarginRight((i + 1 < items.size()) ? cfgIconGap : 0.f);
		}

		// 面板 / 行 / 窗口尺寸随项数变
		panel->setPosition(Ling::Edge::Left, kSideSlack);
		panel->setSize(panelW(), panelH());
		row->setSize(iconsW(), cfgIconBase);

		const float winW = panelW() + 2 * kSideSlack;
		const float winH = kHaloH + panelH();

		// 窗口尺寸变化的**同时**要把位置摆回"水平居中、贴底"，
		// 否则窗口会以左下角为锚、越来越靠右。
		setSize(winW, winH);
		applyDockPlacement(0);

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
			item.indicator->setOn(running);
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
		static const bool verbose = [] {
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"ZDOCK_VERBOSE_IND", buf, 8) > 0;
			}();
		const float d = (dpi > 0.f) ? dpi : 1.f;
		for (size_t i = 0; i < items.size(); ++i) {
			auto& item = items[i];
			if (!item.indicator || !item.node) continue;
			// 物理像素 → 逻辑像素
			const float cxLog = (item.node->x + item.node->w * 0.5f) / d;
			const float yLog = (item.node->y + item.node->h) / d + kIndicatorGap;
			item.indicator->setPosition(Ling::Edge::Left, cxLog - kIndicatorDia * 0.5f);
			item.indicator->setPosition(Ling::Edge::Top, yLog);
			if (verbose) {
				log(std::format(L"[ind] #{} node物理=({:.1f},{:.1f},{:.1f},{:.1f}) -> dot逻辑=({:.1f},{:.1f}) 面板逻辑=({:.1f},{:.1f},{:.1f},{:.1f})",
					i, item.node->x, item.node->y, item.node->w, item.node->h,
					cxLog - kIndicatorDia * 0.5f, yLog,
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
		RECT work = monitorRect();   // rcMonitor：不随 AppBar 预留变化

		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelBottom = work.bottom - static_cast<int>(px(Config::get()->bottomMargin));
		const int panelLeft = work.left + ((work.right - work.left) - panelWpx) / 2;

		// 窗口矩形比面板大（四周是透明 halo / slack），所以窗口左上角要往外让。
		RECT r{};
		r.left = panelLeft - static_cast<int>(px(kSideSlack));
		r.top = panelBottom - static_cast<int>(h);
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
	void DockWin::applyDockPlacement(int offsetY)
	{
		const RECT r = dockRectShown();
		shownOrigin.x = r.left;
		shownOrigin.y = r.top;
		setPosition(r.left, r.top + offsetY);
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
		// 自动化测试注入：探针用 PostMessage 直接把"全屏状态"喂进来，
		// 免得为了造一个真全屏前台窗口去 SetForegroundWindow（那会抢用户焦点）。
		hotZone.onTestInject = [this](bool on) { onFullscreenChanged(on); };
		hotZone.onLog = [](const std::wstring& s) { log(s); };

		if (cfgAutoHide) {
			const RECT hz = hotZoneRect();
			if (!hotZone.alive()) hotZone.create(hz);
			else hotZone.moveTo(hz);
			log(std::format(L"[dock] 自动隐藏已启用（延迟 {}ms / 滑入 {}ms / 滑出 {}ms / 全屏让位 {}）",
				cfgHideDelayMs, cfgSlideInMs, cfgSlideOutMs, cfgHideOnFullscreen ? 1 : 0));
		}
		else {
			// 关掉自动隐藏要立刻回到展开态并销毁热区 ——
			// 不销毁的话屏幕边上会留一条看不见却吃点击的窗口。
			hotZone.destroy();
			if (slideState != SlideState::Shown) {
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
		RECT work = monitorRect();

		const int screenW = work.right - work.left;
		const int panelWpx = static_cast<int>(px(panelW()));
		const int wantW = std::max(panelWpx, screenW / 2);

		const int thick = 3;   // 物理像素，任务书 §4 定 3px
		const int cx = work.left + screenW / 2;

		RECT r{};
		r.left = cx - wantW / 2;
		r.right = r.left + wantW;
		// 贴屏幕底边（不是工作区底边）—— 热区是碰鼠标用的，
		// 必须待在"用户以为 dock 该出现的那条边"上。
		r.bottom = work.bottom;
		r.top = r.bottom - thick;
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
	}

	// ---------------------------------------------------------------------------
	// 当前是否"该隐藏"：自动隐藏开着，且（全屏应用在前台 且 开了全屏让位）。
	// ⚠ 这里**不含**"鼠标在不在面板上"的判定 —— 那由 keepVisible / 延迟管，
	//   否则鼠标一离开就会立刻隐藏，任务书 §5 要的是"约 500ms 后"。
	// ---------------------------------------------------------------------------
	bool DockWin::shouldHideNow() const
	{
		if (!cfgAutoHide) return false;
		if (cfgHideOnFullscreen && fullscreenNow) return true;
		return false;
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
		HWND under = WindowFromPoint(pt);
		return under == hwnd;
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
		if (!cfgAutoHide) return;
		if (menuSessions > 0) return;             // 菜单开着绝不隐藏
		if (shouldHideNow()) { slideOut(); return; }   // 全屏应用在前台：立刻让位
		if (hideDelayTimerOn) return;
		hideDelayTimerOn = true;
		setTimer(cfgHideDelayMs, kTimerAutoHide);
	}

	// ---------------------------------------------------------------------------
	// 滑入（展开）。时长 cfgSlideInMs（默认 200ms）。
	// ---------------------------------------------------------------------------
	void DockWin::slideIn()
	{
		if (!cfgAutoHide || !hwnd) return;
		// ⚠ 全屏应用在前台时**保持隐藏**（任务书 §3："全屏应用前台 → 保持隐藏"）。
		//   热区碰一下也要拦住 —— 否则全屏游戏/视频时鼠标扫过屏幕底边，
		//   dock 会从全屏画面底下拱出来。
		if (shouldHideNow()) return;
		if (slideState == SlideState::Shown && slideT <= 0.f) return;

		// 从当前实际位置开始补间（避免滑动中途反向时跳变）
		RECT cur{};
		GetWindowRect(hwnd, &cur);
		slideFrom = static_cast<float>(cur.top - shownOrigin.y);
		slideTo = 0.f;
		slideT = slideFrom;
		slideDurMs = cfgSlideInMs;
		slideStartTick = GetTickCount64();
		slideState = SlideState::Sliding;
		if (!slideTimerOn) { slideTimerOn = true; setTimer(16, kTimerSlide); }
		log(std::format(L"[dock] 滑入开始（{:.0f}px / {}ms）", slideFrom, slideDurMs));
	}

	// ---------------------------------------------------------------------------
	// 滑出（隐藏）。时长 cfgSlideOutMs（默认 300ms，ease-out）。
	// ⚠ 滑出的位移 = 面板高度 + halo 余量，保证整个窗口（含上方 halo）都在屏幕外。
	//   用窗口的高 h 而不是面板高：halo 那圈虽然透明，但它是窗口的一部分。
	// ---------------------------------------------------------------------------
	void DockWin::slideOut()
	{
		if (!cfgAutoHide || !hwnd) return;
		if (slideState == SlideState::Hidden && slideT >= 1.f) return;

		RECT cur{};
		GetWindowRect(hwnd, &cur);
		slideFrom = static_cast<float>(cur.top - shownOrigin.y);
		// 目标：整个窗口滑到屏幕底边之外（同样锚监视器，不受工作区影响）
		const RECT mon = monitorRect();
		const float full = static_cast<float>(mon.bottom - shownOrigin.y);
		slideTo = full;
		slideT = (slideTo > 0.f) ? (slideFrom / slideTo) : 1.f;
		slideDurMs = cfgSlideOutMs;
		slideStartTick = GetTickCount64();
		slideState = SlideState::Sliding;
		if (!slideTimerOn) { slideTimerOn = true; setTimer(16, kTimerSlide); }
		log(std::format(L"[dock] 滑出开始（{:.0f} → {:.0f}px / {}ms）", slideFrom, slideTo, slideDurMs));
	}

	// ---------------------------------------------------------------------------
	// 滑动补间。16ms 一帧（约 60fps）。
	//
	// ⚠ 用**定时器补间 + SetWindowPos**，不用 Composition 动画：
	//   Composition 动的是节点，动不了窗口在屏幕上的位置。而"滑出屏幕"必须动窗口。
	//   这是任务书 §3 允许的实现 —— 它只约束时长与缓动（200/300ms ease-out）。
	// ---------------------------------------------------------------------------
	void DockWin::tickSlide()
	{
		if (!hwnd || slideState != SlideState::Sliding) {
			if (slideTimerOn) { killTimer(kTimerSlide); slideTimerOn = false; }
			return;
		}

		// ease-out cubic：1-(1-t)^3
		float lin = (slideDurMs > 0)
			? static_cast<float>(GetTickCount64() - slideStartTick) / static_cast<float>(slideDurMs)
			: 1.f;
		if (lin > 1.f) lin = 1.f;
		const float e = 1.f - std::pow(1.f - lin, 3.f);

		const float dist = slideFrom + (slideTo - slideFrom) * e;
		setPosition(shownOrigin.x, shownOrigin.y + static_cast<int>(std::lround(dist)));

		if (lin >= 1.f) {
			killTimer(kTimerSlide);
			slideTimerOn = false;
			slideT = (slideTo > 0.f) ? 1.f : 0.f;
			slideState = (slideTo > 0.f && std::abs(slideTo) > 1.f) ? SlideState::Hidden : SlideState::Shown;
			log(std::format(L"[dock] 滑动结束 -> {}", slideState == SlideState::Hidden ? L"隐藏" : L"展开"));
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
		if (!cfgAutoHide) return;

		// ⚠ 热区**不销毁**。理由：
		//   1) 它已经全透明、不抢焦点、不进 Alt+Tab，全屏应用看不出来；
		//   2) 真按了它也没用 —— slideIn() / keepVisible() 都被
		//      shouldHideNow() 挡住，全屏期间 dock 不会拱出来；
		//   3) 留着它，自动化测试才有稳定的注入通道
		//      （销毁了 hwnd 就失效，探针没法再驱动状态机）。
		//   早期版本在全屏时销毁热区，实测发现会让探针的注入通道断掉。
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
