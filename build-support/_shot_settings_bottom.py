"""把设置窗口滚到底部截一张 —— 专门看"高级"段（预留工作区 + 副作用说明）。

⚠ 滚动靠往设置窗口发 WM_MOUSEWHEEL（Ling 的 ScrollerBox 收这个）。
⚠ 打开设置窗口要用 dock 认识的自定义消息 WM_APP_OPEN_SETTINGS ——
   `--show-settings` 那个命令行参数**不存在**（试过，窗口压根不出来）。

跑法： <python> build-support/_shot_settings_bottom.py
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

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
OUT = shot.OUT
WM_MOUSEWHEEL = 0x020A


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def main():
    kill_existing_zdock(verbose=False)
    d = os.path.join(ROOT, '_tmp_sb')
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
    if not docks:
        print('!! 没找到 dock')
        subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
        return 1
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
    cx = (sr.left + sr.right) // 2
    cy = (sr.top + sr.bottom) // 2
    # 向下滚：wParam 高字是 delta（负 = 向下），lParam 低字 x、高字 y（屏幕坐标）
    for _ in range(16):
        u32.PostMessageW(sw, WM_MOUSEWHEEL, (0xFF88 << 16) & 0xFFFFFFFF,
                         ((cy & 0xFFFF) << 16) | (cx & 0xFFFF))
        time.sleep(0.04)
    time.sleep(0.7)
    data, W, H = shot.grab_printwindow(sw)
    if data:
        shot.write_png(os.path.join(OUT, 'settings_bottom.png'), W, H, data)
        print('  截图 settings_bottom.png (%dx%d)' % (W, H))

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.4)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
