#pragma once
#include <Windows.h>

#include <include/WinBase.h>

#include <functional>
#include <string>
#include <vector>

// ⚠ 前向声明要在 namespace zdock **外面** —— 写到里面会变成 zdock::Ling::TextBox，
//   和真正的 Ling::TextBox 不是一个类型。
namespace Ling { class TextBox; class ScrollerBox; }

namespace zdock {

	class DockWin;

	/// <summary>
	/// 设置窗口（任务书 §2 #25）：**自绘**，用 Ling 的 Button / Slider / Label / TextBox 拼。
	///
	/// 版式是一列"**行式列表**"：每行"标签在左、控件在右"，行间一条细分隔线，
	/// 分组之间用一个大一点的标题隔开；整体放在一个可滚动内容区里。
	///
	/// 配色取的是参考图那种**亮蓝强调**（`#597EF7`）—— ⚠ 它跟 dock 面板指示器的
	/// 青色 `#4CC2FF` **不是同一个色**，是设置窗口专用的。
	///
	/// 为什么不用系统控件：这个程序整体是自绘风格（面板、图标、菜单都是），
	/// 塞一列 Win32 原生控件进去观感会裂开 —— 而 Ling 的 Button 带 hover 三色、
	/// Slider 能自定轨道/填充/滑块颜色，拼出来的东西能跟面板同一套调性。
	///
	/// ⚠ 改动是**即时生效**的（拖滑块当场看到 dock 变），所以没有"取消"语义；
	///   关闭窗口时把配置写回 config.json。
	/// </summary>
	class SettingsWin : public Ling::WinBase
	{
	public:
		~SettingsWin();

		/// <summary>建窗口（只建一次，之后靠 show/hide 复用）。</summary>
		void createOnce();

		/// <summary>显示并把控件刷成当前配置的值。</summary>
		void open();

		/// <summary>隐藏 + 把配置写回 config.json。</summary>
		void close();

		bool isOpen() const { return opened; }

		/// <summary>某个设置项改动后的回调 —— 接到 DockWin::applyLiveConfig 上让改动立刻可见。</summary>
		std::function<void()> onLiveChanged;
		/// <summary>日志。</summary>
		std::function<void(const std::wstring&)> onLog;

		void onCreated() override;
		/// <summary>标题条返回 HTCAPTION，这样自绘窗口也能拖着走。</summary>
		LRESULT onHitTest(const POINT pos) override;

	private:
		void log(const std::wstring& s) const { if (onLog) onLog(s); }

		/// <summary>整页布局（建控件 + 挂回调）。</summary>
		void buildUi();
		/// <summary>建一次 UI 并刷成当前配置。</summary>
		void buildUiOnce();

		// ---- 行构造器：每个返回"下一行的 y"，这样可以一行一行往下堆 ----
		/// <summary>分组标题（内容坐标系里的 y）。返回下一行 y。</summary>
		float section(float y, const wchar_t* title);
		/// <summary>一行：标签在左（kLabelW 宽）、控件在右，行底一条分隔线。</summary>
		float sliderRow(float y, const wchar_t* label, float min, float max, float step,
			std::function<float()> getter, std::function<void(float)> setter);
		float toggleRow(float y, const wchar_t* label,
			std::function<bool()> getter, std::function<void(bool)> setter);
		float choiceRow(float y, const wchar_t* label,
			std::initializer_list<std::pair<const wchar_t*, std::function<bool()>>> opts,
			std::function<void(int)> picker);
		float noteRow(float y, const wchar_t* text);

		/// <summary>行底那条细分隔线。</summary>
		void divider(float y);
		/// <summary>行标签（控件区左边的那个）。</summary>
		void rowLabel(float y, const wchar_t* text);

		/// <summary>按当前配置刷新所有控件（滑块位置、按钮文字与高亮）。</summary>
		void syncFromConfig();
		void saveConfig();

		/// 拖动滑块时的中间值（滑块只认 float，配置里有 int 字段）
		std::vector<std::function<void()>> refreshers;

		/// 所有数值输入框 —— 回车时要把"当前聚焦的那个"失焦（失焦才触发解析应用）
		std::vector<Ling::TextBox*> numericBoxes;

		/// 内容滚动区（所有设置行都堆在它的 content 里）
		Ling::ScrollerBox* scroller{ nullptr };
		Ling::Node* content{ nullptr };

		bool opened{ false };

		// ---- 尺寸（逻辑像素）----
		static constexpr float kWinW = 520.f;
		static constexpr float kWinH = 560.f;
		static constexpr float kTitleH = 46.f;     // 顶部标题条
		static constexpr float kPadX = 22.f;       // 内容区左右内边距
		static constexpr float kRowH = 39.f;   // 跟 ZPin 设置页一致（也正是参考图量出来的 49-50 物理）
		static constexpr float kNoteH = 26.f;  // 注解行（紧跟某项下面的灰字，比设置行矮）
		static constexpr float kSectionH = 52.f;
		static constexpr float kLabelW = 176.f;    // 行标签宽（控件从它右边开始）
		static constexpr float kSliderW = 168.f;
		static constexpr float kNumW = 62.f;
		static constexpr float kBtnW = 62.f;
	};

} // namespace zdock
