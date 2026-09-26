"""诊断：shell 里现在有没有残留的 AppBar 记录？单份占用是多少？
只读探测，不改任何全局状态（除非 --clean）。
"""
import ctypes, sys, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

SPI_GETWORKAREA, SPI_SETWORKAREA = 0x0030, 0x002F

def wa():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

print('当前工作区 =', wa())

# 主监视器
u32.MonitorFromPoint.argtypes = [wt.POINT, wt.DWORD]
u32.MonitorFromPoint.restype = wt.HANDLE
u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]
MI = type('MI', (ctypes.Structure,), {'_fields_': [
    ('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
mi = MI(); mi.cbSize = ctypes.sizeof(MI)
if u32.GetMonitorInfoW(u32.MonitorFromPoint(wt.POINT(0, 0), 1), ctypes.byref(mi)):
    print('主监视器 rcMonitor =', (mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom))
    print('主监视器 rcWork    =', (mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom))

# 枚举 AppBar? 没有公开 API。用 SHAppBarMessage(ABM_GETTASKBARPOS) 看任务栏
class APPBARDATA(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('hWnd', wt.HWND), ('uCallbackMessage', wt.UINT),
                ('uEdge', wt.UINT), ('rc', wt.RECT), ('lParam', wt.LPARAM)]
abd = APPBARDATA(); abd.cbSize = ctypes.sizeof(APPBARDATA)
ABM_GETTASKBARPOS = 5
ok = sh32.SHAppBarMessage(ABM_GETTASKBARPOS, ctypes.byref(abd))
print('ABM_GETTASKBARPOS ->', ok, 'edge=%d rect=(%d,%d)-(%d,%d)' % (
    abd.uEdge, abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom))

if '--clean' in sys.argv:
    r = mi.rcMonitor; r.top = 38
    u32.SystemParametersInfoW(SPI_SETWORKAREA, 0, ctypes.byref(r), 1)
    time.sleep(0.5)
    print('已 SETWORKAREA 到', (r.left, r.top, r.right, r.bottom), '-> 现在 =', wa())
