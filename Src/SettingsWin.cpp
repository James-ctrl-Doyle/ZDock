#include "SettingsWin.h"
#include "AutoStart.h"
#include "Config.h"
#include "Log.h"
#include "MonitorUtil.h"

#include <include/Button.h>
#include <include/Label.h>
#include <include/Slider.h>
#include <include/TextBox.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace zdock {

	using Ling::Button;
	using Ling::Color;
	using Ling::Label;
	using Ling::Slider;

	namespace {
		// 一套跟 dock 面板同调性的配色（面板是 #1A1A1A @80%，强调色 #4CC2FF）
		constexpr uint32_t kBg = 0x1C1C1CFF;
		constexpr uint32_t kText = 0xE8E8E8FF;
		constexpr uint32_t kTextDim = 0x9A9A9AFF;
		constexpr uint32_t kAccent = 0x4CC2FFFF;
		constexpr uint32_t kBtnBg = 0x2E2E2EFF;
		constexpr uint32_t kBtnHover = 0x3C3C3CFF;
		constexpr uint32_t kBtnOn = 0x1F5C7AFF;      // 选中态（偏青的深色）
		constexpr uint32_t kTrack = 0x3A3A3AFF;
	} // namespace

	SettingsWin::~SettingsWin() = default;

	// ---------------------------------------------------------------------------
	// 建窗口
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
	}

	void SettingsWin::createOnce()
	{
		if (hwnd) return;
		setSize(kWinW, kWinH);
		setPosition(0, 0);
		// ⚠ 用 WS_POPUP 而不是带标题栏的样式：这个程序整体自绘，塞个系统标题栏观感会裂。
		//   拖动靠 onHitTest 在标题条区域返回 HTCAPTION（系统会把拖动接过去）。
		//   ⚠ 这里**不加** WS_EX_NOACTIVATE —— 用户是主动打开设置窗口的，
		//     它需要能收键盘焦点（以后加输入框时会用到）。
		createNativeWindow(WS_EX_TOOLWINDOW, WS_POPUP);
		if (hwnd) SetWindowTextW(hwnd, L"ZDockSettings");
		buildUiOnce();
		layout();
		refresh();
	}

	// ---------------------------------------------------------------------------
	// 行构造器
	// ---------------------------------------------------------------------------
	float SettingsWin::section(float y, const wchar_t* title)
	{
		auto* lab = body->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 12.f);
		lab->setText(title);
		lab->setFontSize(15.f);
		lab->setColor(Color(kAccent));
		return y + kSectionH;
	}

	float SettingsWin::sliderRow(float y, const wchar_t* label,
		float min, float max, float step,
		std::function<float()> getter, std::function<void(float)> setter)
	{
		auto* lab = body->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 7.f);
		lab->setText(label);
		lab->setFontSize(13.f);
		lab->setColor(Color(kText));

		auto* sl = body->makeChild<Slider>();
		sl->setPositionType(Ling::Position::Absolute);
		sl->setPosition(Ling::Edge::Left, kPadX + kLabelW);
		sl->setPosition(Ling::Edge::Top, y + 9.f);
		sl->setSize(kSliderW, 20.f);
		sl->setRange(min, max);
		sl->setStep(step);
		sl->setValue(getter());
		sl->setTrackColor(Color(kTrack));
		sl->setFillColor(Color(kAccent));
		sl->setThumbColor(Color(kText));
		sl->setHoverThumbColor(Color(kAccent));

		// ⚠ 精度跟着步长走：step=1 的项显示整数，step=0.05/0.01 的项必须显示小数 ——
		//   统一用 {:.0f} 的话"悬停放大 1.70"会显示成"2"、"不透明度 0.80"会显示成"1"，
		//   看起来像值被吸附到了整数（截图里就是这样，第一版把两个小数项都显示成了整数）。
		const bool fractional = (step < 0.95f);
		auto fmt = [fractional](float v) {
			return fractional ? std::format(L"{:.2f}", v) : std::format(L"{:.0f}", v);
			};

		// ---- 数值输入框 ----
		// 用可输入的 TextBox 而不是只读标签：拖滑块只能拖个大概，
		// 想精确改成某个值（比如正好 64）得手输。两者**双向联动**。
		auto* box = body->makeChild<Ling::TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		box->setPosition(Ling::Edge::Left, kPadX + kLabelW + kSliderW + 12.f);
		box->setPosition(Ling::Edge::Top, y + 5.f);
		box->setSize(64.f, 26.f);
		box->setFontSize(13.f);
		box->setVerticalCenter(true);
		box->setPaddingLeft(7.f);
		box->setPaddingRight(4.f);
		box->setBg(Color(kTrack));
		box->setBorderRadius(5.f);
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
		// 按回车也算确认 —— WinBase::onKeyDown 里拦一下让它失焦（见 buildUi 末尾）。
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
			// 夹紧到滑块的范围：手输 999 不该把面板撑爆
			v = std::clamp(v, min, max);
			setter(v);
			box->setText(fmt(v));       // 规范化回写（顺便清掉非法字符）
			sl->setValue(v);            // ⚠ 值没变的话 Slider 不会再触发 onValueChanged
			if (onLiveChanged) onLiveChanged();
			});

		// 记住"刷新"动作，供 syncFromConfig 用
		refreshers.push_back([sl, box, getter, fmt] {
			const float v = getter();
			sl->setValue(v);
			box->setText(fmt(v));
			});
		return y + kRowH;
	}

	float SettingsWin::toggleRow(float y, const wchar_t* label,
		std::function<bool()> getter, std::function<void(bool)> setter)
	{
		auto* lab = body->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 7.f);
		lab->setText(label);
		lab->setFontSize(13.f);
		lab->setColor(Color(kText));

		auto* b = body->makeChild<Button>();
		b->setPositionType(Ling::Position::Absolute);
		b->setPosition(Ling::Edge::Left, kPadX + kLabelW);
		b->setPosition(Ling::Edge::Top, y + 3.f);
		b->setSize(72.f, 26.f);
		b->setFontSize(13.f);
		b->setBorderRadius(6.f);
		b->setHoverBg(Color(kBtnHover));
		b->setHoverColor(Color(0xFFFFFFFF));
		b->setText(getter() ? L"开" : L"关");
		b->setBg(Color(getter() ? kBtnOn : kBtnBg));
		b->setColor(Color(kText));

		b->onClick.add([this, b, getter, setter](Button*) {
			const bool next = !getter();
			setter(next);
			b->setText(next ? L"开" : L"关");
			b->setBg(Color(next ? kBtnOn : kBtnBg));
			if (onLiveChanged) onLiveChanged();
			});
		refreshers.push_back([b, getter] {
			const bool v = getter();
			b->setText(v ? L"开" : L"关");
			b->setBg(Color(v ? kBtnOn : kBtnBg));
			});
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

		auto* lab = body->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 7.f);
		lab->setText(label);
		lab->setFontSize(13.f);
		lab->setColor(Color(kText));

		float x = kPadX + kLabelW;
		int idx = 0;
		std::vector<Button*> btns;
		for (const auto& [text, isOn] : onVec) {
			auto* b = body->makeChild<Button>();
			b->setPositionType(Ling::Position::Absolute);
			b->setPosition(Ling::Edge::Left, x);
			b->setPosition(Ling::Edge::Top, y + 3.f);
			b->setSize(kBtnW, 26.f);
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
				});
			x += kBtnW + 8.f;
		}
		refreshers.push_back([btns, onVec] {
			for (size_t i = 0; i < btns.size() && i < onVec.size(); ++i) {
				btns[i]->setBg(Color(onVec[i].second() ? kBtnOn : kBtnBg));
			}
			});
		return y + kRowH;
	}

	float SettingsWin::noteRow(float y, const wchar_t* text)
	{
		auto* lab = body->makeChild<Label>();
		lab->setPositionType(Ling::Position::Absolute);
		lab->setPosition(Ling::Edge::Left, kPadX);
		lab->setPosition(Ling::Edge::Top, y + 6.f);
		lab->setText(text);
		lab->setFontSize(12.f);
		lab->setColor(Color(kTextDim));
		return y + 26.f;
	}

	// ---------------------------------------------------------------------------
	// 整页布局
	// ---------------------------------------------------------------------------
	void SettingsWin::buildUi()
	{
		auto* cfg = Config::get();

		// 标题条
		auto* title = body->makeChild<Label>();
		title->setPositionType(Ling::Position::Absolute);
		title->setPosition(Ling::Edge::Left, kPadX);
		title->setPosition(Ling::Edge::Top, 14.f);
		title->setText(L"ZDock 设置");
		title->setFontSize(17.f);
		title->setColor(Color(kText));

		// ⚠ 变量别叫 close —— 会把 SettingsWin::close() 遮住（lambda 里 close() 会
		//   被解析成"调用这个名字的变量"，报 C3493/C2326）
		auto* btnClose = body->makeChild<Button>();
		btnClose->setPositionType(Ling::Position::Absolute);
		btnClose->setPosition(Ling::Edge::Left, kWinW - kPadX - 34.f);
		btnClose->setPosition(Ling::Edge::Top, 12.f);
		btnClose->setSize(34.f, 28.f);
		btnClose->setFontSize(14.f);
		btnClose->setBorderRadius(6.f);
		btnClose->setText(L"×");
		btnClose->setBg(Color(kBtnBg));
		btnClose->setColor(Color(kText));
		btnClose->setHoverBg(Color(0x8B2E2EFF));
		btnClose->setHoverColor(Color(0xFFFFFFFF));
		btnClose->onClick.add([this](Button*) { this->close(); });

		float y = kTitleH + kPadY;

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
			[cfg, this](int i) {
				cfg->dockEdge = (i == 1) ? L"top" : (i == 2) ? L"left" : (i == 3) ? L"right" : L"bottom";
			});
		y = choiceRow(y, L"对齐",
			{ {L"起始", [cfg] { return cfg->dockAlign == L"start"; }},
			  {L"居中", [cfg] { return cfg->dockAlign == L"center"; }},
			  {L"末端", [cfg] { return cfg->dockAlign == L"end"; }} },
			[cfg](int i) {
				cfg->dockAlign = (i == 1) ? L"center" : (i == 2) ? L"end" : L"start";
			});

		// 沿边偏移：用滑块（-400 ~ 400）
		y = sliderRow(y, L"沿边偏移", -400.f, 400.f, 1.f,
			[cfg] { return cfg->dockOffset; },
			[cfg](float v) { cfg->dockOffset = v; });

		// 显示器：把枚举到的显示器列成按钮
		{
			const auto mons = monitors::enumerate();
			const int n = static_cast<int>(mons.size());
			if (n <= 1) {
				y = noteRow(y, L"显示器：只有一台（接多屏后这里会列出可选项）");
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

		// ---- 底部：恢复默认 ----
		auto* reset = body->makeChild<Button>();
		reset->setPositionType(Ling::Position::Absolute);
		reset->setPosition(Ling::Edge::Left, kPadX);
		reset->setPosition(Ling::Edge::Top, kWinH - 46.f);
		reset->setSize(110.f, 30.f);
		reset->setFontSize(13.f);
		reset->setBorderRadius(6.f);
		reset->setText(L"恢复默认外观");
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

		// 摆在主屏中央偏右一点，别压住 dock
		const RECT work = monitors::primaryRect();
		const float d = (dpi > 0.f) ? dpi : 1.f;
		const int w2 = static_cast<int>(std::lround(kWinW * d));
		const int h2 = static_cast<int>(std::lround(kWinH * d));
		const int x = work.left + ((work.right - work.left) - w2) / 2;
		const int y = work.top + ((work.bottom - work.top) - h2) / 2;
		setPosition(x, y);

		syncFromConfig();
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
		const float btnLeft = (kWinW - kPadX - 44.f) * d;
		if (pt.y >= 0 && pt.y < static_cast<LONG>(titleH)
			&& pt.x >= 0 && pt.x < static_cast<LONG>(btnLeft)) {
			return HTCAPTION;
		}
		return HTCLIENT;
	}

} // namespace zdock
