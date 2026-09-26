"""决定性取证：同时跑 ZPin 与（旧行为下的）ZDock，把它们各自的
message-only 窗口（类 STATIC，标题 = appID）真实标题打出来。

⚠ 之前的 _probe_appid.py 结论是**错的**：std::wstring 是运行期堆分配的，
   .rdata 里只有 L"Ling_" 这个前缀字面量，后 6 个字符是运行期算出来的，
   所以"exe 里搜不到 Ling_XXXXXX"完全不能证明 appID 相同。
   这个脚本直接问运行中的窗口，才是最硬的证据。
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowExW.restype = wt.HWND
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
HWND_MESSAGE = wt.HWND(-3)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.dirname(ROOT)
ZDOCK = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
_zdir = os.path.join(PROJECTS, 'ZPin', 'ext', 'build', 'release')
ZPIN = None
if os.path.isdir(_zdir):
    for f in sorted(os.listdir(_zdir)):
        if f.lower().startswith('zpin') and f.lower().endswith('.exe'):
            ZPIN = os.path.join(_zdir, f)
            break


def procs():
    TH = 0x2

    class PE(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('c', wt.DWORD), ('pid', wt.DWORD),
                    ('d', ctypes.c_void_p), ('m', wt.DWORD), ('t', wt.DWORD),
                    ('pp', wt.DWORD), ('pr', ctypes.c_long), ('f', wt.DWORD),
                    ('sz', wt.WCHAR * 260)]

    s = k32.CreateToolhelp32Snapshot(TH, 0)
    o = {}
    if s in (0, -1):
        return o
    e = PE()
    e.dwSize = ctypes.sizeof(e)
    if k32.Process32FirstW(s, ctypes.byref(e)):
        while True:
            o[e.pid] = e.sz
            if not k32.Process32NextW(s, ctypes.byref(e)):
                break
    k32.CloseHandle(s)
    return o


def msg_windows():
    """枚举 HWND_MESSAGE 的直接子窗口（message-only 窗口是它的子窗口）。"""
    out = []
    h = u32.FindWindowExW(HWND_MESSAGE, None, None, None)
    while h:
        cb = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cb, 256)
        tb = ctypes.create_unicode_buffer(512)
        u32.GetWindowTextW(h, tb, 512)
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        out.append((h, cb.value, tb.value, pid.value))
        h = u32.FindWindowExW(HWND_MESSAGE, h, None, None)
    return out


def snapshot(tag):
    pm = procs()
    print('  [%s] message-only 窗口（类 STATIC）:' % tag)
    found = False
    for hwnd, cls, title, pid in msg_windows():
        if cls != 'STATIC':
            continue
        found = True
        print('     0x%08X title=%-16r pid=%-6d %s' % (hwnd, title, pid, pm.get(pid, '?')))
    if not found:
        print('     (无)')
    return [(t, p) for _, c, t, p in msg_windows() if c == 'STATIC']


def main():
    if not ZPIN or not os.path.isfile(ZDOCK):
        print('缺产物'); return 2
    print('ZPin : %s' % ZPIN)
    print('ZDock: %s' % ZDOCK)
    print()

    print('--- A) 只跑 ZPin ---')
    zpin = subprocess.Popen([ZPIN], cwd=os.path.dirname(ZPIN))
    time.sleep(4.0)
    a = snapshot('仅 ZPin')

    print('--- B) 起 ZDock ---')
    zdock = subprocess.Popen([ZDOCK], cwd=os.path.dirname(ZDOCK))
    time.sleep(3.0)
    b = snapshot('ZPin+ZDock')
    print('  ZDock 进程存活=%s' % (zdock.poll() is None))

    print()
    ap = [t for t, p in a if 'ZPin' in procs().get(p, '')]
    print('结论：')
    print('  ZPin 的 appID  = %r' % (ap[0] if ap else '(未捕获)'))
    print('  ZDock 是否起来 = %s' % (zdock.poll() is None))
    print('  ZDock 自己的 appID = %r' % ([t for t, p in b if procs().get(p, '').lower() == 'zdock.exe'] or '(无窗口)'))

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
    return 0


if __name__ == '__main__':
    sys.exit(main())
