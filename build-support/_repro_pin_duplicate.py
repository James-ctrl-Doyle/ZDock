r"""复现「右键固定一个程序之后，dock 上出现两个图标」。

判据（精确到"这一个应用"）：打开 ZDock 的逐项诊断 `ZDOCK_VERBOSE_ITEMS=1`，
它在每次跟踪同步后会把 items 逐条打出来：
    [items] #1 PINNED 'AUMID 探针' key='' path=...\AumidProbeApp.exe
    [items] #2 TEMP   'AUMID 探针' key='aumid:ZDock.Probe.AumidApp' path=...
—— 同一个 exe 既有一条 PINNED 又有一条 TEMP，就是用户看到的"两个图标"。
⚠ 不用日志里那行总数（"Dock 共 N 项，其中临时 K"）：那会被机器上其它正在跑的应用搅乱。

⚠ 复现不去动用户正在用的 Chrome：用 `_probe_aumid_app.cpp` 编出来的 **AumidProbeApp.exe**
   —— 自带 AUMID 的窗口，等价于"从带 AppUserModelID 的快捷方式启动的 Chrome"。
   ZDock 的 WindowTracker 按 AUMID 优先分组（键 = `aumid:...`），DockWin 却用
   `findByExePath()`（只认 `path:` 前缀）找分组 → 固定项认不出自己的运行分组。

⚠ 也试过拿"运行指示器小圆点"和"点固定图标会不会再起一个实例"当第二条判据，
  但**两条都分辨不出好坏**（修复前它们就已经是"正常"的表现，原因没查清），
  所以只留下面这一条确定性的判据：**同一个 exe 有没有同时出现 PINNED 和 TEMP**。

跑法：
  <python> build-support/_repro_pin_duplicate.py                 # 用 build/bin/ZDock.exe
  <python> build-support/_repro_pin_duplicate.py --exe <exe> --tag after
"""

import argparse
import ctypes
import json
import os
import re
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

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
OUT = shot.OUT
PROBE = os.path.join(ROOT, 'build', '_probe_aumid', 'AumidProbeApp.exe')
PROBE_IMAGE = 'AumidProbeApp.exe'
AUMID = 'ZDock.Probe.AumidApp'
RECYCLE_CLSID = '::{645FF040-5081-101B-9F08-00AA002F954E}'
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
CREATE_NO_WINDOW = 0x08000000

ITEM_RE = re.compile(r"\[items\] #\d+\s+(PINNED|TEMP)\s+'([^']*)'\s+key='([^']*)'\s+path=(.*)$")


def lp(x, y):
    return ctypes.c_ssize_t(((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)).value


def title_of(h):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return t.value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(BIN, 'ZDock.exe'))
    ap.add_argument('--tag', default='before')
    args = ap.parse_args()

    if not os.path.exists(PROBE):
        print('!! 缺少 %s —— 先跑 bash build-support/_probe_aumid_app.sh' % PROBE)
        return 1

    kill_existing_zdock(verbose=False)
    subprocess.run(['taskkill', '/F', '/IM', PROBE_IMAGE], capture_output=True)

    d = os.path.join(ROOT, '_tmp_pin')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    exe = os.path.join(d, 'ZDock.exe')
    shutil.copy2(args.exe, exe)
    shutil.copy2(PROBE, os.path.join(d, PROBE_IMAGE))
    probe_in_dock = os.path.join(d, PROBE_IMAGE)

    # 关键：配置里**已经固定了**这个 AUMID 应用 —— 正是用户"右键固定之后"的状态
    win_dir = os.environ.get('WINDIR', r'C:\Windows')
    cfg = {
        'dockEdge': 'bottom', 'dockAlign': 'start', 'dockOffset': 0,
        'iconGap': 12, 'iconSize': 48, 'autoHide': False,
        'hideOnFullscreen': False, 'reserveWorkArea': False, 'autoStart': False,
        'items': [
            {'path': os.path.join(win_dir, 'explorer.exe'), 'name': '资源管理器'},
            {'path': probe_in_dock, 'name': 'AUMID 探针'},          # ← 已固定
            {'path': RECYCLE_CLSID, 'name': '回收站', 'pinRight': True},
        ],
    }
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    # 1) 先起那个"AUMID 应用"（等价于 Chrome 已经在跑）
    probe_log = os.path.join(d, 'probe_out.txt')
    with open(probe_log, 'w', encoding='utf-8') as fout:
        pp = subprocess.Popen([probe_in_dock], cwd=d, stdout=fout,
                              creationflags=CREATE_NO_WINDOW)
    time.sleep(1.6)
    selfcheck_ok = False
    if os.path.exists(probe_log):
        for line in open(probe_log, encoding='utf-8', errors='replace'):
            if '[aumid]' in line:
                print('   ' + line.rstrip())
            if 'selfcheck = OK' in line:
                selfcheck_ok = True
    if not selfcheck_ok:
        print('!! AUMID 自检没过 —— 本次复现无效，别下结论')
        subprocess.run(['taskkill', '/F', '/PID', str(pp.pid)], capture_output=True)
        return 1

    # 2) 起 ZDock（打开逐项诊断）
    env = dict(os.environ)
    env['ZDOCK_VERBOSE_ITEMS'] = '1'
    p = subprocess.Popen([exe], cwd=d, env=env)
    time.sleep(3.5)

    # 3) 读逐项诊断
    lp_ = os.path.join(d, 'ZDock.log')
    lines = []
    if os.path.exists(lp_):
        lines = open(lp_, 'rb').read().decode('utf-8', 'replace').splitlines()
    rows = []
    for raw in [l for l in lines if '[items]' in l]:
        # ⚠ 匹配**整行**：日志格式是 `<日期> <时间> [items] #0 ...`（时间戳不带方括号），
        #   用 split(']',1)[-1] 会把 "[items]" 这个前缀一起切掉，正则就匹配不上了。
        m = ITEM_RE.search(raw)
        if m:
            rows.append((m.group(1), m.group(2), m.group(3), m.group(4)))

    print()
    print('   逐项诊断（只挑探针这个应用；完整列表见下）')
    pinned = [r for r in rows if r[3].lower().endswith(PROBE_IMAGE.lower()) and r[0] == 'PINNED']
    temp = [r for r in rows if r[3].lower().endswith(PROBE_IMAGE.lower()) and r[0] == 'TEMP']
    for r in pinned + temp:
        print('     %-6s %-14s key=%s' % (r[0], r[1], r[2] if r[2] else '(空)'))
    if not rows:
        print('     (没读到 [items] 日志 —— 判据不可用)')
    else:
        print('     —— 本次 items 共 %d 条（含其它应用）' % len(rows))

    # 4) 截图（视觉证据）
    dock = None
    data = None
    W = H = 0
    docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock') if title_of(w) == '']
    if docks:
        dock = docks[0]
        r = wt.RECT()
        u32.GetWindowRect(dock, ctypes.byref(r))
        data, W, H = shot.grab_printwindow(dock)
        if data:
            png = os.path.join(OUT, 'pin_dup_%s.png' % args.tag)
            shot.write_png(png, W, H, data)
            print('   截图 %s (%dx%d)' % (os.path.basename(png), W, H))

    print()
    if not rows:
        print('   => ⚠ 判据不可用')
    elif temp:
        print('   => ❌ 复现：同一个应用占了 %d 个图标（PINNED %d 条 + TEMP %d 条）'
              % (len(pinned) + len(temp), len(pinned), len(temp)))
        if pinned and not pinned[0][2]:
            print('      固定项的 key 是空的 = 它没认出自己的运行分组（匹配断了）')
    elif pinned and pinned[0][2]:
        print('   => ✅ 只有一个图标，且固定项认出了运行分组（key=%s）' % pinned[0][2])
    else:
        print('   => ⚠ 没有 TEMP 了，但固定项的 key 仍是空的（匹配还是没通）')

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    subprocess.run(['taskkill', '/F', '/IM', PROBE_IMAGE], capture_output=True)
    time.sleep(0.6)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
