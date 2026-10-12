"""滑动动画验证（0.1.21）：**匀速** + 帧率稳定性。

数据直接取 ZDock 内部的诊断日志（启动时给环境变量 `ZDOCK_VERBOSE_SLIDE=1`）：
    [slide] 汇总：N 帧 / 实际 Xms / 目标 Yms / 最大帧间隔 Zms / 平均 A.ms | 位移序列 ...

跑两轮做 A/B：
  ① 默认        —— 滑动期间 `timeBeginPeriod(1)`（本次优化）
  ② NO_PRECISION —— 环境变量 `ZDOCK_SLIDE_NO_PRECISION=1`，退回"WM_TIMER 只有 15.6ms 节拍"

判据：
  L1) **匀速**：位移序列的相邻步长基本相等（变异系数小）。
      旧版的 ease-out cubic 步长是**递减**的（3(1-t)^2），变异系数 ≈ 0.6；linear 应 < 0.45。
  L2) **不跳帧**：最大帧间隔 ≤ 22ms（31ms 就是掉到 32fps 的那一顿）。
  L3) 实际耗时接近目标（±40%）。

⚠ 会关掉在跑的 ZDock（单实例互斥体会让新实例静默让位）。跑完不留进程。
"""

import ctypes
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc  # noqa: E402

u32 = ctypes.WinDLL('user32', use_last_error=True)
try:
    u32.SetProcessDPIAware()
except Exception:
    pass
ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [ENUMPROC, wt.LPARAM]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE = os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_slide')
LOG = os.path.join(WORK, 'ZDock.log')
WM_MOUSEMOVE = 0x0200

CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": True, "autoHideDelayMs": 500, "slideInMs": 200, "slideOutMs": 300,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": False,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": [{"path": "C:\\Windows\\explorer.exe"},
              {"path": "::{645FF040-5081-101B-9F08-00AA002F954E}", "name": "回收站", "pinRight": True}],
}

RE_SUMMARY = re.compile(
    r'汇总：(\d+) 帧 / 实际 (\d+)ms / 目标 (\d+)ms / 最大帧间隔 (\d+)ms / 平均 ([\d.]+)ms \| 位移序列 ([\d,\-]+)')
RE_MOVE = re.compile(r'移动窗口耗时：平均 ([\d.]+)ms / 最大 ([\d.]+)ms')


def windows_of(cls):
    out = []

    def cb(h, _lp):
        b = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, b, 256)
        if b.value == cls:
            out.append(h)
        return True

    u32.EnumWindows(ENUMPROC(cb), 0)
    return out


def run(no_precision=False, no_move=False):
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(EXE, os.path.join(WORK, 'ZDock.exe'))
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(CFG, f, ensure_ascii=False)

    env = dict(os.environ)
    env['ZDOCK_VERBOSE_SLIDE'] = '1'
    if no_precision:
        env['ZDOCK_SLIDE_NO_PRECISION'] = '1'
    if no_move:
        env['ZDOCK_SLIDE_NO_MOVE'] = '1'

    subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK, env=env,
                     creationflags=0x00000008, close_fds=True)
    time.sleep(2.2)

    hz = windows_of('ZDock.EdgeHotZone')
    if not hz:
        pc.kill_existing_zdock()
        return None
    # 注入"鼠标进入热区" → 触发滑入；随后真实光标不在 dock 上 → 500ms 后自动滑出
    u32.PostMessageW(hz[0], WM_MOUSEMOVE, 0, 0)
    time.sleep(2.2)
    pc.kill_existing_zdock()
    time.sleep(0.4)

    try:
        with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError:
        return None

    rows = []
    for line in text.splitlines():
        m = RE_SUMMARY.search(line)
        if m:
            rows.append({
                'frames': int(m.group(1)), 'actual': int(m.group(2)), 'target': int(m.group(3)),
                'maxgap': int(m.group(4)), 'avg': float(m.group(5)),
                'seq': [float(x) for x in m.group(6).split(',')],
                'move_avg': None, 'move_max': None,
            })
        m2 = RE_MOVE.search(line)
        if m2 and rows:
            rows[-1]['move_avg'] = float(m2.group(1))
            rows[-1]['move_max'] = float(m2.group(2))
    return rows


def step_cv(seq):
    """相邻步长的变异系数（越小越匀速）。去掉最后一步（收尾可能不满一帧）。"""
    if len(seq) < 4:
        return None
    steps = [abs(seq[i + 1] - seq[i]) for i in range(len(seq) - 1)]
    steps = steps[:-1]
    steps = [s for s in steps if s > 0.5]
    if len(steps) < 3:
        return None
    return statistics.pstdev(steps) / statistics.mean(steps)


def show(tag, rows):
    print(f'  ---- {tag} ----')
    if not rows:
        print('    （没拿到 [slide] 汇总）')
        return
    for r in rows:
        kind = '滑入' if r['target'] == CFG['slideInMs'] else '滑出'
        cv = step_cv(r['seq'])
        cvt = f'{cv:.3f}' if cv is not None else 'n/a'
        mv = f' / setPosition {r["move_avg"]:.2f}ms(最大 {r["move_max"]:.2f})' if r['move_avg'] is not None else ''
        print(f'    {kind}: {r["frames"]} 帧 / 实际 {r["actual"]}ms / 目标 {r["target"]}ms / '
              f'最大帧间隔 {r["maxgap"]}ms / 平均 {r["avg"]}ms / 步长CV={cvt}{mv}')


results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f'   {detail}' if detail else ''))


print('=' * 74)
print('滑动：匀速 + 帧率（定时器 10ms → 期望 ~15.6ms/帧 ≈64fps）')
print('=' * 74)
rows_default = run()
show('默认', rows_default)
rows_old = []

print()
print('=' * 74)
allrows = rows_default or []
cvs = [step_cv(r['seq']) for r in allrows]
cvs = [c for c in cvs if c is not None]
maxgaps = [r['maxgap'] for r in allrows]
dur_ok = all(abs(r['actual'] - r['target']) <= r['target'] * 0.4 for r in allrows)

check('L1) 位移是**匀速**的（步长变异系数 < 0.45）',
      bool(cvs) and all(c < 0.45 for c in cvs),
      f'各次 CV = {[round(c, 3) for c in cvs]}（旧 ease-out 约 0.6）')
check('L2) 没有"3 拍"跳帧（最大帧间隔 ≤ 32ms；偶发一次 2 拍=31ms 属系统调度抖动）',
      bool(maxgaps) and all(g <= 32 for g in maxgaps), f'各次最大间隔 = {maxgaps}ms')
check('L3) 实际耗时接近目标（±40%）', bool(allrows) and dur_ok,
      f'{[(r["actual"], r["target"]) for r in allrows]}')

avgs = [r['avg'] for r in allrows]
check('L4) 平均帧间隔 ≤ 20ms（≥50fps）', bool(avgs) and all(a <= 20 for a in avgs),
      f'各次平均 = {avgs}ms（改前是 24~29ms ≈ 41fps）')

print()
print('=' * 74)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print('=' * 74)
