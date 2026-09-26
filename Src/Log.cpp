#include <Windows.h>

#include "Log.h"

#include <string>

namespace zdock {
	namespace {

		constexpr unsigned long long kMaxLogBytes = 1024ull * 1024ull;

		const std::wstring& logPath()
		{
			static const std::wstring path = [] {
				wchar_t buf[MAX_PATH]{};
				GetModuleFileNameW(nullptr, buf, MAX_PATH);
				std::wstring p{ buf };
				const auto slash = p.find_last_of(L'\\');
				return (slash == std::wstring::npos ? std::wstring{} : p.substr(0, slash + 1)) + L"ZDock.log";
				}();
			return path;
		}

		std::string toUtf8(const std::wstring& s)
		{
			if (s.empty()) return {};
			const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
			if (need <= 0) return {};
			std::string out(static_cast<size_t>(need), '\0');
			WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need, nullptr, nullptr);
			return out;
		}

	} // namespace

	void log(const std::wstring& line)
	{
		const auto& path = logPath();

		// 轮转：只在超限时搬一次，避免每次写盘都查
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)
			&& (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32 | fad.nFileSizeLow) > kMaxLogBytes) {
			MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
		}

		SYSTEMTIME st{};
		GetLocalTime(&st);
		wchar_t stamp[64]{};
		swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

		std::string utf8 = toUtf8(std::wstring{ stamp } + line);
		if (utf8.empty()) return;
		utf8 += "\r\n";

		HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return;
		DWORD written = 0;
		WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
		CloseHandle(file);
	}

	void log(const char* line)
	{
		if (!line) return;
		std::wstring wide;
		for (const char* p = line; *p; ++p) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
		log(wide);
	}

} // namespace zdock
