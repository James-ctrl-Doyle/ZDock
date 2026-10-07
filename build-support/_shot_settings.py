"""给阶段六截验收图：设置窗口 + 停靠边换轴后的样子。

⚠ 截图实现**复用** `_shot_stage4.py` 里的 `grab_printwindow` / `write_png` ——
   同一件事只留一份实现（这个项目踩过"两份截图实现、下次伸手拿错"的坑）。
   一律 `PrintWindow(hwnd, hdc, 2)`（只渲染窗口自身），**绝不 BitBlt 桌面**。

产出： build/_review/stage6_*.png
跑法： <python> build-support/_shot_settings.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _probe_common import kill_existing_zdock   # noqa: E402
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _shot_stage4 as shot   # noqa: E402  （复用 write_png / grab_printwindow / find_windows）

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
OUT = shot.OUT
WM_APP_OPEN_SETTINGS = 0x8000 + 101


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def kill():
    # ⚠ 不能只按映像名杀 —— 验收时跑的是改名副本（ZDock_0.1.x.exe），对不上。
    #   按窗口类名找 pid 才杀得掉；见 _probe_common.py 的说明。
    kill_existing_zdock(verbose=False)


def start(cfg_overrides):
    kill()
    d = os.path.join(ROOT, '_tmp_shot6')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg.update({'autoHide': False, 'reserveWorkArea': False, 'autoStart': False})
    cfg.update(cfg_overrides)
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.6)
    return p, d


def save(hwnd, name):
    data, W, H = shot.grab_printwindow(hwnd)
    if not data:
        print('  %-34s 截图失败' % name)
        return False
    n = shot.write_png(os.path.join(OUT, name), W, H, data)
    print('  %-34s %d bytes (%dx%d)' % (name, n, W, H))
    return True


def main():
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1
    os.makedirs(OUT, exist_ok=True)
    print('===== 阶段六截图 =====')

    try:
        # ---- 设置窗口 ----
        p, d = start({})
        if p.poll() is None:
            docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
                     if title_of(w) == '']
            if docks:
                u32.PostMessageW(docks[0], WM_APP_OPEN_SETTINGS, 0, 0)
                time.sleep(1.4)
                sw = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
                      if title_of(w) == 'ZDockSettings']
                if sw:
                    save(sw[0], 'stage6_1_设置窗口.png')
                save(docks[0], 'stage6_2_停靠底部.png')
        kill()
        time.sleep(0.5)

        # ---- 停靠左边（换轴后的样子）----
        p, d = start({'dockEdge': 'left', 'iconSize': 56})
        if p.poll() is None:
            docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
                     if title_of(w) == '']
            if docks:
                save(docks[0], 'stage6_3_停靠左边.png')
        kill()
        time.sleep(0.5)

        # ---- 停靠上边 ----
        p, d = start({'dockEdge': 'top', 'iconSize': 56})
        if p.poll() is None:
            docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
                     if title_of(w) == '']
            if docks:
                save(docks[0], 'stage6_4_停靠上边.png')
        kill()
    finally:
        kill()
        shutil.rmtree(os.path.join(ROOT, '_tmp_shot6'), ignore_errors=True)

    print('完成 -> %s' % OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
