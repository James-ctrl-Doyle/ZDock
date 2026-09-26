#!/usr/bin/env bash
# 直接调 cl.exe / link.exe / rc.exe 编 ZDock 的最小 MSVC 环境（不走 MSBuild）。
# 用法：  . "$(cd "$(dirname "$0")" && pwd)/_msvc_env.sh"
# 引入后可用：$CL $LINK $RC_EXE $MSVC $SDK_VER，以及 winpath()。
#
# 找路径失败时可用环境变量覆盖：
#   MSVC_ROOT  VC\Tools\MSVC\<版本> 目录（Windows 风格）
#   SDK_ROOT   Windows Kits\10 目录（POSIX 风格，如 "/c/Program Files (x86)/Windows Kits/10"）
export PATH="/usr/bin:/bin:/c/Windows/System32:$PATH"
# ⚠ http_proxy / HTTP_PROXY 两套大小写同时存在时，MSVC 工具的环境字典会撞键（MSB6001）
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY

winpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"
    else printf '%s' "$1" | sed -e 's|^/\([a-zA-Z]\)/|\1:\\|' -e 's|/|\\|g'; fi
}

_find_msvc() {
    if [ -n "${MSVC_ROOT:-}" ]; then printf '%s' "$MSVC_ROOT"; return; fi
    local vswhere='/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
    if [ -x "$vswhere" ]; then
        local p
        p="$("$vswhere" -latest -find 'VC/Tools/MSVC/*/include' 2>/dev/null | head -1)"
        if [ -n "$p" ]; then printf '%s' "$(winpath "$(dirname "$p")")"; return; fi
    fi
    printf '%s' 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
}

MSVC="$(_find_msvc)"
MSVC_BIN="$MSVC\\bin\\Hostx64\\x64"

SDK_BASE_POSIX="${SDK_ROOT:-/c/Program Files (x86)/Windows Kits/10}"
SDK_VER="$(ls "$SDK_BASE_POSIX/Include" 2>/dev/null | sort -V | tail -1)"
SDK_BASE="$(winpath "$SDK_BASE_POSIX")"
SDK_I="$SDK_BASE\\Include\\$SDK_VER"
SDK_L="$SDK_BASE\\Lib\\$SDK_VER"

export INCLUDE="$MSVC\\include;$SDK_I\\ucrt;$SDK_I\\shared;$SDK_I\\um;$SDK_I\\winrt;$SDK_I\\cppwinrt"
export LIB="$MSVC\\lib\\x64;$SDK_L\\ucrt\\x64;$SDK_L\\um\\x64"

CL="$MSVC_BIN\\cl.exe"
LINK="$MSVC_BIN\\link.exe"
RC_EXE="$SDK_BASE\\bin\\$SDK_VER\\x64\\rc.exe"

echo "MSVC : $MSVC"
echo "SDK  : $SDK_VER"
