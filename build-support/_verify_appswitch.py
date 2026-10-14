# 用真实鼠标模拟用户操作，复现「切换应用时 dock 弹出又隐藏」。
#
# 用户原步骤：鼠标聚焦 WorkBuddy → 点击文件管理器窗口 → dock 弹出又马上隐藏。
# 0.1.28 修了"滑入被重排杀死"但用户说还是一样，且滑入前日志一片空白 ——
# 所以这一步先用真实点击把现象复现出来，让带[来源]的诊断日志指出是谁触发的滑入。
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc  # noqa: E402

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
WORK = os.path.join(ROOT, r'build\_verify_appswitch')
LOG = os.path.join(WORK, 'ZDock.log')

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.SetProcessDPIAware()
for fn, arg, res in (
    (u32.EnumWindows, [ctypes.c_void_p, wt.LPARAM], wt.BOOL),
    (u32.GetWindowRect, [wt.HWND, ctypes.POINTER(wt.RECT)], wt.BOOL),
    (u32.SetCursorPos, [ctypes.c_int, ctypes.c_int], wt.BOOL),
    (u32.GetCursorPos, [ctypes.POINTER(wt.POINT)], wt.BOOL),
    (u32.WindowFromPoint, [wt.POINT], wt.HWND),
    (u32.IsWindowVisible, [wt.HWND], wt.BOOL),
    (u32.IsIconic, [wt.HWND], wt.BOOL),
    (u32.SetForegroundWindow, [wt.HWND], wt.BOOL),
    (u32.mouse_event, [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.ULONG], None),
):
    fn.argtypes = arg
    fn.restype = res

EN = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def top_windows():
    """列出可见顶层窗口 (hwnd, cls, title, rect)。"""
    out = []

    def cb(h, _):
        if not u32.IsWindowVisible(h) or u32.IsIconic(h):
            return True
        cls = ctypes.create_unicode_buffer(256)
        title = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cls, 256)
        u32.GetWindowTextW(h, title, 256)
        r = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        out.append((h, cls.value, title.value, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(EN(cb), 0)
    return out


def find(pred):
    for h, cls, title, rc in top_windows():
        if pred(cls, title, rc):
            return (h, cls, title, rc)
    return None


def cursor():
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def real_click(x, y):
    u32.SetCursorPos(int(x), int(y))
    time.sleep(0.08)
    u32.mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    time.sleep(0.05)
    u32.mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP


def dock_top():
    for h, cls, title, rc in top_windows():
        if cls == 'ZDock':
            return rc[1], rc
    return None, None


def run():
    pc.kill_existing_zdock()
    import json
    import shutil
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, r'build\bin\ZDock.exe'), os.path.join(WORK, 'ZDock.exe'))
    # 用用户 _review 里的同款配置（保持 autoHide/slideIn 一致）
    src_cfg = os.path.join(ROOT, r'_review\config.json')
    if os.path.exists(src_cfg):
        shutil.copy2(src_cfg, os.path.join(WORK, 'config.json'))
    subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                     creationflags=0x00000008, close_fds=True)
    time.sleep(2.5)

    # 你正在用鼠标？放弃
    c1 = cursor(); time.sleep(0.25); c2 = cursor(); time.sleep(0.25); c3 = cursor()
    if not (c1 == c2 == c3):
        print('  ⚠ 检测到鼠标正在移动（你正在用），放弃本轮')
        return
    orig = c1

    wb = find(lambda c, t, r: 'WorkBuddy' in t and (r[2] - r[0]) > 300)
    ex = find(lambda c, t, r: c == 'CabinetWClass' and (r[2] - r[0]) > 300)
    print(f'  WorkBuddy 窗口: {wb[2][:40] if wb else None}')
    print(f'  文件管理器窗口: {ex[2][:40] if ex else None}')
    if not wb or not ex:
        print('  [FAIL] 没找到目标窗口（需要 WorkBuddy 和文件管理器都开着）')
        return

    seq = []
    t0 = time.monotonic()

    def sample():
        top, _ = dock_top()
        seq.append((int((time.monotonic() - t0) * 1000), top))

    # ① 聚焦 WorkBuddy（点它）
    wcx, wcy = (wb[3][0] + wb[3][2]) // 2, (wb[3][1] + wb[3][3]) // 2
    real_click(wcx, wcy)
    print(f'  ① 点击 WorkBuddy（{wcx},{wcy}）')
    for _ in range(30):
        sample(); time.sleep(0.02)

    # ② 场景 B：点文件管理器窗口**底部边缘**（用户点窗口让其前台时很常见）
    ecx = (ex[3][0] + ex[3][2]) // 2
    ecy = ex[3][3] - 25
    real_click(ecx, ecy)
    print(f'  ②B 点击文件管理器底边（{ecx},{ecy}）距屏幕底边 {1440 - ecy}px')
    for _ in range(120):
        sample(); time.sleep(0.02)

    # ③ 场景 C：从 WorkBuddy 移向文件管理器，**途经屏幕底边**（热区）
    u32.SetCursorPos(ecx, 1440 - 2)
    time.sleep(0.12)
    u32.SetCursorPos(ecx, ecy)
    time.sleep(0.05)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    print(f'  ③C 途经屏幕底边后点击（{ecx},{ecy}）')
    for _ in range(150):
        sample(); time.sleep(0.02)

    # 还原光标
    u32.SetCursorPos(*orig)

    # 报告
    hidden = max(t for _, t in seq if t is not None)
    shown = min(t for _, t in seq if t is not None)
    moves = [(t, v) for (t, v), (_, pv) in zip(seq[1:], seq)
             if v is not None and pv is not None and v != pv]
    print(f'  dock.top 范围: hidden≈{hidden} shown≈{shown}（跳变 {len(moves)} 次）')
    for t, v in moves[:24]:
        print(f'    {t:5d}ms  top={v}')
    if len(moves) > 24:
        print(f'    …（共 {len(moves)} 次）')

    pc.kill_existing_zdock()


if __name__ == '__main__':
    run()
