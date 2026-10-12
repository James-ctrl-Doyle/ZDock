"""验证「滑动被反复重启」已修（0.1.22）。

用户报"滑上来还是好慢"。日志证据：隐藏态下 `滑出开始（184 → 184px）` 每 200ms 刷一条、
最近 300 行里「滑入开始」149 次而「滑动结束」**0 次** —— 动画被反复重启，永远走不完。

根因：`slideIn()` / `slideOut()` 缺少"已经在朝目标滑"的早退，而它们会被**每个鼠标事件**
调到（`refreshHoverFromCursor → keepVisible → slideIn`），每次调用都把 `slideStartTick`
归零、重新计满一遍时长。

⚠ 关键：这个场景**注入复现不了**（注入不产生连续的 hitTest），必须像用户那样
**把真光标停在 dock 上并小幅来回移动**。

判据：
  R1) "停住 + 移动"整个过程中，日志里「滑入开始」**只有 1 次**（修前是几十次）
  R2) 必须出现「滑动结束 -> 展开」
  R3) 滑入实际耗时接近配置时长（不会无限拖）

⚠ 会移动鼠标约 4 秒，结束还原原位；开跑前采样光标位置，在动就放弃。
"""

import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc  # noqa: E402

_ap = argparse.ArgumentParser()
_ap.add_argument('--exe', default=None)
_args = _ap.parse_args()

u32 = ctypes.WinDLL('user32', use_last_error=True)
try:
    u32.SetProcessDPIAware()
except Exception:
    pass
ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [ENUMPROC, wt.LPARAM]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE = _args.exe or os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_restart')
LOG = os.path.join(WORK, 'ZDock.log')
print(f'被测 exe：{EXE}')

CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": True, "autoHideDelayMs": 200, "slideInMs": 120, "slideOutMs": 200,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": False,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": [{"path": "C:\\Windows\\explorer.exe"},
              {"path": "::{645FF040-5081-101B-9F08-00AA002F954E}", "name": "回收站", "pinRight": True}],
}

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f'   {detail}' if detail else ''))


def windows_of(cls):
    out = []

    def cb(h, _lp):
        b = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, b, 256)
        if b.value == cls:
            r = wt.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(ENUMPROC(cb), 0)
    return out


def cursor():
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def read_log():
    try:
        with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


# 用户在用鼠标就别打扰
p0 = cursor()
time.sleep(0.18)
p1 = cursor()
time.sleep(0.18)
p2 = cursor()
if not (p0 == p1 == p2):
    print(f'⚠ 鼠标正在移动（{p0}->{p1}->{p2}），放弃。')
    sys.exit(2)

orig = cursor()

pc.kill_existing_zdock()
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
shutil.copy2(EXE, os.path.join(WORK, 'ZDock.exe'))
with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
    json.dump(CFG, f, ensure_ascii=False)
env = dict(os.environ)
env['ZDOCK_VERBOSE_SLIDE'] = '1'
subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK, env=env,
                 creationflags=0x00000008, close_fds=True)
time.sleep(2.2)

hz = windows_of('ZDock.EdgeHotZone')
if not hz:
    print('!! 没找到热区窗口')
    pc.kill_existing_zdock()
    sys.exit(1)

hzr = hz[0][1]
cx, cy = (hzr[0] + hzr[2]) // 2, (hzr[1] + hzr[3]) // 2

try:
    # 1) 贴到停靠边 → 滑入
    u32.SetCursorPos(cx, cy)
    time.sleep(0.6)
    dock = windows_of('ZDock')
    t = dock[0][1][1] if dock else None
    print(f'  滑入后 dock.top = {t}（隐藏位 {hzr[3]}）')
    check('R0) 贴边后 dock 完成滑入', t is not None and t < hzr[3] - 50, f'top={t}')

    # 2) 把光标移到面板上，小幅来回移动 —— 这正是用户"用 dock"时会做的事，
    #    会持续触发 refreshHoverFromCursor → keepVisible → slideIn
    panel_y = hzr[3] - 60          # 面板中部高度
    base_x = cx
    for i in range(8):
        u32.SetCursorPos(base_x + (12 if i % 2 else -12), panel_y)
        time.sleep(0.07)

    logs = read_log()
    n_in = logs.count('滑入开始')
    n_done = logs.count('滑动结束')
    print(f'  期间：滑入开始 {n_in} 次 / 滑动结束 {n_done} 次')
    check('R1) 全程「滑入开始」只有 1 次（修前是几十次）', n_in == 1, f'{n_in} 次')
    check('R2) 出现了「滑动结束」', n_done >= 1, f'{n_done} 次')

    # 3) 滑入耗时（从诊断汇总读）
    import re
    m = re.search(r'汇总：\d+ 帧 / 实际 (\d+)ms / 目标 (\d+)ms / 最大帧间隔 (\d+)ms', logs)
    if m:
        actual, target = int(m.group(1)), int(m.group(2))
        check('R3) 滑入实际耗时接近目标（±40%）', abs(actual - target) <= target * 0.4,
              f'实际 {actual}ms / 目标 {target}ms')
    else:
        check('R3) 滑入实际耗时接近目标（±40%）', False, '没拿到 [slide] 汇总')

    # 4) 移开 → 应当正常收起，且只有一次滑出
    u32.SetCursorPos(*orig)
    time.sleep(1.2)
    logs2 = read_log()
    dock2 = windows_of('ZDock')
    t2 = dock2[0][1][1] if dock2 else None
    check('R4) 移开后正常收起', t2 is not None and t2 >= hzr[3] - 4, f'top={t2}')
    print(f'  最终：滑入开始 {logs2.count("滑入开始")} / 滑出开始 {logs2.count("滑出开始")} / '
          f'滑动结束 {logs2.count("滑动结束")}')
finally:
    u32.SetCursorPos(*orig)
    time.sleep(0.1)

pc.kill_existing_zdock()

print()
print('=' * 70)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print('=' * 70)
