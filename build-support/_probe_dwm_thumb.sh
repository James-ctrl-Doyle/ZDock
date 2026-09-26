#!/usr/bin/env bash
# 编 _probe_dwm_thumb.cpp（纯 Win32，不依赖 Ling）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build-support/_msvc_env.sh"

OUT="$ROOT/build/_probe_dwm"
mkdir -p "$OUT"

"$CL" -nologo -c -std:c++20 -EHsc -utf-8 -permissive- -O2 -MT -DNDEBUG \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-Fo"$(winpath "$OUT/dwm.obj")" "$(winpath "$ROOT/build-support/_probe_dwm_thumb.cpp")"

"$LINK" -nologo -SUBSYSTEM:CONSOLE -MACHINE:X64 -INCREMENTAL:NO \
	-OUT:"$(winpath "$OUT/DwmThumbProbe.exe")" \
	"$(winpath "$OUT/dwm.obj")" \
	dwmapi.lib user32.lib gdi32.lib kernel32.lib

echo "==> $(winpath "$OUT/DwmThumbProbe.exe")"
