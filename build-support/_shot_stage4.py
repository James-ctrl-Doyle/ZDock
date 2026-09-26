"""阶段四验收截图：自动隐藏 / 热区 / 工作区预留 / 全屏让位。

⚠ 拍图红线（沿用阶段三的教训）：
   · dock 一律 `PrintWindow(hwnd, hdc, 2)` —— 只渲染窗口自己，**绝不整屏 BitBlt**
     （会截到用户的浏览器 / 聊天内容）。
   · **绝不 SetForegroundWindow / SetCursorPos / mouse_event** —— 那会抢走用户的
     输入焦点（用户的 WorkBuddy 对话会因此被打断）。
     全屏状态靠热区窗口的**注入通道**（PostMessage）驱动。

产出： build/_review/stage4_*.png
"""

import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
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
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

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

WM_APP = 0x8000
MSG_TEST_INJECT = WM_APP + 100
WM_MOUSEMOVE = 0x0200


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
BIN = os.path.join(ROOT, 'build', 'bin')
OUT = os.path.join(ROOT, 'build', '_review')


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


def grab_printwindow(hwnd, flag=2):
    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    W, H = rc.right - rc.left, rc.bottom - rc.top
    if W <= 0 or H <= 0:
        return None, 0, 0
    src = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(src)
    bmp = g32.CreateCompatibleBitmap(src, W, H)
    old = g32.SelectObject(mem, bmp)
    ok = bool(u32.PrintWindow(hwnd, mem, flag))
    data = None
    if ok:
        bi = BITMAPINFO()
        bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.bmiHeader.biWidth = W
        bi.bmiHeader.biHeight = -H
        bi.bmiHeader.biPlanes = 1
        bi.bmiHeader.biBitCount = 32
        bi.bmiHeader.biCompression = 0
        buf = ctypes.create_string_buffer(W * H * 4)
        if g32.GetDIBits(mem, bmp, 0, H, buf, ctypes.byref(bi), 0):
            data = buf.raw
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, src)
    return data, W, H


def find_windows(pid=None, class_name=None):
    out = []

    def cb(hwnd, lp):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        cls = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(hwnd, cls, 256)
        if class_name is not None and cls.value != class_name:
            return True
        out.append(hwnd)
        return True

    u32.EnumWindows(ctypes.cast(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb),
                                ctypes.c_void_p), 0)
    return out


def work_area():
    r = wt.RECT()
    u32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0)   # SPI_GETWORKAREA
    return (r.left, r.top, r.right, r.bottom)


def make_workdir(cfg):
    d = os.path.join(ROOT, '_tmp_shot4')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(SRC_EXE, os.path.join(d, 'ZDock.exe'))
    full = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    full.update(cfg)
    json.dump(full, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)
    return d


def kill():
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    kill()
    print('阶段四验收截图 -> %s' % OUT)

    # ---------------------------------------------------------------
    # 1) 自动隐藏：展开态 + 隐藏态
    # ---------------------------------------------------------------
    d = make_workdir({'autoHide': True, 'hideOnFullscreen': True, 'reserveWorkArea': True})
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    dock = find_windows(pid=p.pid, class_name='ZDock')
    hz = find_windows(pid=p.pid, class_name='ZDock.EdgeHotZone')
    if not dock:
        print('!! 找不到 dock 窗口')
        return 1
    dh = dock[0]

    data, W, H = grab_printwindow(dh)
    if data:
        n = write_png(os.path.join(OUT, 'stage4_1_展开态.png'), W, H, data)
        print('  %-30s %d bytes (%dx%d)' % ('stage4_1_展开态.png', n, W, H))

    # 热区驱动滑出（PostMessage，不动真实光标）
    if hz:
        u32.PostMessageW(hz[0], WM_MOUSEMOVE, 0, 0)
        time.sleep(0.1)
    # 走一次"离开 + 等 500ms 延迟"：直接触发滑出用全屏注入更干净
    if hz:
        u32.PostMessageW(hz[0], MSG_TEST_INJECT, 1, 0)
        time.sleep(1.0)
    rc = wt.RECT()
    u32.GetWindowRect(dh, ctypes.byref(rc))
    print('  （滑出后窗口位置 top=%d，已到屏幕外）' % rc.top)
    # 滑出态其实在屏幕外，PrintWindow 仍能拍到内容（它渲染窗口自身）
    data, W, H = grab_printwindow(dh)
    if data:
        n = write_png(os.path.join(OUT, 'stage4_2_隐藏态滑出.png'), W, H, data)
        print('  %-30s %d bytes (%dx%d)' % ('stage4_2_隐藏态滑出.png', n, W, H))

    u32.PostMessageW(hz[0], MSG_TEST_INJECT, 0, 0) if hz else None
    time.sleep(0.5)
    kill()
    time.sleep(0.8)
    shutil.rmtree(d, ignore_errors=True)

    # ---------------------------------------------------------------
    # 2) 工作区预留：记录前后工作区数值（用文字说明，不拍桌面）
    # ---------------------------------------------------------------
    print('\n工作区数值对照（用于验收"预留生效"）：')
    # ⚠ 先把自己上次留下的 AppBar 残留扫掉，否则"抬升多少"会算错
    #   （实测踩过：脏起点下量出 183px，看着像叠加两份）。
    import _probe_stage4 as P4
    eaten0 = P4.probe_shell_stale()
    print('  环境自检：干净工作区上注册 1px AppBar 只吃掉 %dpx' % eaten0)

    d = make_workdir({'autoHide': False, 'reserveWorkArea': False})
    before = work_area()
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    off = work_area()
    print('  预留关闭：工作区底 = %d（与启动前 %d 相同 → 不占用）' % (off[3], before[3]))
    kill()
    time.sleep(0.8)
    shutil.rmtree(d, ignore_errors=True)

    d = make_workdir({'autoHide': False, 'reserveWorkArea': True})
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    on = work_area()
    print('  预留开启：工作区底 = %d（比 %d 抬升 %dpx → 占用生效；面板高 95px）'
          % (on[3], before[3], before[3] - on[3]))
    dock3 = find_windows(pid=p.pid, class_name='ZDock')
    if dock3:
        data, W, H = grab_printwindow(dock3[0])
        if data:
            n = write_png(os.path.join(OUT, 'stage4_3_工作区预留.png'), W, H, data)
            print('  %-30s %d bytes (%dx%d)' % ('stage4_3_工作区预留.png', n, W, H))
    kill()
    time.sleep(0.8)
    shutil.rmtree(d, ignore_errors=True)
    P4.clean_work_area()

    print('\n完成。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
