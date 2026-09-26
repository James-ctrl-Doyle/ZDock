#pragma once
#include <string>

namespace zdock {

	/// <summary>
	/// 追加一行到 exe 同目录 ZDock.log（UTF-8 + CRLF，带时间戳）。
	/// 简单轮转：超过 1MB 就整体改名成 ZDock.log.1 重新开始 —— 不做写盘缓冲，
	/// 因为我们只在启动、异常、用户操作这类低频点上写。
	/// </summary>
	void log(const std::wstring& line);

	/// <summary>同上，但接受窄字符串（ASCII 常量的便利重载）。</summary>
	void log(const char* line);

} // namespace zdock
