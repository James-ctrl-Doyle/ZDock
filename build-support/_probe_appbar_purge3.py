"""验证：purgeStale 后 sleep 一段时间再注册，能否得到干净的结果。

判据：干净起点 1440；占 95 → 1345。
"""
import ctypes, json, os, shutil, subprocess, tempfile, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)
SPI_GETWORKAREA = 0x0030
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

class APPBARDATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("hWnd", wt.HWND), ("uCallbackMessage", wt.UINT),
                ("uEdge", wt.UINT), ("rc", wt.RECT), ("lParam", wt.LPARAM)]
sh32.SHAppBarMessage.argtypes = [wt.DWORD, ctypes.POINTER(APPBARDATA)]
sh32.SHAppBarMessage.restype = ctypes.c_uint64
ABM_NEW=0x00; ABM_REMOVE=0x01; ABM_QUERYPOS=0x02; ABM_SETPOS=0x03; ABE_BOTTOM=3

def wa():
    r = wt.RECT(); u32.SystemParametersInfoW(SPI_GETWORKAREA,0,ctypes.byref(r),0)
    return (r.left,r.top,r.right,r.bottom)

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, ctypes.c_ulonglong, ctypes.c_longlong)
def _wp(h,m,w,l):
    return u32.DefWindowProcW(h,m,w,ctypes.c_void_p(l))
proc = WNDPROC(_wp)
class WNDCLASS(ctypes.Structure):
    _fields_=[("style",wt.UINT),("lpfnWndProc",WNDPROC),("cbClsExtra",ctypes.c_int),
              ("cbWndExtra",ctypes.c_int),("hInstance",wt.HINSTANCE),("hIcon",wt.HICON),
              ("hCursor",wt.HANDLE),("hbrBackground",wt.HBRUSH),
              ("lpszMenuName",wt.LPCWSTR),("lpszClassName",wt.LPCWSTR)]
u32.CreateWindowExW.restype = wt.HWND
wc = WNDCLASS(); wc.lpfnWndProc=proc; wc.lpszClassName='PyP2'
u32.RegisterClassW(ctypes.byref(wc))
def mk():
    return u32.CreateWindowExW(0,'PyP2','x',0,0,0,10,10,None,None,None,None)

def purge():
    h = mk()
    a = APPBARDATA(); a.cbSize=ctypes.sizeof(APPBARDATA); a.hWnd=h
    a.uCallbackMessage = u32.RegisterWindowMessageW('PyP2N')
    sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(a))
    e = APPBARDATA(); e.cbSize=ctypes.sizeof(APPBARDATA); e.hWnd=h; e.uEdge=ABE_BOTTOM
    e.rc = wt.RECT(0,0,0,0)
    sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(e))
    rr = APPBARDATA(); rr.cbSize=ctypes.sizeof(APPBARDATA); rr.hWnd=h
    sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(rr))
    u32.DestroyWindow(h)

def occupy(h, thick=95):
    a = APPBARDATA(); a.cbSize=ctypes.sizeof(APPBARDATA); a.hWnd=h
    a.uCallbackMessage = u32.RegisterWindowMessageW('PyP2N')
    sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(a))
    q = APPBARDATA(); q.cbSize=ctypes.sizeof(APPBARDATA); q.hWnd=h; q.uEdge=ABE_BOTTOM
    q.rc = wt.RECT(1084, 1440-thick, 1476, 1440)
    sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(q))
    q.rc = wt.RECT(1084, 1440-thick, 1476, 1440)
    sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(q))

print('起点:', wa())

# 造一个残留：注册后再 DestroyWindow（不 REMOVE）
h_dead = mk()
occ_dead = APPBARDATA(); occ_dead.cbSize=ctypes.sizeof(APPBARDATA); occ_dead.hWnd=h_dead
occ_dead.uCallbackMessage = u32.RegisterWindowMessageW('PyP2N')
sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(occ_dead))
q = APPBARDATA(); q.cbSize=ctypes.sizeof(APPBARDATA); q.hWnd=h_dead; q.uEdge=ABE_BOTTOM
q.rc = wt.RECT(1084, 1345, 1476, 1440)
sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(q))
q.rc = wt.RECT(1084, 1345, 1476, 1440)
sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(q))
time.sleep(0.5)
print('占用后:', wa())
u32.DestroyWindow(h_dead)
time.sleep(1.0)
print('模拟残留(销毁窗口但不 REMOVE):', wa())

print()
print('--- 方案：purge 后 sleep 0.4s 再注册 ---')
purge()
time.sleep(0.4)
print('purge 后:', wa())
h_mine = mk()
occ = APPBARDATA(); occ.cbSize=ctypes.sizeof(APPBARDATA); occ.hWnd=h_mine
occ.uCallbackMessage = u32.RegisterWindowMessageW('PyP2N')
sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(occ))
q2 = APPBARDATA(); q2.cbSize=ctypes.sizeof(APPBARDATA); q2.hWnd=h_mine; q2.uEdge=ABE_BOTTOM
q2.rc = wt.RECT(1084, 1345, 1476, 1440)
sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(q2))
q2.rc = wt.RECT(1084, 1345, 1476, 1440)
sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(q2))
time.sleep(0.8)
print('我注册后:', wa(), '（期望 1345 = 干净 1440 - 95）')

# 收尾
rm = APPBARDATA(); rm.cbSize=ctypes.sizeof(APPBARDATA); rm.hWnd=h_mine
sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(rm))
u32.DestroyWindow(h_mine)
time.sleep(0.6)
print('最终:', wa())
