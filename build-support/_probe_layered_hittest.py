"""验证：alpha=0 的 layered 窗口能不能被鼠标命中（WindowFromPoint）。

背景
----
MSDN 关于 `WS_EX_LAYERED`：
  "Hit testing of a layered window is based on the shape and transparency of
   the window. This means that the areas of the window that are color-keyed or
   **whose alpha value is zero will let the mouse messages through**."

ZDock 的贴边热区（EdgeHotZone）用 `SetLayeredWindowAttributes(hwnd,0,0,LWA_ALPHA)`
把 alpha 设成 **0** —— 若上述成立，热区**根本收不到真实鼠标**，自动隐藏就永远唤不出来。
（此前的探针是靠 `PostMessage` 直接注入热区 hwnd，绕过了系统命中测试，因此测不出这个坑。）

本探针只做机制验证：自建窗口，不动 ZDock、不动用户鼠标（只短暂显示 0.6 秒）。

判据：
  alpha=0 → WindowFromPoint(窗口中心) **不该**返回该窗口
  alpha=1 → WindowFromPoint(窗口中心) **应该**返回该窗口

跑法： <python> build-support/_probe_layered_hittest.py
"""

import ctypes
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [('cbSize', wt.UINT), ('style', wt.UINT), ('lpfnWndProc', WNDPROC),
                ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON), ('hCursor', wt.HANDLE),
                ('hbrBackground', wt.HBRUSH), ('lpszMenuName', wt.LPCWSTR),
                ('lpszClassName', wt.LPCWSTR), ('hIconSm', wt.HICON)]


u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.DefWindowProcW.restype = ctypes.c_longlong
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u32.CreateWindowExW.restype = wt.HWND
u32.SetLayeredWindowAttributes.argtypes = [wt.HWND, wt.DWORD, ctypes.c_ubyte, wt.DWORD]
u32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u32.DestroyWindow.argtypes = [wt.HWND]
u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_LAYERED = 0x00080000
LWA_ALPHA = 0x00000002
SW_SHOWNOACTIVATE = 4

_hinst = k32.GetModuleHandleW(None)


def _wndproc(hwnd, msg, wp, lp):
    return u32.DefWindowProcW(hwnd, msg, wp, lp)


_proc = WNDPROC(_wndproc)   # 必须活着，否则回调被 GC


def make_layered(cls_name, x, y, w, h, alpha):
    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
    wc.lpfnWndProc = _proc
    wc.hInstance = _hinst
    wc.lpszClassName = cls_name
    if not u32.RegisterClassExW(ctypes.byref(wc)) and ctypes.get_last_error() != 1410:
        return None
    hwnd = u32.CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        cls_name, 'probe', 0x80000000,  # WS_POPUP
        x, y, w, h, None, None, _hinst, None)
    if not hwnd:
        return None
    u32.SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)
    u32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
    return hwnd


def cls_of(hwnd):
    if not hwnd:
        return '(null)'
    b = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, b, 256)
    return b.value or '(?)'


# 放在屏幕右上角一块不常被点的区域，测完立刻销毁
X, Y, W, H = 1400, 40, 120, 120

results = {}
for alpha in (0, 1):
    cls = f'ZDockProbeLayer{alpha}'
    hwnd = make_layered(cls, X, Y, W, H, alpha)
    if not hwnd:
        print(f'alpha={alpha}: CreateWindow 失败 err={ctypes.get_last_error()}')
        continue
    pt = wt.POINT(X + W // 2, Y + H // 2)
    hit = u32.WindowFromPoint(pt)
    results[alpha] = (hwnd, hit)
    print(f'alpha={alpha}: 窗口 hwnd={hwnd}  中心命中 -> hwnd={hit} 类名={cls_of(hit)!r}  '
          f'{"命中自己 ✅" if hit == hwnd else "穿透了 ❌"}')
    u32.DestroyWindow(hwnd)

print('=' * 70)
ok0 = results.get(0) and results[0][1] != results[0][0]
ok1 = results.get(1) and results[1][1] == results[1][0]
print(f'  alpha=0 会穿透（不应命中） : {"确认" if ok0 else "未复现"}')
print(f'  alpha=1 可命中             : {"确认" if ok1 else "未复现"}')
