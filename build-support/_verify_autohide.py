"""自动隐藏完善后的验证（0.1.18）。

覆盖本次每一条改动：
  1) 启动即收起            —— 启动后 dock 直接在屏幕外的隐藏位
  2) 热区真实可命中        —— `WindowFromPoint(热区中心)` 返回热区自己
                             （这就是修 alpha=0 的那条判据）
  3) 热区收到鼠标 → 唤出    —— `PostMessage(WM_MOUSEMOVE)` 给热区 → dock 滑入到展开位
  4) 全屏时热区不命中      —— 注入全屏 → 热区 Hide、WindowFromPoint 不再返回它、dock 让位
  5) 退出全屏 → 热区恢复    —— 命中与让位都回退
  6) 热区厚度可配          —— hotZoneThickness=12 → 热区高 = 12
  7) 热区范围三档          —— dock / half / full 三档宽度关系正确
  8) 配置读写              —— 新字段出现在写出到 config.json 里

⚠ 全程**不移动用户鼠标、不抢焦点**：
   · 命中判定用 `WindowFromPoint`（只读查询，不动指针）；
   · 触发用 `PostMessage` 注入（热区 WndProc 是纯消息驱动的，本来就这么测）；
   · 全屏用跟踪器的注入通道 `WM_APP+100`（不是 SetForegroundWindow）。

跑法： <python> build-support/_verify_autohide.py
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
    u32.SetProcessDPIAware()   # 让坐标是物理像素，和日志口径一致
except Exception:
    pass

ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [ENUMPROC, wt.LPARAM]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.IsWindowVisible.argtypes = [wt.HWND]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE_SRC = os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_autohide')

CLS_DOCK = 'ZDock'
CLS_HOTZONE = 'ZDock.EdgeHotZone'
CLS_TRACKER = 'ZDock.WindowTracker.MsgWnd'
WM_APP = 0x8000
MSG_TEST_INJECT = WM_APP + 100

BASE_CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": True, "autoHideDelayMs": 300, "slideInMs": 120, "slideOutMs": 150,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": True,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": [{"path": "C:\\Windows\\explorer.exe"}],
}

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f'   {detail}' if detail else ''))


def windows_of(cls=None, pid=None, visible_only=False):
    out = []

    def cb(h, _lp):
        b = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, b, 256)
        if cls and b.value != cls:
            return True
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        if visible_only and not u32.IsWindowVisible(h):
            return True
        r = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        out.append((h, p.value, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(ENUMPROC(cb), 0)
    return out


def first(cls, pid=None):
    ws = windows_of(cls=cls, pid=pid)
    return ws[0] if ws else None


def start(cfg_over=None):
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(EXE_SRC, os.path.join(WORK, 'ZDock.exe'))
    cfg = dict(BASE_CFG)
    if cfg_over:
        cfg.update(cfg_over)
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(cfg, f, ensure_ascii=False)
    DETACHED_PROCESS = 0x00000008
    p = subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                         creationflags=DETACHED_PROCESS, close_fds=True)
    time.sleep(2.2)
    return p


def log_text():
    with open(os.path.join(WORK, 'ZDock.log'), 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


# ===========================================================================
print('=' * 74)
print('A) 启动即收起 + 热区可命中 + 注入唤出')
print('=' * 74)
start()

dock = first(CLS_DOCK)
hz = first(CLS_HOTZONE)
check('A1 dock 主窗口存在', dock is not None, f'hwnd={dock[0] if dock else "?"}')
check('A2 热区窗口已创建', hz is not None, f'hwnd={hz[0] if hz else "?"}')

logs = log_text()
check('A3 日志有「启动即收起」', '启动即收起' in logs,
      next((l.split(']')[-1].strip() for l in logs.splitlines() if '启动即收起' in l), '（无）'))

if dock and hz:
    screen_bottom = hz[2][3]            # 热区贴着屏幕底边 → 它的 bottom 就是屏幕底边
    dt = dock[2][1]
    check('A4 启动后 dock 处于隐藏位（顶边≈屏幕底边）',
          dt >= screen_bottom - 4,
          f'dock.top={dt} 屏幕底边={screen_bottom}')

    # ---- 核心判据：热区能不能被真实鼠标命中 ----
    cx = (hz[2][0] + hz[2][2]) // 2
    cy = (hz[2][1] + hz[2][3]) // 2
    hit = u32.WindowFromPoint(wt.POINT(cx, cy))
    hit_cls = ''
    if hit:
        b = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(hit, b, 256)
        hit_cls = b.value
    check('A5 热区可被命中（WindowFromPoint 返回热区自己）',
          hit == hz[0], f'中心({cx},{cy}) 命中 hwnd={hit} 类名={hit_cls!r}')

    # ---- 热区收到鼠标消息 → dock 必须滑入 ----
    # ⚠ 必须**轮询**、不能在固定时刻只测一次：注入唤出后真实光标并不在 dock 上，
    #   slideIn 一结束就会 scheduleHide（延迟 300ms）再收回去 —— 固定 sleep 0.8s
    #   去看，看到的是"已经收好了"。第一版就踩了这个（A6 假 FAIL）。
    u32.PostMessageW(hz[0], 0x0200, 0, 0)   # WM_MOUSEMOVE
    moved_in = False
    for _ in range(14):                     # 最多约 0.7s
        time.sleep(0.05)
        d2 = first(CLS_DOCK)
        if d2 and d2[2][1] < screen_bottom - 50:
            moved_in = True
            break
    check('A6 热区收到鼠标 → dock 滑入展开位', moved_in,
          f'dock.top={first(CLS_DOCK)[2][1]}（隐藏位≈{screen_bottom}）')

    # ---- 真实光标不在 dock 上 → 按配置的「收起延迟 + 滑出时长」自动收回 ----
    #      config: 延迟 300ms / 滑出 150ms，留足余量后应当已在屏幕外。
    time.sleep(1.2)
    d3 = first(CLS_DOCK)
    check('A7 无人使用时按配置时间自动收起',
          d3 and d3[2][1] >= screen_bottom - 4,
          f'dock.top={d3[2][1] if d3 else "?"}')

# ===========================================================================
print()
print('=' * 74)
print('B) 全屏时热区停用，退出后恢复')
print('=' * 74)
trk = first(CLS_TRACKER)
check('B1 跟踪器接收窗口存在', trk is not None, f'hwnd={trk[0] if trk else "?"}')
if trk and hz:
    u32.PostMessageW(trk[0], MSG_TEST_INJECT, 1, 0)   # 注入"全屏"
    time.sleep(1.0)
    hz_vis = u32.IsWindowVisible(hz[0])
    hit2 = u32.WindowFromPoint(wt.POINT(cx, cy))
    check('B2 全屏时热区被隐藏（不参与命中）', not hz_vis, f'IsWindowVisible={bool(hz_vis)}')
    check('B3 全屏时该点命中不再是热区', hit2 != hz[0],
          f'命中 hwnd={hit2}')
    dock3 = first(CLS_DOCK)
    check('B4 全屏时 dock 让位（留在屏幕外）',
          dock3 and dock3[2][1] >= screen_bottom - 4,
          f'dock.top={dock3[2][1] if dock3 else "?"}')

    u32.PostMessageW(trk[0], MSG_TEST_INJECT, 0, 0)   # 退出全屏
    time.sleep(1.0)
    hz_vis2 = u32.IsWindowVisible(hz[0])
    hit3 = u32.WindowFromPoint(wt.POINT(cx, cy))
    check('B5 退出全屏 → 热区恢复命中', bool(hz_vis2) and hit3 == hz[0],
          f'visible={bool(hz_vis2)} 命中={hit3}')


# ===========================================================================
print()
print('=' * 74)
print('C) 热区厚度 / 范围可配')
print('=' * 74)


def hotzone_size(cfg_over):
    start(cfg_over)
    h = first(CLS_HOTZONE)
    if not h:
        return None
    _, _, r = h
    return (r[2] - r[0], r[3] - r[1])


thick = hotzone_size({"hotZoneThickness": 12, "hotZoneSpan": "half"})
check('C1 厚度 12 → 热区高 = 12', thick and thick[1] == 12, f'实测 {thick}')

w_dock = hotzone_size({"hotZoneThickness": 4, "hotZoneSpan": "dock"})
w_half = hotzone_size({"hotZoneThickness": 4, "hotZoneSpan": "half"})
w_full = hotzone_size({"hotZoneThickness": 4, "hotZoneSpan": "full"})
check('C2 范围 dock → 仅面板宽（< 700）', w_dock and w_dock[0] < 700, f'实测 {w_dock}')
check('C3 范围 half → 半屏（约 900~1100）', w_half and 900 <= w_half[0] <= 1100, f'实测 {w_half}')
check('C4 范围 full → 整条边（> 1500）', w_full and w_full[0] > 1500, f'实测 {w_full}')
check('C5 三档宽度递增', bool(w_dock and w_half and w_full
                            and w_dock[0] < w_half[0] < w_full[0]),
      f'{w_dock} < {w_half} < {w_full}')

# ===========================================================================
print()
print('=' * 74)
print('D) 配置读写')
print('=' * 74)
with open(os.path.join(WORK, 'config.json'), 'r', encoding='utf-8') as f:
    saved = json.load(f)
check('D1 config.json 含 hotZoneThickness', 'hotZoneThickness' in saved,
      f'= {saved.get("hotZoneThickness")}')
check('D2 config.json 含 hotZoneSpan', 'hotZoneSpan' in saved,
      f'= {saved.get("hotZoneSpan")}')

pc.kill_existing_zdock()

# ===========================================================================
print()
print('=' * 74)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print('=' * 74)
