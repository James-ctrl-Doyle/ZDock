"""Bug 2 诊断：悬停时图标缩放到底在抖什么？

思路：把光标**钉**在某个图标中心不动，然后高频采样该图标所在像素区域的截图，
统计"图标占了多少像素"随时间的变化。真正的"一直保持放大"应该是一条平稳的高位线；
来回变大变小会看到像素数反复上下跳。

同时把 ZDock 的 [hit] 诊断日志开着（ZDOCK_VERBOSE_HIT=1），直接看 hover 索引有没有抖。

跑法： <python> build-support/_probe_hover_jitter.py
输出：控制台时序 + build/_review/hover_jitter.txt
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zdock_stage1_test import (  # noqa: E402
    u32, g32, k32, enum_windows, class_of, rect_of, find_dock_window, write_png)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
LOG = os.path.join(ROOT, 'build', 'bin', 'ZDock.log')
OUT = os.path.join(ROOT, 'build', '_review', 'hover_jitter.txt')


def grab_bgra(hwnd, l, t, w, h):
    """抓窗口客户区像素（BGRA，GetDIBits 顺序）。只抓窗口自身，不碰桌面。"""
    hdc_win = u32.GetWindowDC(hwnd)
    hdc_mem = g32.CreateCompatibleDC(hdc_win)
    hbmp = g32.CreateCompatibleBitmap(hdc_win, w, h)
    g32.SelectObject(hdc_mem, hbmp)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long),
                    ('biHeight', ctypes.c_long), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                    ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                    ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wt.DWORD),
                    ('biClrImportant', wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h          # 负数 = 自上而下，省得翻
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    # PrintWindow(..., 2) = PW_RENDERFULLCONTENT：只渲染窗口自身
    u32.PrintWindow(hwnd, hdc_mem, 2)
    g32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)

    g32.DeleteObject(hbmp)
    g32.DeleteDC(hdc_mem)
    u32.ReleaseDC(hwnd, hdc_win)
    return bytearray(buf.raw)


def lit_pixels(bgra, w, h, thresh=40):
    """统计"明显不是背景"的像素数（按亮度阈值）。"""
    n = 0
    for i in range(0, len(bgra), 4):
        b, g, r = bgra[i], bgra[i + 1], bgra[i + 2]
        if r + g + b > thresh * 3:
            n += 1
    return n


def main():
    if not os.path.isfile(EXE):
        print('找不到 exe')
        return 2
    for p in (LOG, LOG + '.1'):
        if os.path.exists(p):
            os.remove(p)

    env = dict(os.environ)
    env['ZDOCK_VERBOSE_HIT'] = '1'
    env['ZDOCK_VERBOSE_HOVER'] = '1'
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env)
    hwnd = find_dock_window(8.0)
    if not hwnd:
        print('没找到 dock 窗口')
        proc.terminate()
        return 1
    time.sleep(1.0)

    l, t, r, b = rect_of(hwnd)
    w, h = r - l, b - t
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    print('dock 窗口 0x%X  (%d,%d)-(%d,%d)  dpi=%.2f' % (hwnd, l, t, r, b, dpi))

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    # 第一个图标中心：面板左边距 48(sideSlack)+14(padX) 逻辑，图标半宽 24
    icon_cx = l + int(round((48 + 14 + 24) * dpi))
    icon_cy = t + h - int(round((6 + 8 + 24) * dpi))   # 参考面板内图标垂直中心
    print('把光标钉在图标中心 (%d,%d)，采样 3 秒不移动' % (icon_cx, icon_cy))

    u32.SetCursorPos(icon_cx, icon_cy)
    time.sleep(0.6)      # 让动画跑完

    samples = []
    t0 = time.time()
    while time.time() - t0 < 3.0:
        px = grab_bgra(hwnd, l, t, w, h)
        samples.append(lit_pixels(px, w, h))
        time.sleep(0.08)

    u32.SetCursorPos(saved.x, saved.y)
    time.sleep(0.4)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    print('\n亮像素计数序列（%d 个样本）：' % len(samples))
    print('  ' + ' '.join(str(s) for s in samples))
    lo, hi = min(samples), max(samples)
    spread = hi - lo
    print('  min=%d max=%d 波动=%d (%.1f%%)' % (lo, hi, spread, 100.0 * spread / max(hi, 1)))
    # 判据：稳定悬停时波动应该很小（<5%）；反复缩放会 >20%
    verdict = '抖动（反复缩放）' if spread > 0.20 * hi else '稳定'
    print('  => %s' % verdict)

    print('\n--- ZDock.log 的 [hit] 行 ---')
    hits = []
    if os.path.exists(LOG):
        with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
            hits = [ln.rstrip() for ln in f if '[hit]' in ln]
    for ln in hits[-25:]:
        print('  ' + ln)
    if not hits:
        print('  (没有 [hit] 行 —— NOHITTEST 没被调用？)')

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('亮像素计数序列: %s\n' % samples)
        f.write('min=%d max=%d spread=%d\n' % (lo, hi, spread))
        f.write('verdict=%s\n\n[hit] 行:\n' % verdict)
        for ln in hits[-40:]:
            f.write(ln + '\n')
    print('\n已写 %s' % OUT)
    return 0 if verdict == '稳定' else 1


if __name__ == '__main__':
    sys.exit(main())
