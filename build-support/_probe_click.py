"""隔离探针：真实鼠标点击注入在这台机器上到底可不可靠？

造一个**独立的顶层测试窗**（置顶、放在屏幕右下的空地），把光标移进去真点一下，
统计它收到的消息。与 dock 无关 —— 只回答"SetCursorPos + mouse_event 能不能把点击
送到目标窗口"。如果这一步就失败，那么任何"点击穿透"的断言都不能用真注入来验证。

跑法： <python> build-support/_probe_click.py
"""

import ctypes
import os
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zdock_stage1_test import (  # noqa: E402
    u32, k32, g32, WNDPROC, WNDCLASSEXW, real_click, sink_clicks, class_of, rect_of)

msg_log = []


def _impl(hwnd, msg, wparam, lparam):
    # 记录所有鼠标相关消息，看清"收到了什么"
    if msg in (0x0201, 0x0202, 0x0200, 0x0084, 0x0021, 0x0006, 0x0007):  # DOWN/UP/MOVE/NCHITTEST/MOUSEACTIVATE/ACTIVATE/...
        name = {0x0201: 'LBUTTONDOWN', 0x0202: 'LBUTTONUP', 0x0200: 'MOUSEMOVE',
                0x0084: 'NCHITTEST', 0x0021: 'MOUSEACTIVATE', 0x0006: 'ACTIVATE', 0x0007: 'SETFOCUS'}.get(msg, hex(msg))
        msg_log.append(name)
        if msg == 0x0201:
            sink_clicks['down'] += 1
    return u32.DefWindowProcW(hwnd, msg, wparam, lparam)


def main():
    proc_fn = WNDPROC(_impl)
    inst = k32.GetModuleHandleW(None)
    cls = 'ZDockClickProbe'
    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
    wc.lpfnWndProc = proc_fn
    wc.hInstance = inst
    wc.hbrBackground = g32.CreateSolidBrush(0x00206020)
    wc.lpszClassName = cls
    if not u32.RegisterClassExW(ctypes.byref(wc)):
        if ctypes.get_last_error() != 1410:
            raise ctypes.WinError(ctypes.get_last_error())

    scr_w = u32.GetSystemMetrics(0)
    scr_h = u32.GetSystemMetrics(1)
    x, y, w, h = scr_w - 320, scr_h - 300, 200, 150
    hwnd = u32.CreateWindowExW(0x00000008 | 0x00000080, cls, 'ClickProbe', 0x80000000 | 0x10000000,
                               x, y, w, h, None, None, inst, None)
    if not hwnd:
        raise ctypes.WinError(ctypes.get_last_error())
    print('探针窗口 %s' % (rect_of(hwnd),))
    time.sleep(0.5)

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    cx, cy = x + w // 2, y + h // 2
    print('点 (%d,%d)' % (cx, cy))
    sink_clicks['down'] = 0
    msg_log.clear()
    real_click(cx, cy)
    time.sleep(0.4)
    print('收到 WM_LBUTTONDOWN: %d' % sink_clicks['down'])
    print('收到的消息序列: %s' % (msg_log,))
    print('点归属: %s' % class_of(u32.WindowFromPoint(wt.POINT(cx, cy))))

    u32.SetCursorPos(saved.x, saved.y)
    u32.DestroyWindow(hwnd)
    return 0 if sink_clicks['down'] >= 1 else 1


if __name__ == '__main__':
    sys.exit(main())
