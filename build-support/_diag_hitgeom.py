"""诊断：把设置窗口"滚动前 / 滚动后"的几何打出来，看命中坐标有没有跟着滚。

靠程序里的诊断开关 `ZDOCK_VERBOSE_SETTINGS=1`（见 SettingsWin::dumpHitGeom）——
它会把 `ScrollerBox::scrollY`、`content` 的 y/h、以及 content 每个子节点的
**x/y/w/h（isPosIn 用的绝对坐标）** 打进 ZDock.log。

跑法： <python> build-support/_diag_hitgeom.py
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

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
u32.PostMessageW.restype = wt.BOOL
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
WM_APP_OPEN_SETTINGS = 0x8000 + 101
WM_MOUSEWHEEL = 0x020A


def lp(x, y):
    return ctypes.c_ssize_t(((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)).value


def title_of(h):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return t.value


def main():
    kill_existing_zdock(verbose=False)
    d = os.path.join(ROOT, '_tmp_hitgeom')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    json.dump({'dockEdge': 'bottom', 'dockAlign': 'start', 'dockOffset': 0,
               'iconGap': 12, 'iconSize': 48, 'autoHide': False,
               'hideOnFullscreen': False, 'reserveWorkArea': False, 'autoStart': False},
              open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    env = dict(os.environ)
    env['ZDOCK_VERBOSE_SETTINGS'] = '1'
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d, env=env)
    time.sleep(2.6)

    docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock') if title_of(w) == '']
    if not docks:
        print('!! 没找到 dock'); return 1
    u32.PostMessageW(docks[0], WM_APP_OPEN_SETTINGS, 0, 0)
    time.sleep(1.4)
    sws = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
           if title_of(w) == 'ZDockSettings']
    if not sws:
        print('!! 没找到设置窗口'); return 1
    sw = sws[0]
    r = wt.RECT()
    u32.GetWindowRect(sw, ctypes.byref(r))
    dpi = u32.GetDpiForWindow(sw) / 96.0
    print('设置窗口 (%d,%d)-(%d,%d)  dpi=%.3f' % (r.left, r.top, r.right, r.bottom, dpi))

    # 滚轮：lParam 用**屏幕**坐标；delta 必须为负（向下）
    sx = r.left + int(round(300 * dpi))
    sy = r.top + int(round(300 * dpi))
    for i in range(4):
        u32.PostMessageW(sw, WM_MOUSEWHEEL,
                         ctypes.c_size_t(((-120) << 16) & 0xFFFFFFFF), lp(sx, sy))
        time.sleep(0.10)
    time.sleep(0.9)

    log = os.path.join(d, 'ZDock.log')
    if not os.path.exists(log):
        print('!! 没有 ZDock.log'); return 1
    lines = open(log, 'rb').read().decode('utf-8', 'replace').splitlines()
    hits = [l.split(']', 1)[-1].strip() for l in lines if '[hit]' in l]
    print()
    print('=== 诊断输出（共 %d 行，只挑关键节点）===' % len(hits))
    # 只看：头部（scrollY/content y）+ 停靠边那排按钮(#26/#27) + 对齐(#32) + 几个开关(#41/#45/#48)
    KEY = ('#26 ', '#27 ', '#32 ', '#41 ', '#45 ', '#48 ')
    for l in hits:
        if 'scrollY=' in l or any(k in l for k in KEY):
            print('  ' + l)
    print()
    print('=== 其余新日志（尾部 12 行）===')
    for l in [l.split(']', 1)[-1].strip() for l in lines][-12:]:
        print('  ' + l[:140])

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.5)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
