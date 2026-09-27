#include "Config.h"
#include "Log.h"

#include <Windows.h>
#include <winrt/Windows.Data.Json.h>
// ⚠ 必须一起引 Foundation.Collections：JsonObject 的 HasKey / JsonArray 的
//   Size / GetAt / Append 都是 IVector / IMap 上的 **auto 返回**函数，
//   只给 Windows.Data.Json.h 的话，定义点看不到 → C3779「要使用将会返回 auto
//   的函数，必须首先定义此函数」。ZPin 的 pch.h 也是两条一起引。
#include <winrt/Windows.Foundation.Collections.h>

#include <format>
#include <fstream>
#include <sstream>

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

namespace zdock {

	namespace {

		/// exe 同目录的某个文件名。
		std::filesystem::path exeDirFile(const wchar_t* name)
		{
			wchar_t buf[MAX_PATH * 2]{};
			GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
			std::filesystem::path p{ buf };
			return p.parent_path() / name;
		}

		/// UTF-8 文件整体读入成 std::string。读不到返回空串。
		std::string readAll(const std::filesystem::path& p)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f) return {};
			std::ostringstream ss;
			ss << f.rdbuf();
			return ss.str();
		}

		/// std::string(UTF-8) -> std::wstring
		std::wstring toWide(const std::string& s)
		{
			if (s.empty()) return {};
			const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			if (need <= 0) return {};
			std::wstring out(static_cast<size_t>(need), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
			return out;
		}

		/// std::wstring -> std::string(UTF-8)
		std::string toUtf8(const std::wstring& s)
		{
			if (s.empty()) return {};
			const int need = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
			if (need <= 0) return {};
			std::string out(static_cast<size_t>(need), '\0');
			WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need, nullptr, nullptr);
			return out;
		}

		/// "#RRGGBBAA" / "#RRGGBB" -> **0xRRGGBBAA**（与 Ling::Color(uint32_t) 一致）。
		/// 解析失败返回 nullopt 让调用方兜底。
		std::optional<uint32_t> parseColorImpl(const std::wstring& s)
		{
			if (s.size() < 7 || s[0] != L'#') return std::nullopt;
			auto hex = [](wchar_t c) -> int {
				if (c >= L'0' && c <= L'9') return c - L'0';
				if (c >= L'a' && c <= L'f') return c - L'a' + 10;
				if (c >= L'A' && c <= L'F') return c - L'A' + 10;
				return -1;
				};
			uint32_t v = 0;
			for (size_t i = 1; i < s.size(); ++i) {
				const int d = hex(s[i]);
				if (d < 0) return std::nullopt;
				v = (v << 4) | static_cast<uint32_t>(d);
			}
			// ⚠ alpha 在**低**字节（0xRRGGBBAA）—— 写成 `| 0xFF000000` 会把 alpha
			//   塞进 R 的位置，得到"红色且几乎全透明"。这个字节序跟
			//   `Ling::Color(uint32_t)` 保持一致，改一处必须改两处。
			if (s.size() == 7)  return v | 0x000000FFu;   // 没给 alpha 就当作不透明
			if (s.size() == 9)  return v;
			return std::nullopt;
		}

	} // namespace

	std::optional<uint32_t> Config::parseColor(const std::wstring& s)
	{
		return parseColorImpl(s);
	}

	uint32_t Config::bgColorValue() const
	{
		if (const auto v = parseColorImpl(bgColor)) return *v;
		return 0x1A1A1ACC;   // 内置默认：深灰 @ 80% 不透明
	}

	Config* Config::get()
	{
		static Config inst;
		return &inst;
	}

	std::vector<ItemConfig> Config::defaultItems()
	{
		wchar_t winDir[MAX_PATH]{};
		if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return {};
		const std::wstring root{ winDir };

		// 顺序即显示顺序。取存在的前 6 个。
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

		std::vector<ItemConfig> out;
		for (const auto& path : candidates) {
			if (out.size() >= 6) break;
			if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
			out.push_back(ItemConfig{ path, {} });
		}
		return out;
	}

	void Config::load()
	{
		configPath = exeDirFile(L"config.json");

		if (!std::filesystem::exists(configPath)) {
			firstRun = true;
			items = defaultItems();
			log(std::format(L"[config] 未找到 {}，用默认值并写出", configPath.wstring()));
			save();
			return;
		}

		const std::string raw = readAll(configPath);
		if (raw.empty()) {
			log(L"[config] 配置文件为空，用默认值（不覆盖用户文件）");
			items = defaultItems();
			return;
		}

		JsonObject obj{ nullptr };
		if (!JsonObject::TryParse(toWide(raw), obj)) {
			// ⚠ 不覆盖：用户手写错一个字符就把整份配置抹掉，代价太大
			log(L"[config] JSON 解析失败，用默认值（**不动**用户文件，请自行检查 config.json）");
			items = defaultItems();
			return;
		}

		// 每个字段单独取值 + 兜底：缺字段用默认，坏字段不拖垮整份配置
		iconSize = static_cast<float>(obj.GetNamedNumber(L"iconSize", iconSize));
		iconGap = static_cast<float>(obj.GetNamedNumber(L"iconGap", iconGap));
		hoverScale = static_cast<float>(obj.GetNamedNumber(L"hoverScale", hoverScale));
		animMs = static_cast<int>(obj.GetNamedNumber(L"animMs", animMs));
		bottomMargin = static_cast<float>(obj.GetNamedNumber(L"bottomMargin", bottomMargin));
		cornerRadius = static_cast<float>(obj.GetNamedNumber(L"cornerRadius", cornerRadius));

		// 布尔字段：GetNamedBoolean 在 JSON 里是数字/字符串时会抛，所以先看类型。
		// 任务书 §9.6 要求这些开关给用户"看得懂"的兜底，不要因为写了个 "yes" 整份配置作废。
		auto readBool = [&](const wchar_t* name, bool cur) -> bool {
			if (!obj.HasKey(name)) return cur;
			try {
				const auto v = obj.GetNamedValue(name);
				if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean) {
					return obj.GetNamedBoolean(name, cur);
				}
				if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number) {
					return obj.GetNamedNumber(name, cur ? 1.0 : 0.0) != 0.0;
				}
				log(std::format(L"[config] {} 不是布尔值，用默认 {}", name, cur ? L"true" : L"false"));
			}
			catch (const winrt::hresult_error&) {
				log(std::format(L"[config] {} 读取失败，用默认 {}", name, cur ? L"true" : L"false"));
			}
			return cur;
			};
		autoHide = readBool(L"autoHide", autoHide);
		hideOnFullscreen = readBool(L"hideOnFullscreen", hideOnFullscreen);
		reserveWorkArea = readBool(L"reserveWorkArea", reserveWorkArea);

		autoHideDelayMs = static_cast<int>(obj.GetNamedNumber(L"autoHideDelayMs", autoHideDelayMs));
		slideInMs = static_cast<int>(obj.GetNamedNumber(L"slideInMs", slideInMs));
		slideOutMs = static_cast<int>(obj.GetNamedNumber(L"slideOutMs", slideOutMs));

		if (obj.HasKey(L"bgColor")) {
			const auto s = std::wstring{ obj.GetNamedString(L"bgColor", L"") };
			if (!s.empty()) {
				if (parseColorImpl(s)) bgColor = s;
				else log(std::format(L"[config] bgColor \"{}\" 解析不了（要 #RRGGBB 或 #RRGGBBAA），用默认", s));
			}
		}

		// 阶段六：位置 / 外观 / 自启
		auto readEnum = [&](const wchar_t* name, std::wstring& cur,
			std::initializer_list<const wchar_t*> allowed) {
			if (!obj.HasKey(name)) return;
			const auto s = std::wstring{ obj.GetNamedString(name, L"") };
			if (s.empty()) return;
			for (const wchar_t* a : allowed) {
				if (s == a) { cur = s; return; }
			}
			// 认不出来的值：记一行日志、保留当前（默认）值，不拖垮整份配置
			std::wstring list;
			for (const wchar_t* a : allowed) { if (!list.empty()) list += L" / "; list += a; }
			log(std::format(L"[config] {} = \"{}\" 不是 {} 之一，用默认 \"{}\"", name, s, list, cur));
			};
		readEnum(L"dockEdge", dockEdge, { L"bottom", L"top", L"left", L"right" });
		readEnum(L"dockAlign", dockAlign, { L"start", L"center", L"end" });

		dockOffset = static_cast<float>(obj.GetNamedNumber(L"dockOffset", dockOffset));
		opacity = static_cast<float>(obj.GetNamedNumber(L"opacity", opacity));
		showIndicator = readBool(L"showIndicator", showIndicator);
		autoStart = readBool(L"autoStart", autoStart);
		monitorIndex = static_cast<int>(obj.GetNamedNumber(L"monitorIndex", monitorIndex));

		items.clear();
		if (obj.HasKey(L"items")) {
			try {
				const JsonArray arr = obj.GetNamedArray(L"items");
				for (uint32_t i = 0; i < arr.Size(); ++i) {
					// ⚠ JsonValue 没有默认构造函数，必须从 arr.GetAt(i) 取
					const winrt::Windows::Data::Json::IJsonValue v = arr.GetAt(i);
					if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
					const JsonObject item = v.GetObject();
					ItemConfig ic;
					ic.path = std::wstring{ item.GetNamedString(L"path", L"") };
					ic.name = std::wstring{ item.GetNamedString(L"name", L"") };
					if (ic.path.empty()) continue;   // 没路径的条目直接跳过，不给后面添麻烦
					items.push_back(std::move(ic));
				}
			}
			catch (const winrt::hresult_error& e) {
				log(std::format(L"[config] items 段解析失败 hr=0x{:08X}，整段用默认表", (unsigned)e.code()));
				items = defaultItems();
			}
		}
		else {
			items = defaultItems();
		}

		// 配置里 items 是空数组 = 用户故意清空了 Dock，**不要**再塞默认值
		if (items.empty() && !obj.HasKey(L"items")) items = defaultItems();

		// 合法性闸门：数值不合理会让布局算出负数，宁可回默认。
		// 传的是"字段引用 + 默认值"，超出范围就写回默认值（而不是简单 clamp ——
		// 用户填了个 9999 明显是笔误，钳到边界值反而会得到一个他没想要又不易察觉的结果）。
		auto gate = [&](float& v, float lo, float hi, float fallback, const wchar_t* name) {
			if (v < lo || v > hi) {
				log(std::format(L"[config] {} = {} 超出 [{}, {}]，回默认 {}", name, v, lo, hi, fallback));
				v = fallback;
			}
			};
		gate(iconSize, 16.f, 256.f, 48.f, L"iconSize");
		gate(iconGap, 0.f, 128.f, 12.f, L"iconGap");
		gate(hoverScale, 1.f, 4.f, 1.7f, L"hoverScale");
		gate(bottomMargin, 0.f, 400.f, 6.f, L"bottomMargin");
		gate(cornerRadius, 0.f, 128.f, 12.f, L"cornerRadius");
		// 阶段六
		gate(dockOffset, -10000.f, 10000.f, 0.f, L"dockOffset");
		gate(opacity, 0.5f, 0.95f, 0.8f, L"opacity");
		if (monitorIndex < -1 || monitorIndex > 15) {
			log(std::format(L"[config] monitorIndex = {} 超出 [-1, 15]，回默认 -1（跟随）", monitorIndex));
			monitorIndex = -1;
		}
		if (animMs < 0 || animMs > 2000) {
			log(std::format(L"[config] animMs = {} 超出 [0, 2000]，回默认 150", animMs));
			animMs = 150;
		}
		// 自动隐藏的三个时长：0 是合法的（=不做动画，直接跳到位），上限 3000ms
		// 已经够慢；再大就是笔误了。
		if (autoHideDelayMs < 0 || autoHideDelayMs > 10000) {
			log(std::format(L"[config] autoHideDelayMs = {} 超出 [0, 10000]，回默认 500", autoHideDelayMs));
			autoHideDelayMs = 500;
		}
		if (slideInMs < 0 || slideInMs > 3000) {
			log(std::format(L"[config] slideInMs = {} 超出 [0, 3000]，回默认 200", slideInMs));
			slideInMs = 200;
		}
		if (slideOutMs < 0 || slideOutMs > 3000) {
			log(std::format(L"[config] slideOutMs = {} 超出 [0, 3000]，回默认 300", slideOutMs));
			slideOutMs = 300;
		}

		log(std::format(L"[config] 载入 ok：icon={:.0f} gap={:.0f} hover={:.2f} anim={}ms bottom={:.0f} 项数={} autoHide={} reserve={} hideFull={} edge={} align={} offset={:.0f} opacity={:.2f} monitor={}",
			iconSize, iconGap, hoverScale, animMs, bottomMargin, items.size(),
			autoHide ? 1 : 0, reserveWorkArea ? 1 : 0, hideOnFullscreen ? 1 : 0,
			dockEdge, dockAlign, dockOffset, opacity, monitorIndex));
	}

	std::string Config::toJson() const
	{
		JsonObject root;

		// 带注释不行（标准 JSON 不支持），所以把可调项的名字写清楚、值给成默认值，
		// 用户照着改。字段名全用 camelCase 与 ZPin 的 config.json 保持一致。
		root.SetNamedValue(L"iconSize", JsonValue::CreateNumberValue(iconSize));
		root.SetNamedValue(L"iconGap", JsonValue::CreateNumberValue(iconGap));
		root.SetNamedValue(L"hoverScale", JsonValue::CreateNumberValue(hoverScale));
		root.SetNamedValue(L"animMs", JsonValue::CreateNumberValue(animMs));
		root.SetNamedValue(L"bottomMargin", JsonValue::CreateNumberValue(bottomMargin));
		root.SetNamedValue(L"bgColor", JsonValue::CreateStringValue(bgColor));
		root.SetNamedValue(L"cornerRadius", JsonValue::CreateNumberValue(cornerRadius));

		// 阶段四：自动隐藏 / 工作区预留。默认值都写成"关"，
		// 即：即便用户没碰这几个字段，行为也和阶段三一模一样。
		root.SetNamedValue(L"autoHide", JsonValue::CreateBooleanValue(autoHide));
		root.SetNamedValue(L"autoHideDelayMs", JsonValue::CreateNumberValue(autoHideDelayMs));
		root.SetNamedValue(L"slideInMs", JsonValue::CreateNumberValue(slideInMs));
		root.SetNamedValue(L"slideOutMs", JsonValue::CreateNumberValue(slideOutMs));
		root.SetNamedValue(L"hideOnFullscreen", JsonValue::CreateBooleanValue(hideOnFullscreen));
		root.SetNamedValue(L"reserveWorkArea", JsonValue::CreateBooleanValue(reserveWorkArea));

		// 阶段六：位置 / 外观 / 自启。默认值都对应"和 0.1.5 完全一样的行为"，
		// 所以老用户升级后观感不变。
		root.SetNamedValue(L"dockEdge", JsonValue::CreateStringValue(dockEdge));
		root.SetNamedValue(L"dockAlign", JsonValue::CreateStringValue(dockAlign));
		root.SetNamedValue(L"dockOffset", JsonValue::CreateNumberValue(dockOffset));
		root.SetNamedValue(L"opacity", JsonValue::CreateNumberValue(opacity));
		root.SetNamedValue(L"showIndicator", JsonValue::CreateBooleanValue(showIndicator));
		root.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
		root.SetNamedValue(L"monitorIndex", JsonValue::CreateNumberValue(monitorIndex));

		JsonArray arr;
		for (const auto& it : items) {
			JsonObject o;
			o.SetNamedValue(L"path", JsonValue::CreateStringValue(it.path));
			if (!it.name.empty()) o.SetNamedValue(L"name", JsonValue::CreateStringValue(it.name));
			arr.Append(o);
		}
		root.SetNamedValue(L"items", arr);

		// 缩进 2 空格，方便用户直接手改
		std::string s = toUtf8(std::wstring{ root.Stringify() });
		return s;
	}

	bool Config::save()
	{
		if (configPath.empty()) configPath = exeDirFile(L"config.json");

		const std::string json = toJson();
		const auto tmp = std::filesystem::path{ configPath }.concat(L".tmp");

		{
			std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
			if (!f) {
				log(std::format(L"[config] 打不开临时文件 {}", tmp.wstring()));
				return false;
			}
			f.write(json.data(), static_cast<std::streamsize>(json.size()));
			f.flush();
			if (!f) {
				log(L"[config] 写临时文件失败");
				return false;
			}
		}

		// 原子替换：写一半崩掉也不会留下半截 JSON 把配置读废
		if (!MoveFileExW(tmp.c_str(), configPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			log(std::format(L"[config] 替换 config.json 失败 GetLastError={}", GetLastError()));
			std::error_code ec;
			std::filesystem::remove(tmp, ec);
			return false;
		}
		log(std::format(L"[config] 已写出 {}", configPath.wstring()));
		return true;
	}

} // namespace zdock
