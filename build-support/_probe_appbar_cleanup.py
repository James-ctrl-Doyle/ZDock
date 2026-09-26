"""关键实测：残留的 AppBar 工作区占用，能不能被'重注册 + 注销'或别的手段清掉。

流程：
  1) 记录干净工作区
  2) 直接 SHAppBarMessage 注册一个 AppBar 并 SETPOS（占 95px）
  3) 不注销，直接放弃（模拟"进程被强杀"）—— 但我们是同进程，所以记录 hwnd 后丢掉引用
  4) 观察工作区
  5) 尝试用各种办法恢复：ABM_REMOVE（同一 hwnd）、重新注册再 REMOVE、ABM_SETPOS 成空
"""
import ctypes, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)

SPI_GETWORKAREA = 0x0030
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

ABM_NEW = 0x00
ABM_REMOVE = 0x01
ABM_QUERYPOS = 0x02
ABM_SETPOS = 0x03
ABM_GETTASKBARPOS = 0x05
ABM_WINDOWPOSCHANGED = 0x09
ABE_BOTTOM = 3

class APPBARDATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("hWnd", wt.HWND),
                ("uCallbackMessage", wt.UINT),
                ("uEdge", wt.UINT),
                ("rc", wt.RECT),
                ("lParam", wt.LPARAM)]

sh32.SHAppBarMessage.argtypes = [wt.DWORD, ctypes.POINTER(APPBARDATA)]
sh32.SHAppBarMessage.restype = ctypes.c_uint64

def wa():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

# 自建一个顶层窗口当 appbar owner
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
def _wp(h, m, w, l):
    return u32.DefWindowProcW(h, m, w, l)
proc = WNDPROC(_wp)

class WNDCLASS(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON), ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HBRUSH), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]

u32.CreateWindowExW.restype = wt.HWND
u32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASS)]

wc = WNDCLASS()
wc.lpfnWndProc = proc
wc.lpszClassName = 'PyTestAppBar'
u32.RegisterClassW(ctypes.byref(wc))

def make_appbar():
    h = u32.CreateWindowExW(0, 'PyTestAppBar', 'x', 0, 0, 0, 10, 10, None, None, None, None)
    return h

print('干净工作区:', wa())

h1 = make_appbar()
print('hwnd#1 =', h1)
abd = APPBARDATA()
abd.cbSize = ctypes.sizeof(APPBARDATA)
abd.hWnd = h1
abd.uCallbackMessage = u32.RegisterWindowMessageW('PyTestAppBarNotify')
print('ABM_NEW ->', sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(abd)))
abd.uEdge = ABE_BOTTOM
abd.rc = wt.RECT(1084, 1345, 1476, 1440)
print('ABM_QUERYPOS ->', sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(abd)), abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom)
abd.rc = wt.RECT(1084, 1345, 1476, 1440)
print('ABM_SETPOS ->', sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(abd)), abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom)
time.sleep(0.6)
print('注册后工作区:', wa())

print()
print('=== 模拟"强杀"：直接 DestroyWindow 而不 ABM_REMOVE ===')
u32.DestroyWindow(h1)
time.sleep(1.5)
print('destroy 后工作区:', wa())

print()
print('=== 尝试 1：让一个新 AppBar 走一遍完整流程（SETPOS 后 REMOVE） ===')
h2 = make_appbar()
abd2 = APPBARDATA()
abd2.cbSize = ctypes.sizeof(APPBARDATA)
abd2.hWnd = h2
abd2.uCallbackMessage = u32.RegisterWindowMessageW('PyTestAppBarNotify2')
print('ABM_NEW ->', sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(abd2)))
abd2.uEdge = ABE_BOTTOM
abd2.rc = wt.RECT(0, 0, 0, 0)
sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(abd2))
abd2.rc = wt.RECT(0, 0, 0, 0)
print('ABM_SETPOS(0) ->', sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(abd2)))
print('ABM_REMOVE ->', sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(abd2)))
time.sleep(1.2)
print('尝试 1 后工作区:', wa())
u32.DestroyWindow(h2)
time.sleep(0.8)
print('再等一下:', wa())
