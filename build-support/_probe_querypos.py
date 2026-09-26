"""拆解 ABM_QUERYPOS / ABM_SETPOS 的语义：看 shell 到底怎么改 rc。
在干净工作区上跑，结束后完整注销 + 复位工作区。
"""
import ctypes, os, shutil, subprocess, time
from ctypes import wintypes as wt
u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

class APPBARDATA(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('hWnd', wt.HWND), ('uCallbackMessage', wt.UINT),
                ('uEdge', wt.UINT), ('rc', wt.RECT), ('lParam', wt.LPARAM)]
ABM_NEW, ABM_REMOVE, ABM_QUERYPOS, ABM_SETPOS, ABM_GETTASKBARPOS = 0, 1, 2, 3, 5
ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM = 0, 1, 2, 3
SPI_GETWORKAREA, SPI_SETWORKAREA = 0x0030, 0x002F

def wa():
    r = wt.RECT(); u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

# --- 建一个真实窗口做 AppBar 宿主 ---
hInst = k32.GetModuleHandleW(None)
CW = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
u32.DefWindowProcW.restype = ctypes.c_longlong
def wndproc(h, m, w, l):
    return u32.DefWindowProcW(h, m, w, l)
cb = CW(wndproc)
WC = type('WC', (ctypes.Structure,), {'_fields_': [
    ('style', wt.UINT), ('lpfnWndProc', CW), ('cbClsExtra', ctypes.c_int),
    ('cbWndExtra', ctypes.c_int), ('hInstance', wt.HINSTANCE), ('hIcon', wt.HANDLE),
    ('hCursor', wt.HANDLE), ('hbrBackground', wt.HANDLE), ('lpszMenuName', wt.LPCWSTR),
    ('lpszClassName', wt.LPCWSTR)]})
u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
wc = WC(); wc.lpfnWndProc = cb; wc.hInstance = hInst; wc.lpszClassName = 'ZQTest'
u32.RegisterClassExW(ctypes.byref(wc))
u32.CreateWindowExW.restype = wt.HWND
hwnd = u32.CreateWindowExW(0, 'ZQTest', 't', 0, 0, 0, 0, 0, None, None, hInst, None)
print('测试窗口 hwnd =', hwnd, ' 当前工作区 =', wa())

msg = u32.RegisterWindowMessageW('ZQTestNotify')
abd = APPBARDATA(); abd.cbSize = ctypes.sizeof(APPBARDATA); abd.hWnd = hwnd
abd.uCallbackMessage = msg
print('ABM_NEW ->', sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(abd)))

# 模拟 ZDock 的 desired：底边 95px 厚，宽 615 居中
mon = (0, 38, 2560, 1440)
desired = wt.RECT(973, 1345, 1588, 1440)   # 贴底，厚 95
print('\ndesired(送进 QUERYPOS) =', (desired.left, desired.top, desired.right, desired.bottom))

abd.uEdge = ABE_BOTTOM
abd.rc = wt.RECT(desired.left, desired.top, desired.right, desired.bottom)
r = sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(abd))
print('ABM_QUERYPOS ->', r, ' rc =', (abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom))

thick = desired.bottom - desired.top
abd.rc.top = abd.rc.bottom - thick
print('  我方修正: rc.top = bottom - %d -> rc =' % thick, (abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom))
r = sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(abd))
print('ABM_SETPOS ->', r, ' rc =', (abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom))
time.sleep(0.6)
print('  ==> 工作区 =', wa())

# 注销
e = APPBARDATA(); e.cbSize = ctypes.sizeof(APPBARDATA); e.hWnd = hwnd
e.uEdge = ABE_BOTTOM; e.rc = wt.RECT(0, 0, 0, 0)
sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(e))
d = APPBARDATA(); d.cbSize = ctypes.sizeof(APPBARDATA); d.hWnd = hwnd
sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(d))
time.sleep(0.6)
print('\n注销后工作区 =', wa())
u32.DestroyWindow(hwnd)
