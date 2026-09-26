"""精确定位：AppBar 残留到底能不能清，以及什么能清。

关键区分：
 · 同一进程内 DestroyWindow（shell 收到 WM_DESTROY）→ 之前实测**能**恢复
 · 真·强杀（窗口被遗弃）→ 之前实测**不能**恢复
本次实测对比这两条，并试几种清理手段。
"""
import ctypes, json, os, shutil, subprocess, tempfile, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)
SPI_GETWORKAREA = 0x0030
SPI_SETWORKAREA = 0x002F
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

class APPBARDATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("hWnd", wt.HWND), ("uCallbackMessage", wt.UINT),
                ("uEdge", wt.UINT), ("rc", wt.RECT), ("lParam", wt.LPARAM)]
sh32.SHAppBarMessage.argtypes = [wt.DWORD, ctypes.POINTER(APPBARDATA)]
sh32.SHAppBarMessage.restype = ctypes.c_uint64

ABM_NEW=0x00; ABM_REMOVE=0x01; ABM_QUERYPOS=0x02; ABM_SETPOS=0x03
ABM_GETTASKBARPOS=0x05; ABE_BOTTOM=3

def wa():
    r = wt.RECT(); u32.SystemParametersInfoW(SPI_GETWORKAREA,0,ctypes.byref(r),0)
    return (r.left,r.top,r.right,r.bottom)

print('起点:', wa())

# ---- 真·强杀场景：起 ZDock，让它占工作区，taskkill ----
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT,'build','bin','ZDock.exe')
d = tempfile.mkdtemp(prefix='zd_')
shutil.copy2(EXE, os.path.join(d,'ZDock.exe'))
with open(os.path.join(d,'config.json'),'w',encoding='utf-8') as f:
    json.dump({"iconSize":48,"iconGap":12,"hoverScale":1.7,"animMs":150,"bottomMargin":6,
               "bgColor":"#1A1A1ACC","cornerRadius":12,"reserveWorkArea":True,
               "items":[{"path":r"C:\Windows\explorer.exe"}]}, f)
p = subprocess.Popen([os.path.join(d,'ZDock.exe')], cwd=d)
time.sleep(1.8)
print('占用后:', wa())
subprocess.run(['taskkill','/F','/IM','ZDock.exe'], capture_output=True)
time.sleep(1.2)
print('强杀后:', wa())

# ---- 手段 A：新窗口 NEW -> SETPOS(空) -> REMOVE ----
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
wc = WNDCLASS(); wc.lpfnWndProc=proc; wc.lpszClassName='PyPurgeAB'
u32.RegisterClassW(ctypes.byref(wc))

def purge_with_new():
    h = u32.CreateWindowExW(0,'PyPurgeAB','x',0,0,0,10,10,None,None,None,None)
    a = APPBARDATA(); a.cbSize=ctypes.sizeof(APPBARDATA); a.hWnd=h
    a.uCallbackMessage = u32.RegisterWindowMessageW('PyPurgeNotify')
    r_new = sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(a))
    e = APPBARDATA(); e.cbSize=ctypes.sizeof(APPBARDATA); e.hWnd=h; e.uEdge=ABE_BOTTOM
    e.rc = wt.RECT(0,0,0,0)
    sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(e))
    rr = APPBARDATA(); rr.cbSize=ctypes.sizeof(APPBARDATA); rr.hWnd=h
    r_rm = sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(rr))
    u32.DestroyWindow(h)
    return r_new, r_rm

print()
print('手段 A：新窗口 NEW->SETPOS(空)->REMOVE')
print('  返回值:', purge_with_new())
time.sleep(1.2)
print('  之后:', wa())

# ---- 手段 B：SPI_SETWORKAREA 硬设回全屏 ----
print()
print('手段 B：SPI_SETWORKAREA 硬设回 (0,38,2560,1440)')
r = wt.RECT(0,38,2560,1440)
ok = u32.SystemParametersInfoW(SPI_SETWORKAREA,0,ctypes.byref(r),0)
print('  ok =', ok)
time.sleep(0.8)
print('  之后:', wa())

shutil.rmtree(d, ignore_errors=True)
