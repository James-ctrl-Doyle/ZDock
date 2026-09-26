"""阶段二验收截图：默认面板 / 图标右键菜单 / 配置生效对比。

⚠ 拍图原则（红线）：菜单那张只 BitBlt **菜单自身那一小块矩形**（实测约 214x108 像素），
   绝不整屏 BitBlt —— 全屏会把用户的浏览器内容一起拍进去（2026-09-24 踩过）。
   dock 本体一律 PrintWindow(hwnd, hdc, 2)，只渲染窗口自己。

产出： build/_review/stage2_*.png
"""

import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowDC.argtypes = [wt.HWND]
u32.GetWindowDC.restype = wt.HDC
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]

g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g32.CreateCompatibleBitmap.restype = wt.HBITMAP
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.SelectObject.restype = wt.HGDIOBJ
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteDC.argtypes = [wt.HDC]
g32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wt.DWORD),
                ('biClrImportant', wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wt.DWORD * 3)]


g32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                          ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), ctypes.c_uint]


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
OUT = os.path.join(ROOT, 'build', '_review')
SYSROOT = os.environ.get('SystemRoot', r'C:\Windows')


def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, w, h, bgra):
    raw = b''.join(b'\x00' + bgra[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    return len(png)


def grab(hwnd, x, y, w, h, use_printwindow, flag=2):
    """抓 hwnd 的 (x,y,w,h) 物理像素区域。返回 bgra bytes 或 None。"""
    src = u32.GetWindowDC(hwnd)
    if not src:
        return None
    mem = g32.CreateCompatibleDC(src)
    bmp = g32.CreateCompatibleBitmap(src, w, h)
    old = g32.SelectObject(mem, bmp)
    ok = False
    if use_printwindow:
        ok = bool(u32.PrintWindow(hwnd, mem, flag))
    else:
        ok = bool(g32.BitBlt(mem, 0, 0, w, h, src, x, y, 0x00CC0020))  # SRCCOPY
    data = None
    if ok:
        bi = BITMAPINFO()
        bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.bmiHeader.biWidth = w
        bi.bmiHeader.biHeight = -h
        bi.bmiHeader.biPlanes = 1
        bi.bmiHeader.biBitCount = 32
        bi.bmiHeader.biCompression = 0
        buf = ctypes.create_string_buffer(w * h * 4)
        if g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0):
            data = buf.raw
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, src)
    return data


def main():
    os.makedirs(OUT, exist_ok=True)
    work = tempfile.mkdtemp(prefix='zdock_shot_')
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    exe = os.path.join(work, 'ZDock.exe')
    print('隔离目录: %s' % work)

    items = [os.path.join(SYSROOT, 'explorer.exe'),
             os.path.join(SYSROOT, 'System32', 'notepad.exe'),
             os.path.join(SYSROOT, 'System32', 'mspaint.exe'),
             os.path.join(SYSROOT, 'System32', 'cmd.exe'),
             os.path.join(SYSROOT, 'System32', 'calc.exe')]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'iconSize': 52, 'hoverScale': 1.6, 'items': [{'path': p} for p in items]},
                  f, ensure_ascii=False, indent=2)

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    p = subprocess.Popen([exe], cwd=work)
    time.sleep(3.5)
    hwnd = u32.FindWindowW('ZDock', None)
    if not hwnd:
        print('窗口没起来')
        p.terminate()
        return 2

    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    W, H = rc.right - rc.left, rc.bottom - rc.top
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    print('窗口 %dx%d dpi=%.2f' % (W, H, dpi))

    # --- 图1：默认面板（光标移开）---
    u32.SetCursorPos(rc.left + W // 2, max(0, rc.top - 300))
    time.sleep(1.0)
    data = grab(hwnd, 0, 0, W, H, True)
    if data:
        n = write_png(os.path.join(OUT, 'stage2_default.png'), W, H, data)
        print('  stage2_default.png  %d bytes' % n)

    # --- 图2：悬停放大 ---
    kSideSlack, kPadX, kPadY, kHaloH, iconGap, iconSize = 48, 14, 8, 72, 12, 52
    n_items = len(items)
    panelW_logical = n_items * iconSize + (n_items - 1) * iconGap + 2 * kPadX
    panel_left = (W - panelW_logical * dpi) / 2.0
    row_left = panel_left + kPadX * dpi
    icon_top = kHaloH * dpi + kPadY * dpi
    idx = 2
    cx = rc.left + row_left + (idx * (iconSize + iconGap) + iconSize / 2.0) * dpi
    cy = rc.top + icon_top + iconSize / 2.0 * dpi
    u32.SetCursorPos(int(cx), int(cy))
    time.sleep(1.2)
    data = grab(hwnd, 0, 0, W, H, True)
    if data:
        n = write_png(os.path.join(OUT, 'stage2_hover.png'), W, H, data)
        print('  stage2_hover.png    %d bytes' % n)

    # --- 图3：图标右键菜单 ---
    cxp, cyp = int(cx - rc.left), int(cy - rc.top)
    lp = (cyp << 16) | (cxp & 0xFFFF)
    u32.PostMessageW(hwnd, 0x0204, 0, lp)
    time.sleep(0.1)
    u32.PostMessageW(hwnd, 0x0205, 0, lp)
    time.sleep(1.5)
    hmenu = u32.FindWindowW('#32768', None)
    if hmenu:
        mr = wt.RECT()
        u32.GetWindowRect(hmenu, ctypes.byref(mr))
        mw, mh = mr.right - mr.left, mr.bottom - mr.top
        print('  菜单 rect=(%d,%d,%d,%d) %dx%d' % (mr.left, mr.top, mr.right, mr.bottom, mw, mh))
        # ⚠ 只 BitBlt 菜单自身这块小矩形，不整屏
        mdata = grab(hmenu, 0, 0, mw, mh, False)
        if mdata:
            n = write_png(os.path.join(OUT, 'stage2_menu.png'), mw, mh, mdata)
            print('  stage2_menu.png     %d bytes  (仅菜单自身 %dx%d)' % (n, mw, mh))
        # 关掉菜单：Esc
        u32.keybd_event(0x1B, 0, 0, None)
        time.sleep(0.05)
        u32.keybd_event(0x1B, 0, 2, None)
    else:
        print('  菜单窗口没找到，跳过')

    # --- 图4：空白处右键的全局菜单 ---
    kPadX_ = kPadX
    panelW_logical2 = n_items * iconSize + (n_items - 1) * iconGap + 2 * kPadX_
    panel_left2 = (W - panelW_logical2 * dpi) / 2.0
    gx = rc.left + panel_left2 + (panelW_logical2 - 4) * dpi
    gy = rc.top + (kHaloH + kPadY + 24) * dpi
    u32.SetCursorPos(int(gx), int(gy))
    time.sleep(0.5)
    gxp, gyp = int(gx - rc.left), int(gy - rc.top)
    glp = (gyp << 16) | (gxp & 0xFFFF)
    u32.PostMessageW(hwnd, 0x0204, 0, glp)
    time.sleep(0.1)
    u32.PostMessageW(hwnd, 0x0205, 0, glp)
    time.sleep(1.5)
    gmenu = u32.FindWindowW('#32768', None)
    if gmenu:
        gr = wt.RECT()
        u32.GetWindowRect(gmenu, ctypes.byref(gr))
        gw, gh = gr.right - gr.left, gr.bottom - gr.top
        gdata = grab(gmenu, 0, 0, gw, gh, False)
        if gdata:
            n = write_png(os.path.join(OUT, 'stage2_menu_global.png'), gw, gh, gdata)
            print('  stage2_menu_global.png %d bytes (%dx%d)' % (n, gw, gh))
        u32.keybd_event(0x1B, 0, 0, None)
        time.sleep(0.05)
        u32.keybd_event(0x1B, 0, 2, None)
        time.sleep(0.6)

    u32.SetCursorPos(saved.x, saved.y)
    time.sleep(0.4)
    try:
        p.terminate()
        time.sleep(1.0)
        if p.poll() is None:
            p.kill()
    except Exception:
        pass
    shutil.rmtree(work, ignore_errors=True)
    print('完成 -> %s' % OUT)
    return 0


u32.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.c_void_p]

if __name__ == '__main__':
    sys.exit(main())
