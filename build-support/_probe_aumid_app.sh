#!/usr/bin/env bash
# 编 _probe_aumid_app.cpp（纯 Win32，不依赖 Ling）—— 自带 AUMID 的窗口探针。
# 用途见该 .cpp 顶部注释：复现「固定之后出现两个图标」。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build-support/_msvc_env.sh"

OUT="$ROOT/build/_probe_aumid"
mkdir -p "$OUT"

"$CL" -nologo -c -std:c++20 -EHsc -utf-8 -permissive- -O2 -MT -DNDEBUG \
	-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-Fo"$(winpath "$OUT/aumid.obj")" "$(winpath "$ROOT/build-support/_probe_aumid_app.cpp")"

"$LINK" -nologo -SUBSYSTEM:CONSOLE -MACHINE:X64 -INCREMENTAL:NO \
	-OUT:"$(winpath "$OUT/AumidProbeApp.exe")" \
	"$(winpath "$OUT/aumid.obj")" \
	shell32.lib propsys.lib ole32.lib user32.lib gdi32.lib kernel32.lib

echo "==> $(winpath "$OUT/AumidProbeApp.exe")"
