#include "SettingsWin.h"
#include "AutoStart.h"
#include "Config.h"
#include "Log.h"
#include "MonitorUtil.h"

#include <include/Button.h>
#include <include/Label.h>
#include <include/ScrollerBox.h>
#include <include/Slider.h>
#include <include/TextBox.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace zdock {

	using Ling::Button;
	using Ling::Color;
	using Ling::Label;
	using Ling::ScrollerBox;
	using Ling::Slider;
	using Ling::TextBox;

	namespace {
		// 一套跟 dock 面板同调性的配色（面板是 #1A1A1A @80%，强调色 #4CC2FF）
		constexpr uint32_t kBg = 0x1C1C1CFF;          // 窗口底
		constexpr uint32_t kNavBg = 0x171717FF;       // 左侧导航底
		constexpr uint32_t kNavOn = 0x2A3F7AFF;       // 导航选中（深蓝）
		constexpr uint32_t kNavHover = 0x2A2A2AFF;
		constexpr uint32_t kText = 0xDCDCDCFF;        // 标签：中亮的灰（参考图也是中性灰，不抢眼）
		constexpr uint32_t kTextDim = 0x9A9A9AFF;
		// ⚠ 强调色取的是参考图里那个亮蓝（#597EF7）—— 它是**设置窗口专用**的，
		//   跟 dock 面板指示器的青色 #4CC2FF 不是同一个。用户明确要"蓝色强调"。
		constexpr uint32_t kAccent = 0x597EF7FF;
		constexpr uint32_t kBtnBg = 0x2E2E2EFF;
		constexpr uint32_t kBtnHover = 0x3C3C3CFF;
		constexpr uint32_t kBtnOn = 0x597EF7FF;       // 选中 / 开启态（亮蓝，同参考图）
		constexpr uint32_t kTrack = 0x3A3A3AFF;
		constexpr uint32_t kLine = 0x303030FF;        // 行分隔线
		constexpr uint32_t kField = 0x2A2A2AFF;       // 输入框底
	} // namespace

	SettingsWin::~SettingsWin() = default;

	// ---------------------------------------------------------------------------
	// 建窗口：骨架（背景 / 左导航底 / 标题 / 内容滚动区 / 关闭按钮）
	// ---------------------------------------------------------------------------
	void SettingsWin::onCreated()
	{
		auto* bg = body->makeChild<Ling::Node>();
		bg->setPositionType(Ling::Position::Absolute);
		bg->setPosition(Ling::Edge::Left, 0.f);
		bg->setPosition(Ling::Edge::Top, 0.f);
		bg->setSize(kWinW, kWinH);
		bg->setBg(Color(kBg));
		bg->setBorderRadius(10.f);
		bg->setBorder(1.f, Color(0xFFFFFF1A));

		auto* title = body->makeChild<Label>();
		title->setPositionType(Ling::Position::Absolute);
		title->setPosition(Ling::Edge::Left, 20.f);
		title->setPosition(Ling::Edge::Top, 15.f);
		title->setText(L"ZDock 设置");
		title->setFontSize(15.f);
		title->setColor(Color(kAccent));

		// ⚠ 变量别叫 close —— 会把 SettingsWin::close() 遮住
		auto* btnClose = body->makeChild<Button>();
		btnClose->setPositionType(Ling::Position::Absolute);
		btnClose->setPosition(Ling::Edge::Left, kWinW - 48.f);
		btnClose->setPosition(Ling::Edge::Top, 13.f);
		btnClose->setSize(32.f, 28.f);
		btnClose->setFontSize(14.f);
		btnClose->setBorderRadius(6.f);
		btnClose->setText(L"×");
		btnClose->setBg(Color(kBg));
		btnClose->setColor(Color(kText));
		btnClose->setHoverBg(Color(0x8B2E2EFF));
		btnClose->setHoverColor(Color(0xFFFFFFFF));
		btnClose->onClick.add([this](Button*) { this->close(); });

		// 右侧内容区：可滚动。所有分组的所有行都堆在它的 content 里，
		// 左侧导航只负责"滚过去"—— 不重建控件。
		scroller = body->makeChild<ScrollerBox>();
		scroller->setPositionType(Ling::Position::Absolute);
		scroller->setPosition(Ling::Edge::Left, 0.f);
		scroller->setPosition(Ling::Edge::Top, kTitleH);
		scroller->setSize(kWinW, kWinH - kTitleH);
		content = scroller->makeChild<Ling::Node>();   // ScrollerBox::setChild 会接管
	}

	void SettingsWin::createOnce()
	{
		if (hwnd) return;
		setSize(kWinW, kWinH);
		setPosition(0, 0);
		// ⚠ 用 WS_POPUP 而不是带标题栏的样式：这个程序整体自绘，塞个系统标题栏观感会裂。
		//   拖动靠 onHitTest 在标题条区域返回 HTCAPTION（系统会把拖动接过去）。
		//   ⚠ 这里**不加** WS_EX_NOACTIVATE —— 用户是主动打开设置窗口的，
		//     它需要能收键盘焦点（数值输入框要用）。
		createNativeWindow(WS_EX_TOOLWINDOW, WS_POPUP);
		if (hwnd) SetWindowTextW(hwnd, L"ZDockSettings");
		buildUiOnce();
		layout();
		refresh();
	}

	// ---------------------------------------------------------------------------
	// 行构造器
	// ---------------------------------------------------------------------------
	void SettingsWin::divider(float y)
	{
		auto* line = content->makeChild<Ling::Node>();
		line->setPositionType(Ling::Position::Absolute);
		line->setPosition(Ling::Edge::Left, kPadX);
		line->setPosition(Ling::Edge::Top, y);
		line->setSize(kWinW - 2 * kPadX, 1.f);
		line->setBg(Color(kLine));
	}

	void SettingsWin::rowLabel(float y, const wchar_t* text)
	{
		auto* lab = content->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 10.f);   // 行高 39、字高约 18 -> 垂直居中
		lab->setText(text);
		lab->setFontSize(13.f);
		lab->setColor(Color(kText));
	}

	float SettingsWin::section(float y, const wchar_t* title)
	{
		auto* lab = content->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 18.f);
		lab->setText(title);
		lab->setFontSize(15.f);
		lab->setColor(Color(kAccent));
		return y + kSectionH;
	}

	float SettingsWin::sliderRow(float y, const wchar_t* label,
		float min, float max, float step,
		std::function<float()> getter, std::function<void(float)> setter)
	{
		rowLabel(y, label);

		// ⚠ 精度跟着步长走：step=1 的项显示整数，step=0.05/0.01 的项必须显示小数 ——
		//   统一用 {:.0f} 的话"悬停放大 1.70"会显示成"2"、"不透明度 0.80"会显示成"1"。
		const bool fractional = (step < 0.95f);
		auto fmt = [fractional](float v) {
			return fractional ? std::format(L"{:.2f}", v) : std::format(L"{:.0f}", v);
			};

		const float x0 = kPadX + kLabelW;
		auto* sl = content->makeChild<Slider>();
		sl->setPositionType(Ling::Position::Absolute);
		sl->setPosition(Ling::Edge::Left, x0);
		sl->setPosition(Ling::Edge::Top, y + 9.f);
		sl->setSize(kSliderW, 20.f);
		sl->setRange(min, max);
		sl->setStep(step);
		sl->setValue(getter());
		sl->setTrackColor(Color(kTrack));
		sl->setFillColor(Color(kAccent));
		sl->setThumbColor(Color(kText));
		sl->setHoverThumbColor(Color(kAccent));

		// ---- 数值输入框 ----
		// 可输入（拖滑块只能拖个大概，想精确改成 64 得手输）。
		// ⚠ 高度给足：这是个**多行**输入框，内容区比文本矮就会冒出滚动条
		//   （第一版 26 逻辑高就带了一条，用户一眼看到）。单行文本 + 32 高足够。
		auto* box = content->makeChild<TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		box->setPosition(Ling::Edge::Left, x0 + kSliderW + 14.f);
		box->setPosition(Ling::Edge::Top, y + 4.f);
		box->setSize(kNumW, 32.f);
		box->setFontSize(13.f);
		box->setVerticalCenter(true);
		box->setPaddingLeft(8.f);
		box->setPaddingRight(6.f);
		box->setBg(Color(kField));
		box->setBorder(1.f, Color(0x3E3E3EFF));   // 深色底下不加描边会糊在一起
		box->setBorderRadius(6.f);
		box->setColor(Color(kText));
		box->setCaretColor(Color(kAccent));
		box->setSelectionBgColor(Color(0x4CC2FF55));
		box->setText(fmt(getter()));
		numericBoxes.push_back(box);

		sl->onValueChanged.add([this, setter, box, fmt](Slider*, float v) {
			setter(v);
			box->setText(fmt(v));       // 拖滑块 → 输入框跟着变
			if (onLiveChanged) onLiveChanged();
			});

		// 输入框 → 滑块：**失焦时**才解析应用（输入过程中每敲一个字符就改配置太跳）。
		// 按回车也算确认 —— WinBase::onKeyDown 里拦一下让它失焦（见 buildUiOnce 末尾）。
		box->onFocusChanged.add([this, box, getter, setter, sl, min, max, step, fmt](
			Ling::TextBox*, bool focused) {
			if (focused) return;

			const std::wstring cur = box->getText();
			// 从文本里抠出数字。step < 1 的项才允许小数点；
			// 其余（"限制为整数"那些）小数点直接被丢掉。
			const bool allowDot = (step < 0.95f);
			std::wstring digits;
			bool neg = false;
			for (wchar_t c : cur) {
				if (c == L'-' && digits.empty()) { neg = true; continue; }
				if (c >= L'0' && c <= L'9') { digits += c; continue; }
				if (allowDot && c == L'.' && digits.find(L'.') == std::wstring::npos) digits += c;
			}

			float v = getter();
			if (!digits.empty() && digits != L".") {
				try { v = std::stof(digits); }
				catch (const std::exception&) { v = getter(); }
				if (neg) v = -v;
			}
			v = std::clamp(v, min, max);   // 手输 999 不该把面板撑爆
			setter(v);
			box->setText(fmt(v));          // 规范化回写（顺便清掉非法字符）
			sl->setValue(v);               // ⚠ 值没变的话 Slider 不会再触发 onValueChanged
			if (onLiveChanged) onLiveChanged();
			});

		refreshers.push_back([sl, box, getter, fmt] {
			const float v = getter();
			sl->setValue(v);
			box->setText(fmt(v));
			});

		divider(y + kRowH);
		return y + kRowH;
	}

	float SettingsWin::toggleRow(float y, const wchar_t* label,
		std::function<bool()> getter, std::function<void(bool)> setter)
	{
		rowLabel(y, label);

		// 开关做成"**无背景的文字按钮**"，用文字颜色表示状态 ——
		// 这是从 ZPin 的设置页搬过来的做法（同一个作者、同一套 Ling）：
		// 开 = 亮蓝字，关 = 灰字，悬停才有底色。比"蓝底白字的方块"轻，
		// 整行看上去就是"标签 ...... 状态"，不会被一排色块切碎。
		auto* b = content->makeChild<Button>();
		b->setPositionType(Ling::Position::Absolute);
		b->setPosition(Ling::Edge::Left, kPadX + kLabelW);
		b->setPosition(Ling::Edge::Top, y + 1.f);
		b->setSize(kWinW - 2 * kPadX - kLabelW, kRowH - 2.f);
		b->setFontSize(13.f);
		b->setBorderRadius(4.f);
		b->setBg(Color(0x00000000));        // 平时没有底色
		b->setHoverBg(Color(kBtnHover));
		b->setHoverColor(Color(kAccent));   // 悬停时字更亮一点

		auto paint = [b](bool on) {
			b->setText(on ? L"已开启" : L"未开启");
			b->setColor(Color(on ? kAccent : kTextDim));
			b->setHoverColor(Color(on ? kAccent : kText));
			};
		paint(getter());

		b->onClick.add([this, b, getter, setter, paint](Button*) {
			const bool next = !getter();
			setter(next);
			paint(next);
			if (onLiveChanged) onLiveChanged();
			});
		refreshers.push_back([b, getter, paint] { paint(getter()); });

		divider(y + kRowH);
		return y + kRowH;
	}

	float SettingsWin::choiceRow(float y, const wchar_t* label,
		std::initializer_list<std::pair<const wchar_t*, std::function<bool()>>> opts,
		std::function<void(int)> picker)
	{
		// ⚠ 先把选项抄进 vector 再往下用：`initializer_list` 只在"当前这条语句"
		//   的临时数组活着，直接把它按值捕获到 refreshers 里 = 抓了一把悬空指针
		//   （刷新时遍历的就是已析构的内存）。
		std::vector<std::pair<std::wstring, std::function<bool()>>> onVec;
		onVec.reserve(opts.size());
		for (const auto& [text, isOn] : opts) onVec.emplace_back(text, isOn);

		rowLabel(y, label);

		float x = kPadX + kLabelW;
		int idx = 0;
		std::vector<Button*> btns;
		for (const auto& [text, isOn] : onVec) {
			auto* b = content->makeChild<Button>();
			b->setPositionType(Ling::Position::Absolute);
			b->setPosition(Ling::Edge::Left, x);
			b->setPosition(Ling::Edge::Top, y + 5.f);
			b->setSize(kBtnW, 28.f);
			b->setFontSize(13.f);
			b->setBorderRadius(6.f);
			b->setHoverBg(Color(kBtnHover));
			b->setHoverColor(Color(0xFFFFFFFF));
			b->setText(text);
			b->setBg(Color(isOn() ? kBtnOn : kBtnBg));
			b->setColor(Color(kText));
			const int myIdx = idx++;
			btns.push_back(b);
			b->onClick.add([this, picker, myIdx](Button*) {
				picker(myIdx);
				if (onLiveChanged) onLiveChanged();
				// 立刻把高亮刷过来（否则用户点了"左"、看着"下"还亮着）
				syncFromConfig();
				// 点了导航项之后要把它滚到顶，别停在半截
				});
			x += kBtnW + 8.f;
		}
		refreshers.push_back([btns, onVec] {
			for (size_t i = 0; i < btns.size() && i < onVec.size(); ++i) {
				btns[i]->setBg(Color(onVec[i].second() ? kBtnOn : kBtnBg));
			}
			});
		divider(y + kRowH);
		return y + kRowH;
	}

	float SettingsWin::noteRow(float y, const wchar_t* text)
	{
		auto* lab = content->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 11.f);
		lab->setText(text);
		lab->setFontSize(12.f);
		lab->setColor(Color(kTextDim));
		return y + kRowH;
	}

	// ---------------------------------------------------------------------------
	// 整页布局：左侧导航 + 右侧全部设置行
	// ---------------------------------------------------------------------------
	void SettingsWin::buildUi()
	{
		auto* cfg = Config::get();

		float y = 8.f;

		// ---- 外观 ----
		y = section(y, L"外观");
		y = sliderRow(y, L"图标大小", 32.f, 128.f, 1.f,
			[cfg] { return cfg->iconSize; },
			[cfg](float v) { cfg->iconSize = v; });
		y = sliderRow(y, L"图标间距", 0.f, 40.f, 1.f,
			[cfg] { return cfg->iconGap; },
			[cfg](float v) { cfg->iconGap = v; });
		y = sliderRow(y, L"悬停放大", 1.0f, 2.5f, 0.05f,
			[cfg] { return cfg->hoverScale; },
			[cfg](float v) { cfg->hoverScale = v; });
		y = sliderRow(y, L"面板不透明度", 0.5f, 0.95f, 0.01f,
			[cfg] { return cfg->opacity; },
			[cfg](float v) { cfg->opacity = v; });
		y = sliderRow(y, L"圆角", 0.f, 32.f, 1.f,
			[cfg] { return cfg->cornerRadius; },
			[cfg](float v) { cfg->cornerRadius = v; });
		y = toggleRow(y, L"运行指示器",
			[cfg] { return cfg->showIndicator; },
			[cfg](bool v) { cfg->showIndicator = v; });

		// ---- 位置 ----
		y = section(y, L"位置");
		y = choiceRow(y, L"停靠边",
			{ {L"下", [cfg] { return cfg->dockEdge == L"bottom"; }},
			  {L"上", [cfg] { return cfg->dockEdge == L"top"; }},
			  {L"左", [cfg] { return cfg->dockEdge == L"left"; }},
			  {L"右", [cfg] { return cfg->dockEdge == L"right"; }} },
			[cfg](int i) {
				cfg->dockEdge = (i == 1) ? L"top" : (i == 2) ? L"left" : (i == 3) ? L"right" : L"bottom";
			});
		y = choiceRow(y, L"对齐",
			{ {L"起始", [cfg] { return cfg->dockAlign == L"start"; }},
			  {L"居中", [cfg] { return cfg->dockAlign == L"center"; }},
			  {L"末端", [cfg] { return cfg->dockAlign == L"end"; }} },
			[cfg](int i) {
				cfg->dockAlign = (i == 1) ? L"center" : (i == 2) ? L"end" : L"start";
			});
		y = sliderRow(y, L"沿边偏移", -400.f, 400.f, 1.f,
			[cfg] { return cfg->dockOffset; },
			[cfg](float v) { cfg->dockOffset = v; });

		// 显示器：多屏时才列出来
		{
			const auto mons = monitors::enumerate();
			const int n = static_cast<int>(mons.size());
			if (n <= 1) {
				y = noteRow(y, L"显示器    只有一台（接多屏后这里会列出可选项）");
			}
			else {
				y = choiceRow(y, L"显示器",
					{ {L"跟随", [cfg] { return cfg->monitorIndex < 0; }},
					  {L"1", [cfg] { return cfg->monitorIndex == 0; }},
					  {L"2", [cfg] { return cfg->monitorIndex == 1; }},
					  {L"3", [cfg] { return cfg->monitorIndex == 2; }} },
					[cfg, n](int i) {
						const int want = i - 1;   // 0 = 跟随
						cfg->monitorIndex = (want >= 0 && want < n) ? want : -1;
					});
			}
		}

		// ---- 行为 ----
		y = section(y, L"行为");
		y = toggleRow(y, L"自动隐藏",
			[cfg] { return cfg->autoHide; },
			[cfg](bool v) { cfg->autoHide = v; });
		y = toggleRow(y, L"全屏时让位",
			[cfg] { return cfg->hideOnFullscreen; },
			[cfg](bool v) { cfg->hideOnFullscreen = v; });
		y = toggleRow(y, L"预留工作区",
			[cfg] { return cfg->reserveWorkArea; },
			[cfg](bool v) { cfg->reserveWorkArea = v; });

		// ---- 启动 ----
		y = section(y, L"启动");
		// ⚠ getter 读的是**注册表真值**（autostart::isEnabled）而不是配置字段 ——
		//   用户可能在别处（任务管理器 / 注册表）关掉过自启，那才是事实。
		//   setter 两个都写，保证配置里那份不漂。
		y = toggleRow(y, L"开机自启",
			[] { return autostart::isEnabled(); },
			[cfg](bool v) {
				autostart::set(v);
				cfg->autoStart = v;
			});

		// ---- 底部：恢复默认外观 ----
		divider(y + 6.f);
		y += 20.f;
		auto* reset = content->makeChild<Button>();
		reset->setPositionType(Ling::Position::Absolute);
		reset->setPosition(Ling::Edge::Left, kPadX);
		reset->setPosition(Ling::Edge::Top, y);
		reset->setSize(118.f, 32.f);
		reset->setFontSize(13.f);
		reset->setBorderRadius(6.f);
		reset->setText(L"恢复默认");
		reset->setBg(Color(kBtnBg));
		reset->setColor(Color(kText));
		reset->setHoverBg(Color(kBtnHover));
		reset->setHoverColor(Color(0xFFFFFFFF));
		reset->onClick.add([this](Button*) {
			auto* c = Config::get();
			c->iconSize = 48.f;
			c->iconGap = 12.f;
			c->hoverScale = 1.7f;
			c->opacity = 0.8f;
			c->cornerRadius = 12.f;
			c->showIndicator = true;
			c->dockEdge = L"bottom";
			c->dockAlign = L"center";
			c->dockOffset = 0.f;
			syncFromConfig();
			if (onLiveChanged) onLiveChanged();
			log(L"[settings] 已恢复默认外观 / 位置");
			});
		y += 46.f;

		// content 的高度要显式给足 —— ScrollerBox 靠它算 maxScroll
		content->setSize(kWinW, y + 16.f);
	}

	void SettingsWin::buildUiOnce()
	{
		buildUi();

		// 回车 = 确认：让当前聚焦的数值输入框失焦，触发"解析 + 应用"。
		// ⚠ 挂在**窗口**的 onKeyDown 上。TextBox 自己也订阅了同一个事件，
		//   它会先把回车当换行插进文本 —— 但解析时会忽略非数字字符（L'\n'），
		//   所以那条换行没有任何副作用，不用去拦。
		this->onKeyDown.add([this](UINT key) {
			if (key != VK_RETURN) return;
			for (auto* b : numericBoxes) {
				if (b && b->isFocused()) { b->blur(); break; }
			}
			});

		syncFromConfig();
	}

	void SettingsWin::syncFromConfig()
	{
		for (auto& f : refreshers) f();
	}

	void SettingsWin::saveConfig()
	{
		if (Config::get()->save()) log(L"[settings] 配置已保存");
		else                        log(L"[settings] 配置保存失败");
	}

	void SettingsWin::open()
	{
		if (!hwnd) createOnce();

		// 摆在主屏中央
		const RECT work = monitors::primaryRect();
		const float d = (dpi > 0.f) ? dpi : 1.f;
		const int w2 = static_cast<int>(std::lround(kWinW * d));
		const int h2 = static_cast<int>(std::lround(kWinH * d));
		const int x = work.left + ((work.right - work.left) - w2) / 2;
		const int y = work.top + ((work.bottom - work.top) - h2) / 2;
		setPosition(x, y);

		syncFromConfig();
		if (scroller) scroller->scrollTo(0.f);
		show();
		if (hwnd) SetForegroundWindow(hwnd);   // 用户主动打开的，给焦点是对的
		refresh();
		opened = true;
		log(L"[settings] 打开设置窗口");
	}

	void SettingsWin::close()
	{
		if (!opened && !hwnd) return;
		saveConfig();
		if (hwnd) hide();
		opened = false;
		log(L"[settings] 关闭设置窗口");
	}

	LRESULT SettingsWin::onHitTest(const POINT pos)
	{
		// 标题条整块当作"系统标题栏" → 系统会接管拖动。
		// ⚠ 关闭按钮在标题条右端，它必须是 HTCLIENT 才能被点到，
		//   所以先把那一小块排除掉。
		POINT pt = pos;
		ScreenToClient(hwnd, &pt);
		const float d = (dpi > 0.f) ? dpi : 1.f;
		const float titleH = kTitleH * d;
		const float btnLeft = (kWinW - 56.f) * d;
		if (pt.y >= 0 && pt.y < static_cast<LONG>(titleH)
			&& pt.x >= 0 && pt.x < static_cast<LONG>(btnLeft)) {
			return HTCAPTION;
		}
		return HTCLIENT;
	}

} // namespace zdock
