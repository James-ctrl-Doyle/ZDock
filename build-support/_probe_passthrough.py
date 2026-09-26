"""穿透诊断探针：搞清楚 halo 区的点击到底去了哪儿。

跑法： <python> build-support/_probe_passthrough.py
它做三件事，互相对照：
  1) 打印 ZDock.log 里 [hit] 行的最后几条 —— dock 对 halo 点到底返回了什么
  2) 在下层放一个测试窗，看 WindowFromPoint 认为那个点属于谁
  3) 真点一下，统计测试窗收到几次 WM_LBUTTONDOWN
"""

import ctypes
import os
import re
import subprocess
import sys
import time
from ctypes import wintypes as wt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
LOG = os.path.join(ROOT, 'build', 'bin', 'ZDock.log')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zdock_stage1_test import (  # noqa: E402  复用已声明好的绑定
    u32, k32, g32, enum_windows, class_of, rect_of, create_sink, real_click,
    sink_clicks, find_dock_window)

u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND


def describe(hwnd):
    if not hwnd:
        return 'NULL'
    return '%s(0x%X) "%s"' % (class_of(hwnd), hwnd, '')


def main():
    if not os.path.isfile(EXE):
        print('找不到 exe')
        return 2
    for p in (LOG, LOG + '.1'):
        if os.path.exists(p):
            os.remove(p)

    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    hwnd = find_dock_window(8.0)
    if not hwnd:
        print('没找到 dock 窗口')
        return 1
    time.sleep(0.5)
    l, t, r, b = rect_of(hwnd)
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    print('dock 窗口 %s dpi=%.2f' % ((l, t, r, b), dpi))

    halo = wt.POINT(l + 8, t + int(round(20 * dpi)))
    panel_mid = wt.POINT(l + int(round(70 * dpi)), b - int(round(30 * dpi)))

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    sink = create_sink((l, t, r, b))
    time.sleep(0.4)
    print('sink 窗口 %s' % (rect_of(sink),))

    for name, pt in (('halo', halo), ('panel', panel_mid)):
        print('\n--- 光标移到 %s (%d,%d) ---' % (name, pt.x, pt.y))
        u32.SetCursorPos(pt.x, pt.y)
        time.sleep(0.4)
        whose = u32.WindowFromPoint(pt)
        print('  WindowFromPoint -> %s' % describe(whose))
        print('  是 sink 吗: %s' % (whose == sink))

    print('\n--- halo 点真点一下 ---')
    u32.SetCursorPos(halo.x, halo.y)
    time.sleep(0.3)
    sink_clicks['down'] = 0
    real_click(halo.x, halo.y)
    print('  sink 收到 WM_LBUTTONDOWN: %d' % sink_clicks['down'])

    u32.SetCursorPos(saved.x, saved.y)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    print('\n--- ZDock.log 的 [hit] 行（最后 12 条）---')
    if os.path.exists(LOG):
        with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
            hits = [ln.rstrip() for ln in f if '[hit]' in ln]
        for ln in hits[-12:]:
            print('  ' + ln)
    else:
        print('  (没有日志)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
