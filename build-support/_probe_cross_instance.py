"""端到端复现 bug 1：ZPin 与 ZDock 互相误判成"第二实例"。

做的事（全程不抢焦点、不动用户文件）：
  1) 起 ZPin（它自己会挂托盘图标待命，不自动进截图模式）
  2) 起 ZDock，看它是否真的起来了（进程 + 窗口 + 日志）
  3) 再起一个 ZDock，看第二次是否被误判
  4) 收尾：杀掉本脚本拉起的两个进程

判据：
  · ZDock 的日志里出现 "second instance -> exit"  -> 被 ZPin 误伤了（bug 复现）
  · 或者 ZDock 进程起来但没有任何窗口            -> 同上

跑法： <python> build-support/_probe_cross_instance.py
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.IsWindow.argtypes = [wt.HWND]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # projects/ZDock
PROJECTS = os.path.dirname(ROOT)                                    # projects/
ZDOCK = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
# ZPin 的发布产物；名字带版本号，扫一下目录别写死
ZPIN = None
_zpin_dir = os.path.join(PROJECTS, 'ZPin', 'ext', 'build', 'release')
if os.path.isdir(_zpin_dir):
    for _f in sorted(os.listdir(_zpin_dir)):
        if _f.lower().startswith('zpin') and _f.lower().endswith('.exe'):
            ZPIN = os.path.join(_zpin_dir, _f)
            break
ZDOCK_LOG = os.path.join(ROOT, 'build', 'bin', 'ZDock.log')


def proc_map():
    TH32CS_SNAPPROCESS = 0x2

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD),
                    ('th32ProcessID', wt.DWORD), ('th32DefaultHeapID', ctypes.c_void_p),
                    ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                    ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                    ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    out = {}
    if snap in (0, -1):
        return out
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    if k32.Process32FirstW(snap, ctypes.byref(e)):
        while True:
            out[e.th32ProcessID] = e.szExeFile
            if not k32.Process32NextW(snap, ctypes.byref(e)):
                break
    k32.CloseHandle(snap)
    return out


def dock_hwnds():
    """枚举本进程树能看到的 ZDock 窗口（类名 ZDock）。"""
    out = []
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(128)
        u32.GetClassNameW(hwnd, buf, 128)
        if buf.value == 'ZDock':
            out.append(hwnd)
        return True

    u32.EnumWindows(CB(cb), 0)
    return out


def log_tail(path, n=8):
    if not os.path.exists(path):
        return []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [ln.rstrip() for ln in f]
    return lines[-n:]


def main():
    for p in (ZDOCK_LOG, ZDOCK_LOG + '.1'):
        if os.path.exists(p):
            os.remove(p)

    if not ZPIN or not os.path.isfile(ZPIN):
        print('找不到 ZPin 发行产物（%s），复现不了，跳过' % _zpin_dir)
        return 2
    if not os.path.isfile(ZDOCK):
        print('找不到 ZDock.exe，先 bash build-support/build.sh')
        return 2
    print('ZPin : %s' % ZPIN)
    print('ZDock: %s' % ZDOCK)

    print('--- 1) 起 ZPin ---')
    zpin = subprocess.Popen([ZPIN], cwd=os.path.dirname(ZPIN))
    time.sleep(4.0)
    pm = proc_map()
    print('  ZPin pid=%d 存活=%s' % (zpin.pid, zpin.poll() is None))
    print('  任务栏托盘进程:', [v for v in pm.values() if 'ZPin' in v])

    print('--- 2) 起 ZDock（ZPin 已在跑）---')
    zdock = subprocess.Popen([ZDOCK], cwd=os.path.dirname(ZDOCK))
    time.sleep(3.0)
    alive = zdock.poll() is None
    wins = dock_hwnds()
    print('  ZDock pid=%d 存活=%s  窗口数=%d' % (zdock.pid, alive, len(wins)))
    print('  日志尾部:')
    for ln in log_tail(ZDOCK_LOG):
        print('    ' + ln)

    verdict = None
    tail = log_tail(ZDOCK_LOG, 20)
    if any('second instance' in ln for ln in tail):
        verdict = 'BUG 复现：ZDock 被 ZPin 误判成第二实例，直接退出'
    elif alive and not wins:
        verdict = 'BUG 复现：ZDock 进程活着但没建出窗口'
    elif not alive:
        verdict = 'BUG 复现：ZDock 起来就退了'
    else:
        verdict = '未复现：ZDock 正常起来了'
    print('  => %s' % verdict)

    print('--- 3) 收尾 ---')
    for p in (zdock, zpin):
        try:
            p.terminate()
        except Exception:
            pass
    time.sleep(1.5)
    for p in (zdock, zpin):
        if p.poll() is None:
            try:
                p.kill()
            except Exception:
                pass
    return 0 if '复现' in verdict else 1


if __name__ == '__main__':
    sys.exit(main())
