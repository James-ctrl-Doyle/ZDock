#!/usr/bin/env bash
# 编一个最小工具 LingAppID.exe —— 只打印 Ling::App::appID。
# 用来验证 Ling 包的 appID 行为（v1.3.0 是编译期常量、v1.3.1 起按 exe 路径哈希）。
#
# 用法：
#   bash build-support/_probe_ling_appid.sh                 # 用默认（v1.3.1）包
#   bash build-support/_probe_ling_appid.sh ling-v1.3.0     # 指定包目录名
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build-support/_msvc_env.sh"

PKG="${1:-ling-v1.3.1}"
LING_ROOT="$ROOT/../Ling/dist/$PKG-x64"
[ -d "$LING_ROOT/include" ] || { echo "找不到 $LING_ROOT" >&2; exit 1; }

OUT="$ROOT/build/_probe_appid"
mkdir -p "$OUT"

"$CL" -nologo -c -std:c++20 -EHsc -utf-8 -permissive- -O2 -MT -DNDEBUG \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-I"$LING_ROOT" \
	-Fo"$(winpath "$OUT/appid.obj")" "$(winpath "$ROOT/build-support/_probe_ling_appid.cpp")"

"$LINK" -nologo -SUBSYSTEM:CONSOLE -MACHINE:X64 -INCREMENTAL:NO \
	-OUT:"$(winpath "$OUT/LingAppID.exe")" \
	-LIBPATH:"$(winpath "$LING_ROOT/x64/Release")" \
	"$(winpath "$OUT/appid.obj")" \
	Ling.lib yoga.lib \
	kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib \
	uuid.lib comdlg32.lib winspool.lib \
	dwmapi.lib windowsapp.lib comctl32.lib imm32.lib version.lib \
	windowscodecs.lib shlwapi.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib \
	ntdll.lib Userenv.lib

echo "==> $(winpath "$OUT/LingAppID.exe")  (Ling=$PKG)"
