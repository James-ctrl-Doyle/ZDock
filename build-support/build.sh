#!/usr/bin/env bash
# 编译 ZDock。直接调 cl.exe / link.exe，不走 MSBuild（本机没装 .NET SDK，也不需要）。
#
#   bash build-support/build.sh
#
# Ling 静态库来源（按优先级）：
#   1) $LING_ROOT
#   2) ../Ling/dist/ling-v1.3.0-x64      —— Ling 仓库打出来的发布包（含 include/ + x64/Release）
#   3) ../Ling                            —— Ling 源码树（布局与发布包一致）
#   LING_FROM_SOURCE=1 强制走源码树。
#
# ⚠ 运行时库必须与 Ling 一致 = /MT（静态 CRT）。Ling 与 ZPin 都是 MultiThreaded，
#   这里用 /MD 会在链接期报 LNK2038 运行库不匹配。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build-support/_msvc_env.sh"

if [ "${LING_FROM_SOURCE:-0}" = "1" ]; then
    LING_ROOT="$ROOT/../Ling"
else
    LING_ROOT="${LING_ROOT:-$ROOT/../Ling/dist/ling-v1.3.0-x64}"
    [ -d "$LING_ROOT/include" ] || LING_ROOT="$ROOT/../Ling"
fi
[ -d "$LING_ROOT/include" ] || { echo "找不到 Ling（试过 $LING_ROOT）" >&2; exit 1; }
echo "Ling : $LING_ROOT"

OUT="$ROOT/build"
OBJ="$OUT/obj"
BIN="$OUT/bin"
mkdir -p "$OBJ" "$BIN"

CL_FLAGS=(
    -nologo -c -std:c++20 -EHsc -utf-8 -permissive- -W3
    -O2 -Oi -Gy -MT -DNDEBUG
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
    -I"$ROOT/Src" -I"$LING_ROOT"
)
# ⚠ 只加 Ling 的**根目录**，绝不加它的 include/ 子目录：Windows 文件系统大小写不敏感，
#   一旦 include/ 进了 -I（且排在系统 INCLUDE 之前），SDK 的 <winbase.h> 会被 Ling 的
#   include/WinBase.h 抢走，windows.h 整片基础类型当场缺失（报 rpcasync.h 的 OVERLAPPED
#   未定义，完全看不出真因）。Ling 自己的头一律写成 <include/xxx.h> 来引用。

SRCS=(
    "$ROOT/Src/Log.cpp"
    "$ROOT/Src/SingleInstance.cpp"
    "$ROOT/Src/IconLoader.cpp"
    "$ROOT/Src/IconNode.cpp"
    "$ROOT/Src/DockWin.cpp"
    "$ROOT/Src/main.cpp"
)

OBJS=()
for src in "${SRCS[@]}"; do
    obj="$OBJ/$(basename "${src%.cpp}").obj"
    "$CL" "${CL_FLAGS[@]}" -Fo"$(winpath "$obj")" "$(winpath "$src")"
    OBJS+=("$(winpath "$obj")")
done

"$RC_EXE" -nologo -fo"$(winpath "$OBJ/Resource.res")" "$(winpath "$ROOT/Src/Res/Resource.rc")"
OBJS+=("$(winpath "$OBJ/Resource.res")")

"$LINK" -nologo -SUBSYSTEM:WINDOWS -MACHINE:X64 -INCREMENTAL:NO -LTCG \
    -OUT:"$(winpath "$BIN/ZDock.exe")" \
    -PDB:"$(winpath "$BIN/ZDock.pdb")" \
    -LIBPATH:"$(winpath "$LING_ROOT/x64/Release")" \
    "${OBJS[@]}" \
    Ling.lib yoga.lib \
    kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib \
    uuid.lib comdlg32.lib winspool.lib \
    dwmapi.lib windowsapp.lib comctl32.lib imm32.lib version.lib \
    windowscodecs.lib shlwapi.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib \
    ntdll.lib Userenv.lib
# ⚠ 手工调 link.exe 不会拿到 MSBuild 默认注入的那批系统库（user32/shell32/…），
#   必须自己列出来，否则全是 LNK2001「无法解析的外部符号 __imp_xxx」。

echo "==> $(winpath "$BIN/ZDock.exe")"
