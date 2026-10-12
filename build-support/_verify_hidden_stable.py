"""验证「切换应用时隐藏的 dock 会上浮」已修（0.1.23）。

链路：切换应用 → 分组变化 → syncWithTracker → relayoutForItemCount（内部
`applyDockPlacement(0)` 摆到**展开位**）→ 隐藏的 dock 跳到屏幕上。
修正原来只写在 relayoutForEnvironment 里，syncWithTracker 那条路径漏了。

这个脚本不碰用户的程序：自己建一个**普通顶层窗口**当"新启动的应用"
（tracker 会收录它 → 触发分组变化），1.5 秒后自毁。

判据：
  H1) 创建窗口（模拟启动应用）后，隐藏的 dock **仍然贴在屏幕外**
  H2) 销毁窗口（模拟退出应用）后，同样仍然隐藏
  H3) 顺带抓一张 dock 展开时的截图，供人工核对图标是否撕裂

⚠ 不移动鼠标、不抢焦点（窗口带 WS_EX_NOACTIVATE）。
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
import _probe_common as pc          # noqa: E402
import _shot_stage4 as shot         # noqa: E402

_ap = argparse.ArgumentParser()
_ap.add_argument('--exe', default=None)
_args = _ap.parse_args()

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
try:
    u32.SetProcessDPIAware()
except Exception:
    pass

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.DefWindowProcW.restype = ctypes.c_longlong
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u32.CreateWindowExW.restype = wt.HWND
u32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u32.DestroyWindow.argtypes = [wt.HWND]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SetForegroundWindow.argtypes = [wt.HWND]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE = _args.exe or os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_hidden')
OUT = os.path.join(ROOT, r'build\_review')
WM_MOUSEMOVE = 0x0200
print(f'被测 exe：{EXE}')
u32.RegisterClassExW.argtypes = [ctypes.c_void_p]

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

_hinst = k32.GetModuleHandleW(None)
_proc = WNDPROC(lambda h, m, w, l: u32.DefWindowProcW(h, m, w, l))

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

    u32.EnumWindows(ctypes.cast(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb),
                                ctypes.c_void_p), 0)
    return out


def make_app_window(cls, title, x, y, w, h):
    """造一个普通顶层窗口（不抢焦点），让 tracker 把它当成"刚启动的应用"。"""
    class WC(ctypes.Structure):
        _fields_ = [('cbSize', wt.UINT), ('style', wt.UINT), ('lpfnWndProc', WNDPROC),
                    ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                    ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON), ('hCursor', wt.HANDLE),
                    ('hbrBackground', wt.HBRUSH), ('lpszMenuName', wt.LPCWSTR),
                    ('lpszClassName', wt.LPCWSTR), ('hIconSm', wt.HICON)]
    wc = WC()
    wc.cbSize = ctypes.sizeof(WC)
    wc.lpfnWndProc = _proc
    wc.hInstance = _hinst
    wc.lpszClassName = cls
    if not u32.RegisterClassExW(ctypes.byref(wc)) and ctypes.get_last_error() != 1410:
        return None
    # WS_OVERLAPPEDWINDOW，但不激活
    return u32.CreateWindowExW(0x08000000, cls, title, 0x00CF0000,
                               x, y, w, h, None, None, _hinst, None)


# ---------------- 启动 dock ----------------
pc.kill_existing_zdock()
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
shutil.copy2(EXE, os.path.join(WORK, 'ZDock.exe'))
with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
    json.dump(CFG, f, ensure_ascii=False)
subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                 creationflags=0x00000008, close_fds=True)
time.sleep(2.2)

hz = windows_of('ZDock.EdgeHotZone')
if not hz:
    print('!! 没找到热区窗口')
    pc.kill_existing_zdock()
    sys.exit(1)
bottom = hz[0][1][3]
print(f'  屏幕底边 = {bottom}')

d = windows_of('ZDock')
base_top = d[0][1][1] if d else None
check('H0) 启动后 dock 在隐藏位', base_top is not None and base_top >= bottom - 4,
      f'dock.top={base_top}')

def sample_min_top(seconds, step=0.04):
    """在 seconds 内持续采样 dock.top，返回 (最小 top, 采样数)。

    ⚠ 必须**连续采样**：dock 被摆回展开位后，只要光标不在 dock 上，
      200ms 内就会被 scheduleHide 收回去 —— 固定 sleep 再看是看不到那一瞬的。
    """
    mn, n = None, 0
    end = time.time() + seconds
    while time.time() < end:
        d = windows_of('ZDock')
        if d:
            t = d[0][1][1]
            mn = t if mn is None else min(mn, t)
            n += 1
        time.sleep(step)
    return mn, n


# ---------------- 模拟"切换/启动应用" ----------------
app = make_app_window('ZDockProbeApp', 'ZDock 测试窗口（会自动关闭）', 40, 40, 360, 220)
if app:
    u32.ShowWindow(app, 4)     # SW_SHOWNOACTIVATE
top1, n1 = sample_min_top(2.0)

d = windows_of('ZDock')
check('H1) 启动应用（分组变化）后隐藏的 dock **没有上浮**',
      top1 is not None and top1 >= bottom - 4,
      f'期间最小 dock.top={top1}（屏幕底边 {bottom}，采样 {n1} 次）')

# ---------------- 模拟"退出应用" ----------------
if app:
    u32.DestroyWindow(app)
top2, n2 = sample_min_top(2.0)

check('H2) 退出应用后仍然隐藏', top2 is not None and top2 >= bottom - 4,
      f'期间最小 dock.top={top2}（采样 {n2} 次）')

# ---------------- 抓一张展开态截图（查图标撕裂） ----------------
hz = windows_of('ZDock.EdgeHotZone')
if hz:
    r = hz[0][1]
    u32.PostMessageW(hz[0][0], WM_MOUSEMOVE, 0, 0)   # 触发展开（不移动鼠标）
    time.sleep(0.5)
    dw = windows_of('ZDock')
    if dw:
        data, W, H = shot.grab_printwindow(dw[0][0])
        if data:
            shot.write_png(os.path.join(OUT, 'verify_dock_icons.png'), W, H, data)
            print(f'  已抓图 verify_dock_icons.png ({W}x{H})')
            check('H3) 抓到展开态截图（供人工核对图标）', True, f'{W}x{H}')
        else:
            check('H3) 抓到展开态截图（供人工核对图标）', False, 'PrintWindow 失败')
    else:
        check('H3) 抓到展开态截图（供人工核对图标）', False, '没找到 dock 窗口')

pc.kill_existing_zdock()

print()
print('=' * 70)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, dd in results:
    if not ok:
        print(f'  FAIL: {n}   {dd}')
print('=' * 70)
