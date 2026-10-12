"""纯机制探针：`SetTimer(16)` 的真实触发间隔 —— 带 / 不带 `timeBeginPeriod(1)`。

背景：ZDock 的滑动补间靠 `WM_TIMER` 驱动，实测平均 **24ms/帧**（≈41fps），
而且 `timeBeginPeriod(1)` 几乎没改善。但 `setPosition` 只花 0.13ms —— 说明慢的不是
"移动窗口"，而是"定时器什么时候来"。

这个探针在一个**几乎空的窗口**上做同样的事：
  · 如果连空窗口都只能拿到 ~24ms，那就是 `WM_TIMER` 本身的问题（消息优先级低 /
    时钟节拍），换驱动方式（`CreateTimerQueueTimer` / `SetWaitableTimer`）才有意义；
  · 如果空窗口能拿到 ~16ms，那问题在 ZDock 的消息泵被别的消息挤占。

⚠ 完全不碰 ZDock，也不显示任何窗口，只跑 ~1 秒。

跑法： <python> build-support/_probe_timer_resolution.py
"""

import ctypes
import statistics
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
winmm = ctypes.WinDLL('winmm')

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
u32.SetTimer.argtypes = [wt.HWND, ctypes.c_size_t, ctypes.c_uint, ctypes.c_void_p]
u32.SetTimer.restype = ctypes.c_size_t
u32.KillTimer.argtypes = [wt.HWND, ctypes.c_size_t]
u32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
u32.DestroyWindow.argtypes = [wt.HWND]
winmm.timeBeginPeriod.argtypes = [ctypes.c_uint]
winmm.timeEndPeriod.argtypes = [ctypes.c_uint]

WM_TIMER = 0x0113
WM_QUIT = 0x0012
_hinst = k32.GetModuleHandleW(None)
_proc = WNDPROC(lambda h, m, w, l: u32.DefWindowProcW(h, m, w, l))   # 保持引用


def make_window(cls):
    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
    wc.lpfnWndProc = _proc
    wc.hInstance = _hinst
    wc.lpszClassName = cls
    if not u32.RegisterClassExW(ctypes.byref(wc)) and ctypes.get_last_error() != 1410:
        return None
    return u32.CreateWindowExW(0, cls, 'probe', 0x80000000,  # WS_POPUP，不显示
                               0, 0, 10, 10, None, None, _hinst, None)


def measure(period_1, ms=16, n=30):
    if period_1:
        winmm.timeBeginPeriod(1)
    try:
        hwnd = make_window('ZDockTimerProbe' + ('A' if period_1 else 'B'))
        if not hwnd:
            return None
        u32.SetTimer(hwnd, 1, ms, None)
        gaps, msg = [], wt.MSG()
        t0 = time.perf_counter()
        last = t0
        while len(gaps) < n:
            r = u32.GetMessageW(ctypes.byref(msg), None, 0, 0)
            if r <= 0 or msg.message == WM_QUIT:
                break
            if msg.message == WM_TIMER:
                now = time.perf_counter()
                gaps.append((now - last) * 1000.0)
                last = now
            u32.DispatchMessageW(ctypes.byref(msg))
        u32.KillTimer(hwnd, 1)
        u32.DestroyWindow(hwnd)
        return gaps
    finally:
        if period_1:
            winmm.timeEndPeriod(1)


def report(tag, gaps):
    if not gaps:
        print(f'  {tag}: 拿不到样本')
        return None
    g = gaps[1:]        # 第一帧含启动开销，去掉
    print(f'  {tag}: 平均 {statistics.mean(g):.2f}ms / 中位 {statistics.median(g):.2f}ms / '
          f'最大 {max(g):.2f}ms / 最小 {min(g):.2f}ms   （{len(g)} 帧）')
    return statistics.mean(g)


print('=' * 72)
print('空窗口上 SetTimer 的真实触发间隔（扫不同 elapse）')
print('=' * 72)
rows = []
for ms in (10, 15, 16, 20, 25, 31, 32):
    g = measure(False, ms=ms)
    m = report(f'SetTimer({ms:>2}ms)', g) if g else None
    rows.append((ms, m))
print()
a = report('SetTimer(16) + timeBeginPeriod(1)', measure(True))
print('=' * 72)
for ms, m in rows:
    if m:
        print(f'  请求 {ms:>2}ms → 实际 {m:5.1f}ms   （约 {1000.0 / m:4.1f} fps）')
print('=' * 72)
