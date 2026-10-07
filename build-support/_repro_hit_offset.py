"""精确定位：设置窗口滚过之后，某个鼠标位置实际点亮的是**哪个控件**。

判据（比扫像素可靠）：同一个鼠标位置拍两张 ——
  ① 鼠标放在那里（某个 Button 可能进入 hover）
  ② 鼠标挪到窗口外（所有 hover 复位）
两帧的**差异区域**就是"刚才被 hover 的那个控件"所在的矩形。
把它换算成逻辑坐标，和"这个 y 上**视觉上**是什么控件"一比 ——
不一致就是命中坐标没跟着滚动偏移。

跑法： <python> build-support/_repro_hit_offset.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _probe_common import kill_existing_zdock   # noqa: E402
import _shot_stage4 as shot   # noqa: E402
from _shot_settings import WM_APP_OPEN_SETTINGS   # noqa: E402

u32 = ctypes.WinDLL('user32')
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
WM_MOUSEWHEEL = 0x020A

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
OUT = shot.OUT


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def diff_bbox(a, b, W, H):
    """两帧 BGRA 缓冲的差异包围盒（像素坐标 (x0,y0,x1,y1)；无差异返回 None）。"""
    if not a or not b or len(a) != len(b):
        return None
    x0, y0, x1, y1 = W, H, -1, -1
    for y in range(H):
        base = y * W * 4
        row_diff = False
        for x in range(0, W * 4, 4):
            i = base + x
            if a[i] != b[i] or a[i + 1] != b[i + 1] or a[i + 2] != b[i + 2]:
                xx = x // 4
                if xx < x0: x0 = xx
                if xx > x1: x1 = xx
                row_diff = True
        if row_diff:
            if y < y0: y0 = y
            if y > y1: y1 = y
    if x1 < 0:
        return None
    return (x0, y0, x1, y1)


def post_move(sw, sr, dpi, x_logic, y_logic):
    """直接给设置窗口投递 WM_MOUSEMOVE（lParam = **客户区**坐标）。

    ⚠ 为什么不用 SetCursorPos：探针起的是后台窗口，鼠标移过去时系统不一定
      把 WM_MOUSEMOVE 送到它（hover 也就不会更新）—— 实测扫遍全窗口都是
      "没有控件被点亮"。直接投递消息能确定性地驱动 Ling 的 onMouseMove 广播，
      不依赖窗口激活状态。
    """
    cx = int(round(x_logic * dpi))
    cy = int(round(y_logic * dpi))
    u32.PostMessageW(sw, 0x0200, 0, ((cy & 0xFFFF) << 16) | (cx & 0xFFFF))  # WM_MOUSEMOVE
    time.sleep(0.22)


def probe_point(sw, sr, dpi, x_logic, y_logic, tag):
    """在 (x_logic,y_logic) 制造一次 move，返回被 hover 的控件矩形（逻辑坐标）。"""
    post_move(sw, sr, dpi, -200, -200)          # 先移到客户区外 -> 所有 hover 复位
    time.sleep(0.20)
    off, W, H = shot.grab_printwindow(sw)

    post_move(sw, sr, dpi, x_logic, y_logic)    # 再移到目标点
    time.sleep(0.22)
    on, W2, H2 = shot.grab_printwindow(sw)

    bb = diff_bbox(off, on, W, H)
    if not bb:
        print('   %-3s 逻辑(%3d,%3d) -> 没有控件被点亮' % (tag, x_logic, y_logic))
        return None
    r = tuple(round(v / dpi) for v in bb)
    print('   %-3s 逻辑(%3d,%3d) -> 点亮 x %3d..%3d  y %3d..%3d'
          % (tag, x_logic, y_logic, r[0], r[2], r[1], r[3]))
    return r


def main():
    kill_existing_zdock(verbose=False)
    d = os.path.join(ROOT, '_tmp_off')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(os.path.join(BIN, 'ZDock.exe'), os.path.join(d, 'ZDock.exe'))
    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg.update({'autoHide': False, 'reserveWorkArea': False, 'autoStart': False})
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False)

    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.6)
    docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
             if title_of(w) == '']
    u32.PostMessageW(docks[0], WM_APP_OPEN_SETTINGS, 0, 0)
    time.sleep(1.5)
    wins = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
            if title_of(w) == 'ZDockSettings']
    if not wins:
        print('!! 没找到设置窗口')
        subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
        return 1
    sw = wins[0]
    sr = wt.RECT()
    u32.GetWindowRect(sw, ctypes.byref(sr))
    dpi = u32.GetDpiForWindow(sw) / 96.0
    print('设置窗口 (%d,%d)-(%d,%d)  dpi=%.2f' % (sr.left, sr.top, sr.right, sr.bottom, dpi))

    # 「上」按钮横向中心（逻辑）：kPadX+kLabelW+(kBtnW+8)+kBtnW/2 = 22+176+70+31 = 299
    X = 299

    print('--- A) 滚动前，扫「停靠边」那一行附近 ---')
    for yl in (340, 355, 365, 375, 390, 405):
        probe_point(sw, sr, dpi, X, yl, 'A')

    print('--- B) 滚动 6 格 ---')
    u32.SetCursorPos(sr.left + 3, sr.top + 3)
    time.sleep(0.3)
    for _ in range(6):
        u32.PostMessageW(sw, WM_MOUSEWHEEL, (0xFF88 << 16) & 0xFFFFFFFF,
                         (((sr.top + 300) & 0xFFFF) << 16) | ((sr.left + 300) & 0xFFFF))
        time.sleep(0.06)
    time.sleep(0.7)

    print('--- C) 滚动后再扫（按钮视觉上应该上移了）---')
    # 滚了 6 格之后按钮应整体上移；按用户报的位置重点扫这一带
    for yl in (140, 155, 170, 185, 200, 215, 230, 245, 260, 275, 290, 305, 320, 335, 350, 365, 380):
        probe_point(sw, sr, dpi, X, yl, 'C')

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.4)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
