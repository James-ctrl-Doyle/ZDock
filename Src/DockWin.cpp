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

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int panelWpx = static_cast<int>(px(panelW()));
		// 底边留白取自 config：任务栏自动隐藏时调大些可以少抢底部热区
		const int panelBottom = work.bottom - static_cast<int>(px(Config::get()->bottomMargin));
		const int panelLeft = work.left + ((work.right - work.left) - panelWpx) / 2;
		const int winLeft = panelLeft - static_cast<int>(px(kSideSlack));
		const int winTop = panelBottom - static_cast<int>(h);
		setPosition(winLeft, winTop);
		log(std::format(L"[dock] dpi={:.2f} 窗口物理 {}x{} @ ({},{})", dpi, w, h, winLeft, winTop));

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
		if (under == hwnd) {
			POINT client = pt;
			ScreenToClient(hwnd, &client);
			idx = indexAtHover(client);
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
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ZDock 0.1.2 · 配置持久化");

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

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelBottom = work.bottom - static_cast<int>(px(Config::get()->bottomMargin));
		const int panelLeft = work.left + ((work.right - work.left) - panelWpx) / 2;
		setPosition(panelLeft - static_cast<int>(px(kSideSlack)), panelBottom - static_cast<int>(h));

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

		// 全屏变化 → 阶段四会用来让位；阶段三先只记日志
		tracker.onFullscreenChanged = [](bool on) {
			log(std::format(L"[dock] 全屏应用 {}", on ? L"进入" : L"退出"));
			};

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
		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const float oldH = h;   // setSize 会改 h
		setSize(winW, winH);
		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelBottom = work.bottom - static_cast<int>(px(Config::get()->bottomMargin));
		const int panelLeft = work.left + ((work.right - work.left) - panelWpx) / 2;
		(void)oldH;
		setPosition(panelLeft - static_cast<int>(px(kSideSlack)),
			panelBottom - static_cast<int>(h));

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
