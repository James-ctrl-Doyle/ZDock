#include "AutoStart.h"
#include "Log.h"

#include <Windows.h>

#include <format>
#include <vector>

namespace zdock::autostart {

	std::wstring exePath()
	{
		// ⚠ MAX_PATH 不够：长路径下 GetModuleFileNameW 会截断并返回 MAX_PATH，
		//   拿到的路径是错的（末尾缺一截），写进注册表就再也启动不起来。
		//   先按 MAX_PATH 试，返回长度顶到上限就翻倍重试。
		std::vector<wchar_t> buf(MAX_PATH);
		for (;;) {
			const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (n == 0) return {};
			if (n < buf.size() - 1) return std::wstring{ buf.data(), n };
			buf.resize(buf.size() * 2);
			if (buf.size() > 32768) return {};   // 到 NT 的上限了，放弃
		}
	}

	std::wstring expectedValue()
	{
		const std::wstring exe = exePath();
		if (exe.empty()) return {};
		// 一律加引号：带空格时必须，不带空格时也无害 ——
		// 统一加省得判断，也省得以后改路径时忘掉。
		return L"\"" + exe + L"\"";
	}

	std::wstring currentValue()
	{
		HKEY key{};
		if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
			return {};
		}
		DWORD type = 0;
		DWORD bytes = 0;
		LSTATUS st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
		if (st != ERROR_SUCCESS || type != REG_SZ || bytes == 0) {
			RegCloseKey(key);
			return {};
		}
		std::wstring out(bytes / sizeof(wchar_t), L'\0');
		st = RegQueryValueExW(key, kValueName, nullptr, &type,
			reinterpret_cast<LPBYTE>(out.data()), &bytes);
		RegCloseKey(key);
		if (st != ERROR_SUCCESS) return {};
		// 去掉结尾的 \0（RegQueryValueEx 把长度算进去了）
		while (!out.empty() && out.back() == L'\0') out.pop_back();
		return out;
	}

	bool isEnabled()
	{
		return !currentValue().empty();
	}

	bool set(bool on)
	{
		HKEY key{};
		// ⚠ 要 KEY_SET_VALUE；用 KEY_WRITE 也行，但别只给 KEY_READ。
		const LSTATUS open = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
		if (open != ERROR_SUCCESS) {
			log(std::format(L"[autostart] 打不开 Run 项 err={}", open));
			return false;
		}

		bool ok = false;
		if (on) {
			const std::wstring value = expectedValue();
			if (value.empty()) {
				log(L"[autostart] 拿不到 exe 路径，无法注册自启");
				RegCloseKey(key);
				return false;
			}
			const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
			const LSTATUS st = RegSetValueExW(key, kValueName, 0, REG_SZ,
				reinterpret_cast<const BYTE*>(value.c_str()), bytes);
			ok = (st == ERROR_SUCCESS);
			if (ok) log(std::format(L"[autostart] 已开启：{} = {}", kValueName, value));
			else    log(std::format(L"[autostart] 写入失败 err={}", st));
		}
		else {
			const LSTATUS st = RegDeleteValueW(key, kValueName);
			// 值本来就不在（ERROR_FILE_NOT_FOUND）也算成功 —— 目标状态已达成
			ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
			if (ok) log(L"[autostart] 已关闭（删掉 Run 项里的值）");
			else    log(std::format(L"[autostart] 删除失败 err={}", st));
		}
		RegCloseKey(key);
		return ok;
	}

	bool sync(bool want)
	{
		const std::wstring cur = currentValue();
		if (want) {
			// 已经是我们要的值 → 什么都不做（避免每次启动都写注册表）
			if (cur == expectedValue() && !cur.empty()) return true;
			return set(true);
		}
		if (cur.empty()) return true;   // 本来就没开
		return set(false);
	}

} // namespace zdock::autostart
