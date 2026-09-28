"""ZDock 阶段一 gate 验证。

跑法： <python> build-support/zdock_stage1_test.py [--exe <路径>]
不带参数时用 build/bin/ZDock.exe。

阶段一的 gate 只有三条（任务书 §13）：
  1) 红线自检   —— 窗口是独立顶层窗口；启动前后 explorer 的 WorkerW 计数不变
  2) 鼠标穿透   —— halo 透明区的真实点击落到下层窗口；面板实体的点击不穿透
  3) 性能       —— 空闲 10s CPU 增量 ≈ 0；悬停扫动期间 CPU 远低于 5%/核

另外顺手验证位置（贴工作区底边居中）与渲染（PrintWindow 截图，空闲/悬停两图必须不同，
证明确实发生了放大动画）。

⚠ 会用 SetCursorPos 真实移动鼠标（约 20 秒），结束前还原原位。
⚠ 截图一律 PrintWindow(hwnd, hdc, 2)，绝不 BitBlt 桌面 —— 那会拍到用户自己的窗口内容。
"""

import argparse
import ctypes
import os
import struct
import subprocess
import sys
import time
import zlib
from ctypes import wintypes as wt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
OUT_DIR = os.path.join(ROOT, 'build', '_review')

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)

# ---------------------------------------------------------------- Win32 声明
# ⚠ 每个函数都要给 restype/argtypes：64 位下句柄会被截断、样式常量会 OverflowError；
#   而且未声明时这类错误只在回调触发时才以 "Exception ignored" 刷出来，
#   第一眼很像被测程序崩了。
u32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
u32.EnumWindows.restype = wt.BOOL
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetClassNameW.restype = ctypes.c_int
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.restype = ctypes.c_int
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRect.restype = wt.BOOL
u32.GetParent.argtypes = [wt.HWND]
u32.GetParent.restype = wt.HWND
u32.GetAncestor.argtypes = [wt.HWND, ctypes.c_uint]
u32.GetAncestor.restype = wt.HWND
u32.GetDesktopWindow.argtypes = []
u32.GetDesktopWindow.restype = wt.HWND
u32.GetShellWindow.argtypes = []
u32.GetShellWindow.restype = wt.HWND
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.IsWindowVisible.restype = wt.BOOL
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowThreadProcessId.restype = wt.DWORD
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
u32.PrintWindow.restype = wt.BOOL
u32.GetWindowDC.argtypes = [wt.HWND]
u32.GetWindowDC.restype = wt.HDC
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.ReleaseDC.restype = ctypes.c_int
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.GetCursorPos.restype = wt.BOOL
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.SetCursorPos.restype = wt.BOOL
u32.SystemParametersInfoW.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint]
u32.SystemParametersInfoW.restype = wt.BOOL
u32.GetSystemMetrics.argtypes = [ctypes.c_int]
u32.GetSystemMetrics.restype = ctypes.c_int
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.DestroyWindow.argtypes = [wt.HWND]
u32.DestroyWindow.restype = wt.BOOL
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
u32.CreateWindowExW.restype = wt.HWND
u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
u32.RegisterClassExW.restype = wt.ATOM
u32.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.DefWindowProcW.restype = ctypes.c_ssize_t
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.mouse_event.restype = None
u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.SetWindowPos.restype = wt.BOOL
u32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
u32.PeekMessageW.restype = wt.BOOL
u32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
u32.TranslateMessage.restype = wt.BOOL
u32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
u32.DispatchMessageW.restype = ctypes.c_ssize_t

PM_REMOVE = 1


def pump(seconds=0.5):
    """抽干并派发本线程的消息队列。

    ⚠ 没有这一步，测试窗的 WNDPROC 根本不会被调用 —— 鼠标消息（WM_MOUSEMOVE /
      WM_LBUTTONDOWN）和 PostMessage 都是**排队消息**，必须有 GetMessage/DispatchMessage
      才会送达窗口过程。踩过：以为"注入点击不生效"，其实是脚本自己没泵消息；
      WindowFromPoint 是同步命中测试所以一直是对的，误导了排查方向。
    """
    end = time.time() + seconds
    msg = wt.MSG()
    while time.time() < end:
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            u32.TranslateMessage(ctypes.byref(msg))
            u32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.01)

g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g32.CreateCompatibleBitmap.restype = wt.HBITMAP
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.SelectObject.restype = wt.HGDIOBJ
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteObject.restype = wt.BOOL
g32.DeleteDC.argtypes = [wt.HDC]
g32.DeleteDC.restype = wt.BOOL
g32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                          ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
g32.GetDIBits.restype = ctypes.c_int
g32.CreateSolidBrush.argtypes = [wt.COLORREF]
g32.CreateSolidBrush.restype = wt.HBRUSH
g32.CreateRectRgn.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
g32.CreateRectRgn.restype = wt.HANDLE
g32.CombineRgn.argtypes = [wt.HANDLE, wt.HANDLE, wt.HANDLE, ctypes.c_int]
g32.CombineRgn.restype = ctypes.c_int
u32.SetWindowRgn.argtypes = [wt.HWND, wt.HANDLE, wt.BOOL]
u32.SetWindowRgn.restype = ctypes.c_int

k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
k32.GetModuleHandleW.restype = wt.HMODULE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.GetProcessTimes.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
k32.GetProcessTimes.restype = wt.BOOL

_get_long = getattr(u32, 'GetWindowLongPtrW', None) or u32.GetWindowLongW
_get_long.argtypes = [wt.HWND, ctypes.c_int]
_get_long.restype = ctypes.c_ssize_t

GWL_EXSTYLE = -20
GWL_HWNDPARENT = -8
GA_PARENT = 1
SPI_GETWORKAREA = 0x0030
SM_CXSCREEN, SM_CYSCREEN = 0, 1

WS_EX_TOPMOST = 0x00000008
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_EX_LAYERED = 0x00080000
WS_EX_NOACTIVATE = 0x08000000
WS_VISIBLE = 0x10000000
WS_POPUP = 0x80000000

ME_LEFTDOWN, ME_LEFTUP = 0x0002, 0x0004
WM_LBUTTONDOWN = 0x0201
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(('  [PASS] ' if ok else '  [FAIL] ') + name + (('   ' + detail) if detail else ''))
    return bool(ok)


def info(text):
    print('  .. ' + text)


# ---------------------------------------------------------------- 窗口枚举工具
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def enum_windows():
    found = []

    def cb(hwnd, _):
        found.append(hwnd)
        return True

    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def class_of(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def rect_of(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def pid_of(hwnd):
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def find_dock_window(timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        for hwnd in enum_windows():
            if class_of(hwnd) == 'ZDock' and u32.IsWindowVisible(hwnd):
                return hwnd
        time.sleep(0.15)
    return None


def shell_chain_counts():
    """explorer 拥有的 Progman / WorkerW / SHELLDLL_DefView 计数（红线判据）。"""
    shell = u32.GetShellWindow()
    shell_pid = pid_of(shell) if shell else 0
    counts = {}
    for hwnd in enum_windows():
        if pid_of(hwnd) != shell_pid:
            continue
        cls = class_of(hwnd)
        if cls in ('Progman', 'WorkerW', 'SHELLDLL_DefView'):
            counts[cls] = counts.get(cls, 0) + 1
    return counts


# ---------------------------------------------------------------- PNG（零依赖）
def write_png(path, width, height, bgra):
    """bgra: 自下而上的 32 位 BGRA（GetDIBits 的 BI_RGB 32bpp 就是这个顺序）

    ⚠ 逐行加 filter 字节，且 BGRA→RGBA 的交换必须**逐行**做：整块缓冲上按 4 字节步长
      交换会被每行那个 filter 字节错位，整张图颜色会花掉。
    """
    stride = width * 4
    raw = bytearray()
    for y in range(height - 1, -1, -1):            # DIB 自下而上 → PNG 自上而下
        row = bytearray(bgra[y * stride:(y + 1) * stride])
        for i in range(0, len(row), 4):
            row[i], row[i + 2] = row[i + 2], row[i]
        raw.append(0)                              # filter type 0
        raw += row

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    return len(png)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', ctypes.c_uint32), ('biWidth', ctypes.c_int32), ('biHeight', ctypes.c_int32),
                ('biPlanes', ctypes.c_uint16), ('biBitCount', ctypes.c_uint16), ('biCompression', ctypes.c_uint32),
                ('biSizeImage', ctypes.c_uint32), ('biXPelsPerMeter', ctypes.c_int32),
                ('biYPelsPerMeter', ctypes.c_int32), ('biClrUsed', ctypes.c_uint32),
                ('biClrImportant', ctypes.c_uint32)]


def capture_window(hwnd, path):
    """PrintWindow(PW_RENDERFULLCONTENT=2) 把窗口自绘成 PNG（不读桌面像素）"""
    l, t, r, b = rect_of(hwnd)
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        return None
    hdc = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    old = g32.SelectObject(mem, bmp)
    ok = u32.PrintWindow(hwnd, mem, 2)
    g32.SelectObject(mem, old)          # ⚠ GetDIBits 不能作用于还选在 DC 里的位图

    buf = ctypes.create_string_buffer(w * h * 4)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight = w, h
    bi.biPlanes, bi.biBitCount, bi.biCompression = 1, 32, 0
    lines = g32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, hdc)
    if not ok or lines == 0:
        return None
    data = buf.raw
    write_png(path, w, h, data)
    return data


# ---------------------------------------------------------------- 下层窗口（穿透 A/B 用）
sink_clicks = {'down': 0, 'up': 0}
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM)


def _sink_impl(hwnd, msg, wparam, lparam):
    if msg == WM_LBUTTONDOWN:
        sink_clicks['down'] += 1
    elif msg == 0x0202:                 # WM_LBUTTONUP
        sink_clicks['up'] += 1
    return u32.DefWindowProcW(hwnd, msg, wparam, lparam)


# ⚠ 结构体里的回调字段必须是真正的 WNDPROC 类型；用 c_void_p + cast 会丢签名，
#   参数按默认 c_int 解析，64 位 LPARAM 直接 OverflowError（只在回调被调用时才报）。
_sink_proc = WNDPROC(_sink_impl)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [('cbSize', ctypes.c_uint), ('style', ctypes.c_uint), ('lpfnWndProc', WNDPROC),
                ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int), ('hInstance', wt.HINSTANCE),
                ('hIcon', wt.HICON), ('hCursor', wt.HANDLE), ('hbrBackground', wt.HBRUSH),
                ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR), ('hIconSm', wt.HICON)]


def create_sink(rect, islands=None, below=None):
    """造一个会数点击的测试窗，作为 dock 的"下层窗口"。

    islands: 只在这些屏幕矩形内存在（窗口 region 裁剪），默认整窗 —— 目的是把
             屏幕占用压到最小；测试只需要"halo 上一个小块 + 面板上一个小块"。
    below:   插到这个窗口的正下方（SetWindowPos 的 hWndInsertAfter 语义）。
             ⚠ 必须显式插入 dock 正下方：桌面底部常被用户的其它窗口占着
               （踩过两次：一次点到用户的 Chrome，一次是虎牙小窗），
               而它们可能是 topmost，光靠 HWND_TOP 排不到它们前面。
    """
    l, t, r, b = rect
    inst = k32.GetModuleHandleW(None)
    cls_name = 'ZDockTestSink'
    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
    wc.lpfnWndProc = _sink_proc
    wc.hInstance = inst
    wc.hbrBackground = g32.CreateSolidBrush(0x00202020)
    wc.lpszClassName = cls_name
    if not u32.RegisterClassExW(ctypes.byref(wc)):
        err = ctypes.get_last_error()
        if err != 1410:                 # ERROR_CLASS_ALREADY_EXISTS 忽略
            raise ctypes.WinError(err)
    hwnd = u32.CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls_name, 'ZDockTestSink',
                               WS_POPUP | WS_VISIBLE, l, t, r - l, b - t, None, None, inst, None)
    if not hwnd:
        raise ctypes.WinError(ctypes.get_last_error())

    if islands:
        region = g32.CreateRectRgn(0, 0, 0, 0)
        for (ix, iy, iw, ih) in islands:
            piece = g32.CreateRectRgn(ix - l, iy - t, ix - l + iw, iy - t + ih)
            g32.CombineRgn(region, region, piece, 2)   # RGN_OR
            g32.DeleteObject(piece)
        u32.SetWindowRgn(hwnd, region, True)

    pin_below(hwnd, below, rect)
    return hwnd


def pin_below(hwnd, target, rect):
    """把测试窗钉到 target 的正下方（两者都是 topmost 靠这个定序）。

    ⚠ 必须在**每次点击前**重钉：点一下会让测试窗变成前台，而点击顶层窗口会把它
      抬到顶层带的最前 —— 于是它就跑到 dock 上面去了（踩过：面板那一下被它接走）。
      窗口本身也加了 WS_EX_NOACTIVATE，双重保险。
    """
    if not target:
        return
    l, t, r, b = rect
    SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_SHOWWINDOW = 0x0001, 0x0002, 0x0010, 0x0040
    u32.SetWindowPos(hwnd, target, l, t, r - l, b - t,
                     SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW)


def point_owner(x, y):
    pt = wt.POINT(x, y)
    return u32.WindowFromPoint(pt)


def real_click(x, y, pump_after=0.5):
    u32.SetCursorPos(x, y)
    time.sleep(0.12)
    u32.mouse_event(ME_LEFTDOWN, 0, 0, 0, None)
    time.sleep(0.06)
    u32.mouse_event(ME_LEFTUP, 0, 0, 0, None)
    pump(pump_after)      # 排队消息要泵才会进 WNDPROC


def cpu_seconds(hproc):
    c, e, k, u = (ctypes.c_uint64() for _ in range(4))
    k32.GetProcessTimes(hproc, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    return (k.value + u.value) / 1e7      # 100ns 单位


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print('找不到 exe：' + exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    print('== ZDock 阶段一 gate 验证 ==')
    info('exe: ' + exe)

    before = shell_chain_counts()
    info('启动前 explorer shell 窗口：' + str(before))

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe))
    hwnd = find_dock_window(8.0)
    if not hwnd:
        print('  [FAIL] 没找到 ZDock 窗口（进程退出码 %s）' % proc.poll())
        proc.terminate()
        return 1
    time.sleep(0.6)
    hproc = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, proc.pid)

    # ---- 1. 红线自检
    print('\n-- 1. 红线自检 --')
    ex = _get_long(hwnd, GWL_EXSTYLE)
    owner = _get_long(hwnd, GWL_HWNDPARENT)
    root = u32.GetAncestor(hwnd, GA_PARENT)
    check('窗口是独立顶层窗口（GA_PARENT = 桌面）', root == u32.GetDesktopWindow(), 'GA_PARENT=0x%X' % (root or 0))
    check('没有 owner（GWL_HWNDPARENT = 0）', owner == 0, 'owner=0x%X' % (owner or 0))
    check('WS_EX_TOPMOST', bool(ex & WS_EX_TOPMOST))
    check('WS_EX_TOOLWINDOW（不进 Alt+Tab / 任务栏）', bool(ex & WS_EX_TOOLWINDOW))
    check('WS_EX_NOACTIVATE（点击不抢焦点）', bool(ex & WS_EX_NOACTIVATE))
    check('Composition 栈：NOREDIRECTIONBITMAP 且非 layered',
          bool(ex & WS_EX_NOREDIRECTIONBITMAP) and not (ex & WS_EX_LAYERED))

    after = shell_chain_counts()
    info('启动后 explorer shell 窗口：' + str(after))
    check('启动前后 explorer 的 WorkerW 计数不变',
          before.get('WorkerW', 0) == after.get('WorkerW', 0),
          '%s -> %s' % (before.get('WorkerW', 0), after.get('WorkerW', 0)))
    check('没往桌面链里塞窗口（Progman / SHELLDLL_DefView 计数不变）',
          before.get('Progman', 0) == after.get('Progman', 0)
          and before.get('SHELLDLL_DefView', 0) == after.get('SHELLDLL_DefView', 0))

    # ---- 2. 位置
    print('\n-- 2. 位置 --')
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    wa = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(wa), 0)
    # ⚠ 定位基准是**监视器 rcMonitor**，不是工作区。
    #   原因：AppBar 预留会改工作区，拿工作区当基准 = 自引用反馈回路
    #   （dock 每注册一次就往上爬一层）。见 DockWin::dockRectShown 注释。
    MONITOR_DEFAULTTONEAREST = 2
    MI = type('MI', (ctypes.Structure,), {'_fields_': [
        ('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    u32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
    u32.MonitorFromWindow.restype = wt.HANDLE
    u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]
    mi = MI(); mi.cbSize = ctypes.sizeof(MI)
    mon = wt.RECT()
    _mh = u32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
    if _mh and u32.GetMonitorInfoW(_mh, ctypes.byref(mi)):
        mon = mi.rcMonitor
    else:
        mon = wt.RECT(0, 0, u32.GetSystemMetrics(SM_CXSCREEN), u32.GetSystemMetrics(SM_CYSCREEN))
    l, t, r, b = rect_of(hwnd)
    win_w, win_h = r - l, b - t
    panel_h = int(round((48 + 2 * 8) * dpi))
    info('屏幕 %dx%d 监视器 %s 工作区 %s 窗口 %s dpi=%.2f' %
         (u32.GetSystemMetrics(SM_CXSCREEN), u32.GetSystemMetrics(SM_CYSCREEN),
          (mon.left, mon.top, mon.right, mon.bottom),
          (wa.left, wa.top, wa.right, wa.bottom), (l, t, r, b), dpi))
    check('面板底边贴屏幕底边（留 6 逻辑像素）',
          abs(b - (mon.bottom - round(6 * dpi))) <= 2,
          '窗口底=%d 期望=%d' % (b, mon.bottom - round(6 * dpi)))
    check('水平居中于屏幕', abs((l + r) / 2 - (mon.left + mon.right) / 2) <= 2,
          '窗口中心=%.0f 屏幕中心=%.0f' % ((l + r) / 2, (mon.left + mon.right) / 2))
    check('窗口比面板高（给放大留了 halo）', win_h > panel_h,
          'win_h=%d panel_h=%d' % (win_h, panel_h))

    # ---- 3. 渲染
    print('\n-- 3. 渲染 --')
    # ⚠ 先记下用户光标在哪 —— 必须在这之后就把光标挪开，
    #   否则最后"还原光标"会把它还到我们临时用的那个位置。
    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    # ⚠ 拍"空闲参考图"之前**必须先把光标移开**。
    #   这个探针跑完会把光标还原到它开始时的位置 —— 如果那是上一次跑、
    #   或者别的脚本留下的 dock 上某处，那么这张"空闲图"其实**已经带着放大**了，
    #   后面拿它跟悬停图一比 → 差异 0 像素 → 误判成"放大动画没在画"。
    #   （实测踩过：调试脚本把光标留在 dock 上，下一轮阶段一就报这条假失败。）
    u32.SetCursorPos(4, 4)
    time.sleep(0.35)
    idle_png = os.path.join(OUT_DIR, 'zdock_idle.png')
    idle = capture_window(hwnd, idle_png)
    check('PrintWindow 抓到窗口自绘内容', idle is not None, idle_png)

    side_slack = int(round(48 * dpi))
    pad_x = 14 * dpi
    icon_base = 48 * dpi
    gap = 12 * dpi
    panel_left = l + side_slack
    icon_y = b - int(round(8 * dpi)) - int(icon_base / 2)
    icon_centers = [int(panel_left + pad_x + i * (icon_base + gap) + icon_base / 2) for i in range(6)]

    u32.SetCursorPos(icon_centers[2], icon_y)
    time.sleep(0.45)
    hover_png = os.path.join(OUT_DIR, 'zdock_hover.png')
    hover = capture_window(hwnd, hover_png)
    check('悬停截图抓取成功', hover is not None, hover_png)

    if idle and hover and len(idle) == len(hover):
        stride = win_w * 4
        halo = int(round(72 * dpi))
        y_from = max(0, win_h - halo - int(icon_base * 1.3))
        diff = 0
        for y in range(y_from, win_h):
            base = y * stride
            for x in range(0, win_w * 4, 4):
                if idle[base + x:base + x + 3] != hover[base + x:base + x + 3]:
                    diff += 1
        check('悬停后画面确有变化（放大动画真的在画）', diff > 200, '差异像素 %d' % diff)
    else:
        check('悬停前后可比对', False, '截图尺寸不一致')

    # ---- 4. 鼠标穿透（测试窗插在 dock 正下方，只开两个小岛）
    print('\n-- 4. 鼠标穿透 --')
    halo_x = l + 8
    halo_y = t + int(round(20 * dpi))
    panel_x = panel_left + int(round(4 * dpi))
    panel_y = b - int(round((24 + 8) * dpi))

    # 先做不点鼠标的判定：面板属于 dock、halo 不属于 dock
    whose_panel = point_owner(panel_x, panel_y)
    check('面板实体区属于 dock（WindowFromPoint = dock）', whose_panel == hwnd,
          'window=%s' % class_of(whose_panel))
    whose_halo = point_owner(halo_x, halo_y)
    check('halo 不再属于 dock（窗口 region 已把它挖掉）', whose_halo != hwnd,
          'window=%s' % class_of(whose_halo))

    sink = create_sink((l, t, r, b),
                       islands=[(halo_x - 60, halo_y - 25, 120, 50),
                                (panel_x - 30, panel_y - 25, 60, 50)],
                       below=hwnd)
    pump(0.5)

    # 先做"面板不穿透"（此刻测试窗在最下、dock 在上面），再做 halo 穿透
    pin_below(sink, hwnd, (l, t, r, b))
    sink_clicks['down'] = 0
    real_click(panel_x, panel_y)
    check('面板实体区点击被 dock 吃掉（不穿透）', sink_clicks['down'] == 0,
          '下层收到 %d 次' % sink_clicks['down'])

    pin_below(sink, hwnd, (l, t, r, b))
    if point_owner(halo_x, halo_y) == sink:
        sink_clicks['down'] = 0
        real_click(halo_x, halo_y)
        check('halo 透明区点击穿透到下层窗口', sink_clicks['down'] >= 1,
              '下层收到 %d 次 WM_LBUTTONDOWN' % sink_clicks['down'])
    else:
        # 下层窗口没能排到 dock 正下方 —— 那就不点，绝不把点击打到用户自己的窗口上
        info('halo 点当前属于 %s，跳过真实点击（避免误点用户窗口）' % class_of(point_owner(halo_x, halo_y)))
        check('halo 透明区点击穿透到下层窗口', False,
              '下层窗口未就绪（被 %s 占着）' % class_of(point_owner(halo_x, halo_y)))

    u32.DestroyWindow(sink)

    # ---- 5. 性能
    print('\n-- 5. 性能 --')
    cores = os.cpu_count() or 1
    u32.SetCursorPos(saved.x, saved.y)
    idle_start = cpu_seconds(hproc)
    time.sleep(10.0)
    idle_delta = cpu_seconds(hproc) - idle_start
    check('空闲 10s CPU 增量 < 1% 单核', idle_delta < 0.1,
          '%.3fs / 10s = %.3f%% 单核（%d 核）' % (idle_delta, idle_delta * 10, cores))

    sweep_start = cpu_seconds(hproc)
    t0 = time.time()
    while time.time() - t0 < 3.0:
        for cx in icon_centers:
            u32.SetCursorPos(cx, icon_y)
            time.sleep(0.05)
    sweep_span = time.time() - t0
    sweep_delta = cpu_seconds(hproc) - sweep_start
    pct = sweep_delta / sweep_span * 100
    check('悬停扫动期间 CPU < 5% 单核', pct < 5.0,
          '%.2f%% 单核（%.2fs 内 %.3fs）' % (pct, sweep_span, sweep_delta))

    u32.SetCursorPos(saved.x, saved.y)
    time.sleep(1.2)
    post_start = cpu_seconds(hproc)
    time.sleep(3.0)
    post_delta = cpu_seconds(hproc) - post_start
    check('移开鼠标后回落（3s 增量 < 1% 单核）', post_delta < 0.03, '%.3fs / 3s' % post_delta)

    # ---- 6. 收尾
    print('\n-- 6. 收尾 --')
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    time.sleep(0.8)
    check('进程已退出', proc.poll() is not None, 'exit=%s' % proc.poll())
    left = [h for h in enum_windows() if class_of(h) == 'ZDock' and u32.IsWindowVisible(h)]
    check('无残留窗口', not left, '剩余 %d 个' % len(left))
    final = shell_chain_counts()
    check('结束后 explorer 的 WorkerW 计数仍与启动前一致',
          final.get('WorkerW', 0) == before.get('WorkerW', 0),
          '%s vs %s' % (final.get('WorkerW', 0), before.get('WorkerW', 0)))

    failed = [n for n, ok, _ in results if not ok]
    print('\n== 汇总：%d/%d 通过 ==' % (len(results) - len(failed), len(results)))
    for n in failed:
        print('   FAIL: ' + n)
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
