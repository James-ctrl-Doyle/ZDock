# 决定性实验：复现「隐藏 dock 被无声唤出」的完整链路。
#
# 链路（2026-10-14 代码走查 + Ling 源码确认）：
#   切换应用 → tracker.onChanged → syncWithTracker → refreshIndicators()（无条件）
#   → placeIndicators() → IndicatorNode::setPosition → Node::setPosition → win->refresh()
#   → WM_PAINT → WinBase::paint() → layout() → refreshHover() → onMouseMove(lastMousePos)
#   → ZDock 的 onMouseMove 处理器**不看坐标**直接 keepVisible() → slideIn()
#   → 隐藏的 dock 被唤出（光标根本不在 dock 上）→ 200ms 后又收起。
#
# 判据：dock 隐藏 + 光标不在 dock 上 + 触发一次分组变化（建一个可收录的窗口）
#       → dock **必须保持隐藏**。修前会弹出来。
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

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE = os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_relayoutwake')
LOG = os.path.join(WORK, 'ZDock.log')

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
u32.SetProcessDPIAware()
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HWND, wt.UINT,
                             ctypes.c_void_p, ctypes.c_void_p)
EN = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [('cbSize', wt.UINT), ('style', wt.UINT), ('lpfnWndProc', ctypes.c_void_p),
                ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                ('hInstance', wt.HINSTANCE), ('hIcon', wt.HANDLE),
                ('hCursor', wt.HANDLE), ('hbrBackground', wt.HANDLE),
                ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR),
                ('hIconSm', wt.HANDLE)]


def find_zdock():
    out = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cls, 256)
        if cls.value == 'ZDock':
            r = wt.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(EN(cb), 0)
    return out[0] if out else None


def make_trackable_window():
    """建一个**会被 tracker 收录**的窗口（触发分组变化 → syncWithTracker → 重排）。
    样式抄 _verify_slidecut.py（那个探针的窗口确认能被收录）。"""
    hinst = ctypes.WinDLL('kernel32').GetModuleHandleW(None)
    cls = 'ZDockRelayoutWakeProbe'

    @WNDPROC
    def wndproc(h, m, wp, lp):
        u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT,
                                       ctypes.c_void_p, ctypes.c_void_p]
        u32.DefWindowProcW.restype = ctypes.c_void_p
        return u32.DefWindowProcW(h, m, wp, lp)

    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(wc)
    wc.lpfnWndProc = ctypes.cast(wndproc, ctypes.c_void_p)
    wc.hInstance = hinst
    wc.lpszClassName = cls
    u32.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
    u32.RegisterClassExW.restype = wt.ATOM
    u32.RegisterClassExW(ctypes.byref(wc))
    u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
    u32.CreateWindowExW.restype = wt.HWND
    h = u32.CreateWindowExW(0, cls, '重排唤出探针', 0x00CF0000,
                            120, 120, 360, 240, None, None, hinst, None)
    if h:
        u32.ShowWindow(h, 4)   # SW_SHOWNOACTIVATE
    return h


def run():
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(EXE, os.path.join(WORK, 'ZDock.exe'))
    cfg = {
        "autoHide": True, "autoHideDelayMs": 200,
        "slideInMs": 150, "slideOutMs": 200,
        "hotZoneEnabled": True, "hotZoneThickness": 4,
        "showIndicator": False,
        "items": [{"path": "C:\\Windows\\explorer.exe", "name": "文件资源管理器"}],
    }
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(cfg, f, ensure_ascii=False)
    subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                     creationflags=0x00000008, close_fds=True)
    time.sleep(2.5)

    d = find_zdock()
    hidden_top = d[1][1] if d else None
    print(f'  起始 dock.top = {hidden_top}（应为隐藏位）')

    # 记录光标（不动它）—— 判据要求"光标不在 dock 上"
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    print(f'  当前光标 = ({p.x},{p.y})（不在 dock 上即可）')

    # ① 建可收录窗口 → 分组变化 → syncWithTracker → refreshIndicators → placeIndicators
    #    → setPosition → refresh → WM_PAINT → layout → refreshHover → onMouseMove → keepVisible?
    app = make_trackable_window()
    print(f'  探针窗口 hwnd = {app}')

    # ② 高频采样 dock 位置（抓那一瞬的弹出）
    seq = []
    t0 = time.monotonic()
    for _ in range(200):
        dd = find_zdock()
        if dd:
            seq.append((int((time.monotonic() - t0) * 1000), dd[1][1]))
        time.sleep(0.006)

    if app:
        u32.DestroyWindow(app)

    moved = [(t, v) for (t, v), (_, pv) in zip(seq[1:], seq) if v != pv]
    print(f'  采样 {len(seq)} 点 / 位置变化 {len(moved)} 次')
    for t, v in moved[:20]:
        print(f'    {t:5d}ms  top={v}')
    print(f'  最终 top = {seq[-1][1] if seq else None} / 隐藏位 = {hidden_top}')

    log = ''
    if os.path.exists(LOG):
        import io
        log = io.open(LOG, encoding='utf-8', errors='ignore').read()
    for line in log.splitlines():
        if '滑入开始' in line or '分组变化' in line:
            print('  日志:', line)

    stayed = seq and all(v >= hidden_top - 1 for _, v in seq)
    print(f'  [{"PASS" if stayed else "FAIL"}] 分组变化期间 dock 保持隐藏'
          f'（最小 top = {min((v for _, v in seq), default=None)}）')
    pc.kill_existing_zdock()


if __name__ == '__main__':
    run()
