#pragma once
#include <Windows.h>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <string>

namespace zdock {

	/// <summary>
	/// 从 shell 图标缓存取高清源图标（jumbo 档 256px），按 targetPx 用 WIC 的
	/// 高质量缩放器缩到目标像素，再转成 D2D 位图。
	/// path 可以是 exe / dll / .lnk / 文件夹 / 文档 —— 图标解析规则交给 shell。
	/// 失败返回空指针，调用方自行兜底（不要在这里弹框）。
	/// </summary>
	Microsoft::WRL::ComPtr<ID2D1Bitmap> loadShellIcon(ID2D1DeviceContext* ctx, const std::wstring& path, int targetPx);

	/// <summary>解析 .lnk 的目标路径；不是快捷方式或解析失败返回空串。</summary>
	std::wstring resolveShortcutTarget(const std::wstring& path);

	/// <summary>取显示名（文件名去扩展名；快捷方式取快捷方式自身名字）。</summary>
	std::wstring displayNameOf(const std::wstring& path);

} // namespace zdock
