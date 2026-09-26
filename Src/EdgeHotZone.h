#pragma once
#include <Windows.h>
#include <functional>
#include <string>

namespace zdock {

	/// <summary>
	/// 自动隐藏的"触发展开"热区：一条贴停靠边的**透明细窗**（任务书 §4）。
	///
	/// 设计依据（任务书 §4 / 红线 4 / 红线 5）：
	///  · 热区必须是**窗口**，不能用低级鼠标钩子、也不能靠定时器轮询光标位置
	///    —— 钩子原则上不用（红线 5），轮询直接违反红线 4。
	///  · 尺寸：厚约 3px（物理像素，贴边），宽取 `max(dock 宽, 屏宽/2)`，
	///    居中于停靠边。这样鼠标斜着往屏幕底边划过去也能撞上。
	///  · 它只在"自动隐藏开启"时存在；关掉自动隐藏要立刻销毁，
	///    否则会在屏幕边上留一条看不见却吃点击的窗口。
	///
	/// 为什么是独立窗口而不是给 dock 主窗口加一条"探出边"：
	///  · dock 主窗口在隐藏态是**整体滑到屏幕外**的，它自己碰到不热区；
	///  · 独立窗口可以完全不参与 dock 的 region / 命中区域那套逻辑，
	///    互不干扰。它唯一的职责就是"收到 WM_MOUSEMOVE 就告诉宿主展开"。
	///
	/// ⚠ 它**不抢焦点**（WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW），
	///   也不做绘制（完全透明、无绘制调用）。
	/// </summary>
	class EdgeHotZone
	{
	public:
		EdgeHotZone() = default;
		~EdgeHotZone();

		/// <summary>
		/// 创建（若已存在则先销毁重建）热区窗口。
		/// 参数是**物理像素**的屏幕矩形（调用方已经算好停靠边上的位置）。
		/// </summary>
		bool create(const RECT& screenRect);

		/// <summary>销毁热区窗口（自动隐藏关掉 / 退出时调）。</summary>
		void destroy();

		bool alive() const { return hwnd != nullptr; }

		/// <summary>把热区移到新位置（面板宽度 / 显示器变化时调）。</summary>
		void moveTo(const RECT& screenRect);

		/// <summary>鼠标进入热区时回调（宿主据此滑入 dock）。</summary>
		std::function<void()> onEnter;

		/// <summary>
		/// **仅自动化测试用**的注入通道：收到约定的私有消息时回调，参数 = 全屏状态。
		///
		/// ⚠ 为什么需要它：全屏让位这条链路的真值来自 `GetForegroundWindow()`，
		///   而要让一个窗口真变成前台，测试脚本就必须 `SetForegroundWindow`
		///   —— 那会**抢走用户的输入焦点**（本工作区明确禁止：用户的
		///   WorkBuddy 对话会因此被打断）。所以留一条"注入"通道，让探针用
		///   `PostMessage` 直接驱动宿主，完全不碰焦点。
		///   正常运行时没有任何代码会发这条消息，等于零影响。
		/// </summary>
		std::function<void(bool)> onTestInject;

		/// <summary>日志回调（复用宿主的 Log）。</summary>
		std::function<void(const std::wstring&)> onLog;

		/// <summary>测试注入用的消息号（探针 PostMessage 时用）。</summary>
		static constexpr UINT kMsgTestInject = WM_APP + 100;

	private:
		static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
		void log(const std::wstring& s) const { if (onLog) onLog(s); }

		HWND hwnd{ nullptr };
		/// 已经给这个 hwnd 派发过 onEnter（避免鼠标在热区里移动时反复触发）
		bool entered{ false };
	};

} // namespace zdock
