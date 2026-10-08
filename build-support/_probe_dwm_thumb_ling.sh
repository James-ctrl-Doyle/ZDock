#!/usr/bin/env bash
# 编 _probe_dwm_thumb_ling.cpp（用真 Ling 窗口验证 DWM 缩略图宿主）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build-support/_msvc_env.sh"

. "$ROOT/build-support/_ling_pkg.sh"
LING_ROOT="$(ling_resolve_root "$ROOT")"
[ -d "$LING_ROOT/include" ] || { echo "找不到 $LING_ROOT" >&2; exit 1; }

OUT="$ROOT/build/_probe_dwm_ling"
mkdir -p "$OUT"

"$CL" -nologo -c -std:c++20 -EHsc -utf-8 -permissive- -O2 -MT -DNDEBUG \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-I"$LING_ROOT" \
	-Fo"$(winpath "$OUT/probe.obj")" "$(winpath "$ROOT/build-support/_probe_dwm_thumb_ling.cpp")"

"$LINK" -nologo -SUBSYSTEM:CONSOLE -MACHINE:X64 -INCREMENTAL:NO \
	-OUT:"$(winpath "$OUT/DwmThumbLingProbe.exe")" \
	-LIBPATH:"$(winpath "$LING_ROOT/x64/Release")" \
	"$(winpath "$OUT/probe.obj")" \
	Ling.lib yoga.lib \
	kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib \
	uuid.lib comdlg32.lib winspool.lib \
	dwmapi.lib windowsapp.lib comctl32.lib imm32.lib version.lib \
	windowscodecs.lib shlwapi.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib \
	ntdll.lib Userenv.lib

echo "==> $(winpath "$OUT/DwmThumbLingProbe.exe")"
