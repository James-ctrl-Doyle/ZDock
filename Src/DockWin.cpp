#include "DockWin.h"
#include "IconLoader.h"
#include "Log.h"
#include <include/App.h>
#include <include/D2D.h>

#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <format>

namespace zdock {

	DockWin::DockWin() = default;
	DockWin::~DockWin() = default;

	float DockWin::panelW() const
	{
		const float n = static_cast<float>(items.size());
		if (n <= 0) return 2 * kPadX;
		return n * kIconBase + (n - 1) * kIconGap + 2 * kPadX;
	}

	float DockWin::panelH() const { return kIconBase + 2 * kPadY; }

	float DockWin::iconsW() const
	{
		const float n = static_cast<float>(items.size());
		if (n <= 0) return 0.f;
		return n * kIconBase + (n - 1) * kIconGap;
	}

	bool DockWin::inRect(POINT pt, float x, float y, float w, float h)
	{
		return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
	}

	// ---------------------------------------------------------------------------
	// 阶段一：图标列表先写死在候选表里（取存在的前 6 个）。
	// 持久化配置在阶段二接上 —— 那时这份表就是"首次启动的默认值"。
	// ---------------------------------------------------------------------------
	void DockWin::collectDefaultItems()
	{
		wchar_t winDir[MAX_PATH]{};
		if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return;
		const std::wstring root{ winDir };

		const std::wstring candidates[] = {
			root + L"\\explorer.exe",
			root + L"\\System32\\notepad.exe",
			root + L"\\System32\\mspaint.exe",
			root + L"\\System32\\cmd.exe",
			root + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
			root + L"\\System32\\SnippingTool.exe",
			root + L"\\System32\\calc.exe",
			root + L"\\System32\\regedit.exe",
		};

		for (const auto& path : candidates) {
			if (items.size() >= 6) break;
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
			DockItem item;
			item.path = path;
			item.name = displayNameOf(path);
			items.push_back(std::move(item));
		}
	}

	void DockWin::create()
	{
		collectDefaultItems();
		log(std::format(L"[dock] 候选图标命中 {} 个", items.size()));

		const float winW = panelW() + 2 * kSideSlack;   // 逻辑
		const float winH = kHaloH + panelH();
		setSize(winW, winH);                            // 内部 ×dpi，之后本对象的 w/h 是物理像素

		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int panelWpx = static_cast<int>(px(panelW()));
		const int panelBottom = work.bottom - static_cast<int>(px(kBottomMargin));
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
				showContextMenu();
				return;
			}
			const int idx = indexAtVisual(pt);
			if (idx >= 0) launch(idx);
			});

		// 窗口没了就退进程：否则 DestroyWindow 之后消息循环还在空转，留一个
		// "没窗口没托盘"的僵尸进程（ZPin 那边踩过同款）
		onDestroy.add([] { Ling::App::get()->quit(0); });

		layout();   // 立刻布一次局，别等第一次 WM_PAINT
		updateHitRegion();   // 初始命中区域 = 面板本体（halo 完全不参与命中）
		refresh();
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
		panel->setBg(Ling::Color(0x1A1A1ACC));
		panel->setBorderRadius(12.f);
		panel->setBorder(1.f, Ling::Color(0xFFFFFF14));

		row = body->makeChild<Ling::Node>();
		row->setPositionType(Ling::Position::Absolute);
		row->setPosition(Ling::Edge::Left, kSideSlack + kPadX);
		row->setPosition(Ling::Edge::Top, kHaloH + kPadY);
		row->setSize(iconsW(), kIconBase);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::FlexEnd);

		for (size_t i = 0; i < items.size(); ++i) {
			auto* node = row->makeChild<IconNode>();
			node->setSize(kIconBase, kIconBase);
			if (i + 1 < items.size()) node->setMarginRight(kIconGap);
			items[i].node = node;
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
		const int targetPx = static_cast<int>(std::lround(px(kIconBase) * kHoverPeak));
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
		const float half = px(kIconGap) * 0.5f;
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
				target = 1.f + (kHoverPeak - 1.f) * std::exp(-(d / kHoverSigma) * (d / kHoverSigma));
			}
			node->animateScale(target, kAnimMs);
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

	void DockWin::showContextMenu()
	{
		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 ZDock");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ZDock 0.1.0 · 阶段一");

		// App::popupMenu 内部 TrackPopupMenuEx(TPM_RETURNCMD) 并负责 DestroyMenu
		const UINT cmd = Ling::App::get()->popupMenu(menu);
		if (cmd == kMenuExit) Ling::App::get()->quit(0);
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
