"""真实鼠标验证：光标停在屏幕停靠边上时 dock 必须升起并**保持展开**（0.1.19）。

修的两个漏判都只有真实鼠标能暴露：
  · ① `refreshHoverFromCursor()` 判"鼠标还在不在 dock 上"时漏了热区 ——
       热区压在最上层，鼠标贴底边时命中的是热区而不是 dock，被误判成"已离开"
       → dock 升起约 500ms 又被收回。
  · ② `EdgeHotZone` 从没调 `TrackMouseEvent(TME_LEAVE)` 订阅 WM_MOUSELEAVE ——
       `entered` 永远是 true → **热区只有第一次有效**，之后再碰彻底没反应。

⚠ 为什么必须用真实鼠标（前两次"验证通过"都是假的）：
  · 探针 `PostMessage` 直接投给热区 → **绕过系统命中测试**（掩盖了 alpha=0 的坑）；
  · 注入唤出后**真实光标并不在热区里**，于是"自动收回"被当成正确行为；
  · 注入**天然测不出 ②** —— 没有真实移动就没有 WM_MOUSELEAVE。

⚠ 会移动鼠标约 7 秒，**结束前还原到原位**；不点击、不抢焦点。
⚠ 开跑前采样 3 次光标位置，**只要在动就放弃**（说明你正在用鼠标），不做任何移动。

跑法： <python> build-support/_verify_hotzone_hover.py
"""

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
EXE_SRC = os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_hover')
LOG = os.path.join(WORK, 'ZDock.log')

CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": True, "autoHideDelayMs": 500, "slideInMs": 200, "slideOutMs": 300,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": False,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": [{"path": "C:\\Windows\\explorer.exe"}],
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
        if b.value != cls:
            return True
        r = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(ENUMPROC(cb), 0)
    return out


def dock_top():
    d = windows_of('ZDock')
    return d[0][1][1] if d else None


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


def start():
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(EXE_SRC, os.path.join(WORK, 'ZDock.exe'))
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(CFG, f, ensure_ascii=False)
    subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                     creationflags=0x00000008, close_fds=True)
    time.sleep(2.2)


# ---- 你正在用鼠标的话就放弃，绝不打搅 ----
p0, p1 = cursor(), None
time.sleep(0.18)
p1 = cursor()
time.sleep(0.18)
p2 = cursor()
if not (p0 == p1 == p2):
    print(f'⚠ 检测到鼠标正在移动（{p0} -> {p1} -> {p2}）—— 你正在用鼠标。')
    print('  已放弃（不做任何移动）。稍后重跑即可。')
    sys.exit(2)

start()

hz = windows_of('ZDock.EdgeHotZone')
check('热区窗口存在', bool(hz))
if not hz:
    pc.kill_existing_zdock()
    sys.exit(1)

_, hzr = hz[0]
cx, cy = (hzr[0] + hzr[2]) // 2, (hzr[1] + hzr[3]) // 2
bottom = hzr[3]
orig = cursor()
print(f'  热区={hzr} 中心=({cx},{cy}) 屏幕底边={bottom} 原光标={orig}')

try:
    # ================= 第 1 次：贴住 =================
    u32.SetCursorPos(cx, cy)
    time.sleep(0.12)
    mv = cursor()
    check('① 光标已移到停靠边上', abs(mv[0] - cx) <= 2 and abs(mv[1] - cy) <= 2,
          f'实测 {mv} 目标 ({cx},{cy})')

    tops = []
    for _ in range(4):                 # ~1.6s，远超 500ms 延迟 + 300ms 滑出
        time.sleep(0.4)
        tops.append(dock_top())
    check('② 贴住期间 dock 升起并**保持展开**',
          bool(tops) and all(t is not None and t < bottom - 50 for t in tops),
          f'dock.top 采样={tops}（展开位应 < {bottom - 50}，隐藏位={bottom}）')

    during = read_log()
    check('③ 贴住期间没有发生"滑出"',
          during.count('滑出开始') == 0,
          f'滑出次数={during.count("滑出开始")}')

    # ================= 移开 → 应按时收回 =================
    u32.SetCursorPos(*orig)
    time.sleep(1.6)
    t = dock_top()
    check('④ 光标移开后 dock 正常收回', t is not None and t >= bottom - 4,
          f'移开后 dock.top={t}')

    # ================= 第 2 次：再贴一次（验 TrackMouseEvent） =================
    u32.SetCursorPos(cx, cy)
    again = False
    for _ in range(14):                # 最多 ~1.4s 等它升起
        time.sleep(0.1)
        t = dock_top()
        if t is not None and t < bottom - 50:
            again = True
            break
    check('⑤ **第二次**贴停靠边 dock 仍能升起（entered 能重置）', again,
          f'第二次贴住后 dock.top={dock_top()}')

    # 再确认第二次也能保持
    time.sleep(1.0)
    t = dock_top()
    check('⑥ 第二次贴住也能保持展开', t is not None and t < bottom - 50,
          f'dock.top={t}')

    u32.SetCursorPos(*orig)
    time.sleep(1.6)

finally:
    u32.SetCursorPos(*orig)            # 无论如何还原
    time.sleep(0.1)

logs = read_log()
enter_times = logs.count('鼠标进入')
check('⑦ 日志里出现了 2 次"鼠标进入"（两次都真的触发了）', enter_times >= 2,
      f'出现 {enter_times} 次')

pc.kill_existing_zdock()
u32.SetCursorPos(*orig)

print()
print('=' * 70)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print('=' * 70)
