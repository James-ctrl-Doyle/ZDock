"""给"默认配置"截图：资源管理器 | 运行中的应用 | 回收站。

⚠ 截图实现复用 _shot_stage4 的 grab_printwindow / write_png（同一种截图只留一份）。

跑法： <python> build-support/_shot_default.py
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
import _shot_stage4 as shot   # noqa: E402

u32 = ctypes.WinDLL('user32')
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

ROOT = shot.ROOT
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
OUT = shot.OUT
RB = '::{645FF040-5081-101B-9F08-00AA002F954E}'


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def main():
    kill_existing_zdock(verbose=False)
    d = os.path.join(ROOT, '_tmp_default')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    # ⚠ 故意**不**写 config.json —— 让它走首次启动的默认值
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(3.0)

    cfg_path = os.path.join(d, 'config.json')
    if os.path.exists(cfg_path):
        cfg = json.load(open(cfg_path, encoding='utf-8'))
        print('生成的默认 items：')
        for it in cfg.get('items', []):
            print('   path=%s  name=%s  pinRight=%s'
                  % (it.get('path'), it.get('name', ''), it.get('pinRight', False)))
    else:
        print('!! 没生成 config.json')

    ws = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock') if title_of(w) == '']
    if ws:
        data, W, H = shot.grab_printwindow(ws[0])
        if data:
            shot.write_png(os.path.join(OUT, 'stage7_default_items.png'), W, H, data)
            print('  截图 stage7_default_items.png (%dx%d)' % (W, H))

    log = os.path.join(d, 'ZDock.log')
    if os.path.exists(log):
        lines = open(log, 'rb').read().decode('utf-8', 'replace').splitlines()
        print('关键日志：')
        for l in lines:
            if any(k in l for k in ('提取图标', '位图', '分组变化', 'Dock 共')):
                print('   ' + l.split(']')[-1].strip()[:110])

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.4)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
