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
