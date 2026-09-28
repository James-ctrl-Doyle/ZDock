#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>

namespace zdock {

	/// <summary>一个 Dock 项的配置（图标列表里的一项）。</summary>
	struct ItemConfig
	{
		/// <summary>
		/// 可执行文件 / 文件 / 文件夹的路径；
		/// 也支持 **shell 虚拟对象**（`"::{CLSID}"` 或 `"shell:xxx"`），比如回收站
		/// —— 那种没有 exe 路径。⚠ 图标提取与打开都要按 PIDL 走，不能当普通路径处理
		/// （实测 `SHGetFileInfo` 对裸 CLSID 字符串直接返回 0）。
		/// </summary>
		std::wstring path;
		std::wstring name;   // 显示名。空 = 从文件名推
		/// <summary>
		/// 靠右固定：**正在运行的应用**（临时图标）会插在"左侧固定项"和"右侧固定项"之间。
		/// 默认 false（靠左）。这样"资源管理器在最左、回收站在最右、中间是运行中的软件"
		/// 就是一份默认配置能表达出来的东西。
		/// </summary>
		bool pinRight{ false };
	};

	/// <summary>
	/// ZDock 的配置。读写 exe 同目录的 `config.json`。
	///
	/// 设计约定（与 ZPin 一致，便于两边共享心智）：
	///  · **文件不存在**时用内置默认值，并把默认值写出去（用户能看到有哪些可调项）
	///  · **文件存在但解析失败**时用默认值，**不覆盖**用户文件
	///    （覆盖会把用户手写错一个字就整个配置抹掉，代价太大）
	///  · 每个字段单独取值 + 单独兜底：缺字段用默认，坏字段不拖垮整份配置
	///  · 保存走"写临时文件 + 原子替换"，避免写一半断电留下半截 JSON
	/// </summary>
	class Config
	{
	public:
		static Config* get();

		/// <summary>从 exe 同目录的 config.json 载入（不存在则用默认并落盘）。</summary>
		void load();

		/// <summary>把当前值写回 config.json（原子替换）。</summary>
		bool save();

		// ---------------- 可调项 ----------------
		// 说明：默认值 = 阶段一实测下来合用的那套参数，改这里的默认值等于改产品行为。

		/// <summary>图标基准边长（逻辑像素）。</summary>
		float iconSize{ 48.f };
		/// <summary>图标间距（逻辑像素）。</summary>
		float iconGap{ 12.f };
		/// <summary>悬停峰值缩放倍数。</summary>
		float hoverScale{ 1.7f };
		/// <summary>放大/缩回动画时长（毫秒）。</summary>
		int   animMs{ 150 };
		/// <summary>面板距工作区底边（逻辑像素）。任务栏自动隐藏时调大些可避免抢热区。</summary>
		float bottomMargin{ 6.f };
		/// <summary>面板背景色，形如 "#RRGGBBAA"。</summary>
		std::wstring bgColor{ L"#1A1A1ACC" };
		/// <summary>面板圆角（逻辑像素）。</summary>
		float cornerRadius{ 12.f };

		// ---------------- 阶段四：自动隐藏 / 工作区预留 ----------------

		/// <summary>
		/// 自动隐藏：鼠标离开 dock 且无悬停 / 菜单会话 → 滑出屏幕边缘；
		/// 鼠标触到停靠边的热区 → 滑入。默认关。
		/// </summary>
		bool autoHide{ false };
		/// <summary>鼠标离开到开始滑出的延迟（毫秒）。任务书 §5 定 500。</summary>
		int  autoHideDelayMs{ 500 };
		/// <summary>滑入动画时长（毫秒）。任务书 §3 定 200。</summary>
		int  slideInMs{ 200 };
		/// <summary>滑出动画时长（毫秒）。任务书 §3 定 300。</summary>
		int  slideOutMs{ 300 };
		/// <summary>全屏应用在前台时保持隐藏（自动隐藏开启时才有意义）。</summary>
		bool hideOnFullscreen{ true };
		/// <summary>
		/// 工作区预留（AppBar）：向系统申请把工作区从停靠边缩进，普通窗口最大化
		/// 时不会盖住 dock。默认**关** —— 关了 dock 就是一块浮在最上层的窗口。
		/// </summary>
		bool reserveWorkArea{ false };

		/// <summary>Dock 上的图标列表。空 = 用内置默认表。</summary>
		std::vector<ItemConfig> items;

		// ---------------- 阶段六：位置 / 外观 / 自启 ----------------

		/// <summary>
		/// 停靠边：`"bottom"`（默认）/ `"top"` / `"left"` / `"right"`（任务书 §2 #22）。
		///
		/// 用字符串存而不是枚举，是为了让用户手改 config.json 时能看懂；
		/// 认不出来的值会记一行日志并回默认（不拖垮整份配置）。
		/// </summary>
		std::wstring dockEdge{ L"bottom" };
		/// <summary>沿停靠边的对齐：`"start"` / `"center"`（默认）/ `"end"`。</summary>
		std::wstring dockAlign{ L"center" };
		/// <summary>沿停靠边的偏移（逻辑像素，正数朝"末端"方向 —— bottom 时即向右）。</summary>
		float dockOffset{ 0.f };
		/// <summary>
		/// 面板不透明度 0.5~0.95（任务书 §3）。
		/// ⚠ 它**取代** `bgColor` 里那两位 alpha：bgColor 只贡献 RGB。
		///   默认 0.80 与内置 bgColor 的 `#1A1A1ACC` 一致，所以升级不改变观感。
		/// </summary>
		float opacity{ 0.8f };
		/// <summary>是否显示运行指示器（外观项，任务书 §2 #25）。</summary>
		bool showIndicator{ true };
		/// <summary>开机自启（HKCU\...\Run 项，任务书 §2 #26）。</summary>
		bool autoStart{ false };
		/// <summary>
		/// 显示器锚定（任务书 §2 #31）：`-1` = 跟随（主显示器），
		/// `>=0` = 该显示器的序号（按系统枚举顺序，从 0 起）。
		/// </summary>
		int monitorIndex{ -1 };

		/// <summary>
		/// 把 "#RRGGBBAA" / "#RRGGBB" 解析成 **0xRRGGBBAA**（与 `Ling::Color(uint32_t)` 同一字节序）。
		/// 解析不了返回 nullopt（调用方兜底）。放成静态是为了让 DockWin 复用同一套解析，
		/// 不要在别处再写一份 —— 两套解析迟早会漂移。
		/// </summary>
		static std::optional<uint32_t> parseColor(const std::wstring& s);

		/// <summary>把 bgColor 解析成 0xRRGGBBAA，失败回落到内置默认值。</summary>
		uint32_t bgColorValue() const;

		/// <summary>配置文件路径（exe 同目录 config.json）。</summary>
		const std::filesystem::path& path() const { return configPath; }

		/// <summary>本次载入是否发生了"文件不存在，用默认值"（首次启动）。</summary>
		bool firstRun{ false };

	private:
		Config() = default;
		~Config() = default;
		Config(const Config&) = delete;
		Config& operator=(const Config&) = delete;

		/// <summary>内置默认图标表（系统自带的那几个）。</summary>
		static std::vector<ItemConfig> defaultItems();
		/// <summary>把当前值序列化成 JSON 文本。</summary>
		std::string toJson() const;

		std::filesystem::path configPath;
	};

} // namespace zdock
