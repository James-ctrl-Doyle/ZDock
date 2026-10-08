#!/usr/bin/env bash
# Ling 发布包路径的**唯一出处**。
#
# 为什么要有这个文件：Ling 的版本号原先抄在 4 个地方（build.sh 与三个 dev 探针），
# 每次升 Ling 都要逐个手改 —— 2026-10-08 升 v1.4.0 时就只改了 build.sh，三个探针漏了。
#
# ⚠ **升级 Ling 时只改下面这一行的 LING_PKG_VER。**
#
# 解析优先级（与 ZPin 的 rebuild_all.sh 一致）：
#   1) $LING_ROOT 环境变量
#   2) ../Ling/dist/ling-<版本>-x64（Ling 仓库打出来的发布包）
#   3) ../Ling（Ling 源码树）

LING_PKG_VER="${LING_PKG_VER:-v1.4.0}"

# 用法： LING_ROOT="$(ling_resolve_root "$ROOT")"     # $ROOT = ZDock 仓库根
ling_resolve_root() {
    if [ -n "${LING_ROOT:-}" ]; then printf '%s' "$LING_ROOT"; return; fi
    local pkg="$1/../Ling/dist/ling-${LING_PKG_VER}-x64"
    if [ -d "$pkg/include" ]; then printf '%s' "$pkg"; return; fi
    printf '%s' "$1/../Ling"
}
