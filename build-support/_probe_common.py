"""探针公共工具。

⚠ **每个探针开跑前都应该先调 `kill_existing_zdock()`。**

理由：ZDock 的单实例互斥体名是**固定的**
（`Local\\ZDock.SingleInstance.{6B1E7A3C-...}`），所以只要已经有一个实例在跑，
探针起的那个会**静默让位退出** —— 日志里只有一句
"已有实例在运行 -> 本次启动放弃"，表现出来却是**所有探针一起 FAIL**，
非常像"程序启动就崩了"。（这坑踩过两次，一次还误判成崩溃去查了半天。）

而**不能只按映像名杀**：用户验收时跑的是**改名副本**（`ZDock_0.1.10.exe` 之类），
`taskkill /IM ZDock.exe` 根本对不上、杀不掉。
所以这里按**窗口类名**找顶层窗口、再拿它的 pid 去杀 —— 改名不影响。

用户 2026-10-07 明确同意："你在验证脚本里，最前面加一个关闭实例"。
"""

import ctypes
import subprocess
import sys
import time
from ctypes import wintypes as wt

_u32 = ctypes.WinDLL('user32')
_u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
_u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
_u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]


def zdock_pids():
    """当前所有在跑的 ZDock 实例的 pid（按**窗口类名**认，不看 exe 名字）。"""
    pids = set()

    def cb(hwnd, _lp):
        buf = ctypes.create_unicode_buffer(256)
        _u32.GetClassNameW(hwnd, buf, 256)
        # dock 主窗口 / 设置窗口都是这个类名
        if buf.value == 'ZDock':
            p = wt.DWORD()
            _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value:
                pids.add(int(p.value))
        return True

    _u32.EnumWindows(
        ctypes.cast(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb),
                    ctypes.c_void_p), 0)
    return pids


def kill_existing_zdock(verbose=True):
    """关掉所有在跑的 ZDock 实例，返回关掉了几个。

    先试**友好关闭**（不带 /F = 发 WM_CLOSE）—— 让它自己走正常退出路径
    （注销 AppBar、删强杀自愈的状态文件），比直接强杀干净。
    0.6 秒还没走再强杀。
    """
    pids = zdock_pids()
    if not pids:
        return 0

    for pid in pids:
        subprocess.run(['taskkill', '/PID', str(pid)], capture_output=True)
    time.sleep(0.6)
    for pid in zdock_pids():        # 还活着的强杀
        subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
    time.sleep(0.3)

    if verbose:
        print('  [setup] 已关闭 %d 个在跑的 ZDock 实例'
              '（不关的话单实例会让探针静默让位）' % len(pids))
        sys.stdout.flush()
    return len(pids)


if __name__ == '__main__':
    n = kill_existing_zdock()
    print('关闭了 %d 个实例' % n)


# ---------------------------------------------------------------------------
# 工作区快照 / 还原
#
# ⚠ 为什么必须有：ZDock 的"预留工作区"走 AppBar，而 **AppBar 的占用是全局状态** ——
#   `taskkill /F` 之后 shell 还留着那条死记录，工作区一直缩着，**不会自动恢复**。
#   用户机器上用的是**全屏开始菜单**，它是按工作区布局的，工作区缩了 95px，
#   开始菜单就跟着变小 / 上移（2026-10-07 他报的"测试完以后开始菜单也跟着上移了"）。
#
#   所以凡是可能碰到 AppBar 的探针，都必须：
#     开跑前 snapshot_work_area()（先关掉在跑的实例，此时量的才是干净值）
#     收尾时 restore_work_area()
# ---------------------------------------------------------------------------

_u32 = ctypes.WinDLL('user32')
SPI_GETWORKAREA = 0x0030
SPI_SETWORKAREA = 0x002F

_work_snapshot = None


def clean_work_area_value():
    """算出"没有任何 AppBar 占用"时该有的工作区：**主监视器矩形 − 任务栏那一条**。

    ⚠ 为什么不能直接读 `SPI_GETWORKAREA` 当基线：AppBar 的占用是全局状态，
      上次测试强杀留下的死记录还在时，读到的**就是脏值** —— 拿它当快照等于
      把脏状态固化成"标准答案"，收尾也恢复不了。
    ⚠ 也不能像老代码那样写死"顶部 38px"：那是**本机**的任务栏高度，
      换台机器（任务栏在底部 / 自动隐藏 / 换 DPI）就错了。这里从 shell 问。
    """
    MONITORINFO = type('MONITORINFO', (ctypes.Structure,), {
        '_fields_': [('cbSize', wt.DWORD), ('rcMonitor', wt.RECT),
                     ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    class APPBARDATA(ctypes.Structure):
        _fields_ = [('cbSize', wt.DWORD), ('hWnd', wt.HWND), ('uCallbackMessage', wt.UINT),
                    ('uEdge', wt.UINT), ('rc', wt.RECT), ('lParam', wt.LPARAM)]

    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    _u32.MonitorFromPoint.argtypes = [wt.POINT, wt.DWORD]
    _u32.MonitorFromPoint.restype = wt.HANDLE
    _u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]
    mon = _u32.MonitorFromPoint(wt.POINT(0, 0), 1)      # MONITOR_DEFAULTTOPRIMARY
    if not mon or not _u32.GetMonitorInfoW(mon, ctypes.byref(mi)):
        return None
    m = mi.rcMonitor

    ab = APPBARDATA()
    ab.cbSize = ctypes.sizeof(APPBARDATA)
    sh = ctypes.WinDLL('shell32')
    sh.SHAppBarMessage.argtypes = [wt.DWORD, ctypes.c_void_p]
    sh.SHAppBarMessage.restype = ctypes.c_void_p
    sh.SHAppBarMessage(5, ctypes.byref(ab))            # ABM_GETTASKBARPOS

    left, top, right, bottom = m.left, m.top, m.right, m.bottom
    ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM = 0, 1, 2, 3
    if ab.uEdge == ABE_TOP:
        top = max(top, ab.rc.bottom)
    elif ab.uEdge == ABE_BOTTOM:
        bottom = min(bottom, ab.rc.top)
    elif ab.uEdge == ABE_LEFT:
        left = max(left, ab.rc.right)
    elif ab.uEdge == ABE_RIGHT:
        right = min(right, ab.rc.left)
    return (left, top, right, bottom)


def current_work_area():
    r = wt.RECT()
    _u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)


def snapshot_work_area(verbose=True):
    """记下"干净工作区"（算出来的，不是读出来的），供收尾时写回。

    ⚠ 用算的值而不是当前值：开跑时可能还有上次强杀留下的死 AppBar，
      这时 `SPI_GETWORKAREA` 是脏的（见 clean_work_area_value 的说明）。
    """
    global _work_snapshot
    _work_snapshot = clean_work_area_value()
    if verbose and _work_snapshot:
        cur = current_work_area()
        dirty = '' if cur == _work_snapshot else f'   ⚠ 当前读到的是 {cur}（被占用着，收尾会写回干净值）'
        print(f'  [setup] 干净工作区 ({_work_snapshot[0]},{_work_snapshot[1]})-'
              f'({_work_snapshot[2]},{_work_snapshot[3]}){dirty}')
        sys.stdout.flush()
    return _work_snapshot


def restore_work_area(verbose=True):
    """收尾：把工作区写回开跑前记下的值。没记过就什么都不做。

    ⚠ 调用前**先确保没有 dock 在跑** —— 否则 shell 会按那条还活着的 AppBar 重算，
      我们这次写的值会被它盖回去。
    """
    if _work_snapshot is None:
        return False
    r = wt.RECT(*_work_snapshot)
    ok = bool(_u32.SystemParametersInfoW(SPI_SETWORKAREA, 0, ctypes.byref(r), 1))
    if verbose:
        r2 = wt.RECT()
        _u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r2), 0)
        s0 = _work_snapshot
        note = '' if ok else '  (写回失败)'
        print(f'  [teardown] 工作区写回 ({s0[0]},{s0[1]})-({s0[2]},{s0[3]})'
              f' -> 现在是 ({r2.left},{r2.top})-({r2.right},{r2.bottom}){note}')
        sys.stdout.flush()
    return ok
