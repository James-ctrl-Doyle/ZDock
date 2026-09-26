"""阶段三验收截图：运行指示器 / 临时图标 / 点击切换 / 分组列表。

⚠ 拍图原则（红线）：
   · dock 本体一律 PrintWindow(hwnd, hdc, 2) —— 只渲染窗口自己，绝不整屏 BitBlt。
   · 菜单只 BitBlt **菜单自身那一小块矩形**。
   · **绝不用 SetCursorPos / mouse_event / keybd_event** 之外的物理输入驱动交互，
     而且 SetCursorPos 只用来摆放悬停位置（不点击）。上一阶段用它们点模态菜单，
     把用户的 WorkBuddy 焦点抢走、任务被取消。
     更好的做法：hover 用 PostMessage(WM_MOUSEMOVE) 驱动，
     Ling 的 onHitTest 会在系统询问时按真实光标位置刷新 —— 所以 hover 那几张
     仍需 SetCursorPos（这是唯一可靠的办法），但用完立刻恢复光标位置。

产出： build/_review/stage3_*.png
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.ScreenToClient.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]

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
    src = u32.GetWindowDC(hwnd)
    if not src:
        return None
    mem = g32.CreateCompatibleDC(src)
    bmp = g32.CreateCompatibleBitmap(src, w, h)
    old = g32.SelectObject(mem, bmp)
    if use_printwindow:
        ok = bool(u32.PrintWindow(hwnd, mem, flag))
    else:
        ok = bool(g32.BitBlt(mem, 0, 0, w, h, src, x, y, 0x00CC0020))
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


def shoot_dock(hwnd, name):
    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    W, H = rc.right - rc.left, rc.bottom - rc.top
    data = grab(hwnd, 0, 0, W, H, True)
    if data:
        n = write_png(os.path.join(OUT, name), W, H, data)
        print('  %-26s %d bytes  (%dx%d)' % (name, n, W, H))
        return True
    print('  %-26s ** 截图失败 **' % name)
    return False


def main():
    os.makedirs(OUT, exist_ok=True)
    work = tempfile.mkdtemp(prefix='zdock_s3shot_')
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    exe = os.path.join(work, 'ZDock.exe')
    print('隔离目录: %s' % work)

    # config 里只放 explorer + charmap（charmap 后面由探针自己启动，
    # 这里放它）—— 其余靠"运行时临时图标"出现，正好验证临时图标。
    items = [os.path.join(SYSROOT, 'explorer.exe'),
             os.path.join(SYSROOT, 'System32', 'mspaint.exe')]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'iconSize': 52, 'hoverScale': 1.6, 'items': [{'path': p} for p in items]},
                  f, ensure_ascii=False, indent=2)

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    # 光标先移到屏幕以外，避免 hover 影响"默认面板"那张
    u32.SetCursorPos(0, 0)
    time.sleep(0.3)

    p = subprocess.Popen([exe], cwd=work,
                         env={**os.environ, 'ZDOCK_VERBOSE_IND': '1'})
    time.sleep(3.5)
    hwnd = u32.FindWindowW('ZDock', None)
    if not hwnd:
        print('窗口没起来')
        p.terminate()
        shutil.rmtree(work, ignore_errors=True)
        return 2

    # 打印指示器定位日志（排查"指示器到底摆哪了"）
    logp = os.path.join(work, 'ZDock.log')
    if os.path.exists(logp):
        print('\n--- 指示器定位日志 ---')
        with open(logp, encoding='utf-8', errors='replace') as f:
            for l in f:
                if '[ind]' in l:
                    print('  ' + l.rstrip())

    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    W, H = rc.right - rc.left, rc.bottom - rc.top
    print('窗口 %dx%d dpi=%.2f @ (%d,%d)' % (W, H, dpi, rc.left, rc.top))

    print('\n[图1] 默认面板（此时有系统 app 在跑 → 应有运行指示器 + 临时图标）')
    if not shoot_dock(hwnd, 'stage3_default.png'):
        pass

    print('\n[图2] 启动 charmap（不在 config），看临时图标是否补出来')
    app = subprocess.Popen([os.path.join(SYSROOT, 'System32', 'charmap.exe')])
    time.sleep(2.5)
    shoot_dock(hwnd, 'stage3_temp_icon.png')

    print('\n[图3] 悬停放大（光标移到图标上，只摆位置不点击）')
    # 布局常量（逻辑像素）——和 DockWin.h 保持一致
    kSideSlack, kPadX, kPadY, kHaloH, iconGap, iconSize = 48, 14, 8, 72, 12, 52
    n_items = 4   # explorer + mspaint + 两个临时（WorkBuddy / Chrome 之类）
    panelW_logical = n_items * iconSize + (n_items - 1) * iconGap + 2 * kPadX
    panel_left = (W - panelW_logical * dpi) / 2.0
    row_left = panel_left + kPadX * dpi
    icon_top = kHaloH * dpi + kPadY * dpi
    idx = 0
    cx = rc.left + row_left + (idx * (iconSize + iconGap) + iconSize / 2.0) * dpi
    cy = rc.top + icon_top + iconSize / 2.0 * dpi
    u32.SetCursorPos(int(cx), int(cy))
    time.sleep(1.2)
    shoot_dock(hwnd, 'stage3_hover.png')

    print('\n[图4] 图标右键菜单（临时图标 → 应有"固定到 Dock"）')
    # 找最后一个图标（临时项一般在末尾）
    last_cx = rc.left + row_left + ((n_items - 1) * (iconSize + iconGap) + iconSize / 2.0) * dpi
    u32.SetCursorPos(int(last_cx), int(cy))
    time.sleep(0.5)
    cxp, cyp = int(last_cx - rc.left), int(cy - rc.top)
    lp = (cyp << 16) | (cxp & 0xFFFF)
    u32.PostMessageW(hwnd, 0x0204, 0, lp)   # WM_RBUTTONDOWN
    time.sleep(0.1)
    u32.PostMessageW(hwnd, 0x0205, 0, lp)   # WM_RBUTTONUP
    time.sleep(1.5)
    hmenu = u32.FindWindowW('#32768', None)
    if hmenu:
        mr = wt.RECT()
        u32.GetWindowRect(hmenu, ctypes.byref(mr))
        mw, mh = mr.right - mr.left, mr.bottom - mr.top
        print('  菜单 rect=(%d,%d,%d,%d) %dx%d' % (mr.left, mr.top, mr.right, mr.bottom, mw, mh))
        mdata = grab(hmenu, 0, 0, mw, mh, False)
        if mdata:
            n = write_png(os.path.join(OUT, 'stage3_menu_temp.png'), mw, mh, mdata)
            print('  stage3_menu_temp.png       %d bytes  (仅菜单自身 %dx%d)' % (n, mw, mh))
        # 关菜单：PostMessage WM_KEYDOWN/UP VK_ESCAPE 到菜单窗口（不抢焦点）
        u32.PostMessageW(hmenu, 0x0100, 0x1B, 0)
        time.sleep(0.15)
    else:
        print('  菜单窗口没找到，跳过')

    # 收尾
    u32.SetCursorPos(saved.x, saved.y)
    time.sleep(0.3)
    try:
        app.terminate()
    except Exception:
        pass
    try:
        p.terminate()
        time.sleep(1.0)
        if p.poll() is None:
            p.kill()
    except Exception:
        pass
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True)
    shutil.rmtree(work, ignore_errors=True)
    print('\n完成 -> %s' % OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
