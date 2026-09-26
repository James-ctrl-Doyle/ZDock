"""阶段三第一步：验证事件驱动窗口跟踪的两个机制在本进程可用。

这是在写任何跟踪代码之前必须做的**最贵假设验证**（任务书 §14.2 的要求）：
  1) SetWinEventHook 能否收到其它进程的窗口事件？（不注入、不挂钩子，官方接口）
  2) RegisterShellHookWindow 能否收到 HSHELL_* 事件？
  3) 从事件里拿到的 hwnd 能否正确解析出 pid / exe 路径 / AUMID？

跑法： <python> build-support/_probe_track_api.py
全程只读：只枚举与查询，不改任何窗口。
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
sh = ctypes.WinDLL('shell32', use_last_error=True)
ole = ctypes.WinDLL('ole32', use_last_error=True)

# ---- WinEvent 常量 ----
EVENT_OBJECT_CREATE = 0x8000
EVENT_OBJECT_DESTROY = 0x8001
EVENT_OBJECT_SHOW = 0x8002
EVENT_OBJECT_HIDE = 0x8003
EVENT_SYSTEM_FOREGROUND = 0x0003
EVENT_OBJECT_NAMECHANGE = 0x800C
EVENT_OBJECT_LOCATIONCHANGE = 0x800B
EVENT_SYSTEM_MINIMIZESTART = 0x0016
EVENT_SYSTEM_MINIMIZEEND = 0x0017

WINEVENT_OUTOFCONTEXT = 0x0000
WINEVENT_SKIPOWNPROCESS = 0x0002

EVENT_NAMES = {
    EVENT_OBJECT_CREATE: 'CREATE',
    EVENT_OBJECT_DESTROY: 'DESTROY',
    EVENT_OBJECT_SHOW: 'SHOW',
    EVENT_OBJECT_HIDE: 'HIDE',
    EVENT_SYSTEM_FOREGROUND: 'FOREGROUND',
    EVENT_OBJECT_NAMECHANGE: 'NAMECHANGE',
    EVENT_OBJECT_LOCATIONCHANGE: 'LOCATIONCHANGE',
    EVENT_SYSTEM_MINIMIZESTART: 'MINIMIZESTART',
    EVENT_SYSTEM_MINIMIZEEND: 'MINIMIZEEND',
}

u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowThreadProcessId.restype = wt.DWORD
u32.IsWindow.argtypes = [wt.HWND]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
u32.GetWindow.restype = wt.HWND
u32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u32.GetWindowLongW.restype = ctypes.c_long
ole.CoInitializeEx.argtypes = [ctypes.c_void_p, wt.DWORD]

GWL_EXSTYLE = -20
GWL_STYLE = -16
GW_OWNER = 4
WS_EX_TOOLWINDOW = 0x00000080

k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]

_hits = {}
SHELLHOOK_MSG = [0]     # RegisterWindowMessageW("SHELLHOOK") 的返回值

# HSHELL_* 常量（WinUser.h）
HSHELL = {
    1: 'WINDOWCREATED', 2: 'WINDOWDESTROYED', 3: 'ACTIVATE_SHELLWINDOW',
    4: 'WINDOWACTIVATED', 5: 'GETMINRECT', 6: 'REDRAW', 7: 'TASKM AN',
    8: 'LANGUAGE', 9: 'ACCESSIBILITYSTATE', 10: 'APPCOMMAND',
    11: 'WINDOWREPLACED', 12: 'WINDOWREPLACING', 13: 'MONITORCHANGED',
    0x8006: 'FLASH',           # REDRAW | HIGHBIT
    0x8004: 'RUDEAPPACTIVATED',  # WINDOWACTIVATED | HIGHBIT
}


def proc_path(pid):
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return buf.value
        return None
    finally:
        k32.CloseHandle(h)


def is_trackable(hwnd):
    """任务书 §9.4 的过滤规则（按它实现一遍，验证规则本身是否合理）。"""
    if not u32.IsWindow(hwnd) or not u32.IsWindowVisible(hwnd):
        return False, 'not-window-or-hidden'
    if u32.GetWindow(hwnd, GW_OWNER):
        return False, 'has-owner'
    if u32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW:
        return False, 'toolwindow'
    # 任务书：不要无脑排除 UWP 的 ApplicationFrameWindow —— 要下钻
    return True, 'ok'


def make_cb():
    CB = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.DWORD, wt.HWND,
                            ctypes.c_long, ctypes.c_long, wt.DWORD, wt.DWORD)

    def cb(hook, ev, hwnd, idobj, idchild, tid, t):
        if hwnd:
            _hits.setdefault(ev, []).append(hwnd)
    return CB(cb)


def make_shell_cb():
    CB = ctypes.WINFUNCTYPE(None, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

    def cb(hwnd, msg, wp, lp):
        _hits.setdefault(('SHELL', msg), []).append((wp, lp))
    return CB(cb)


def main():
    results = []

    # ---- 1) SetWinEventHook ----
    print('[1] SetWinEventHook（OUTOFCONTEXT，不注入）')
    hooks = []
    cb = make_cb()
    for ev in (EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
               EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_NAMECHANGE):
        h = u32.SetWinEventHook(ev, ev, None, cb, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)
        print('    event=%-14s hook=%s %s' % (
            EVENT_NAMES.get(ev, hex(ev)), h,
            'OK' if h else 'FAILED err=%d' % ctypes.get_last_error()))
        if h:
            hooks.append(h)
    results.append(('SetWinEventHook 建钩成功', len(hooks) == 5))

    # ---- 2) RegisterShellHookWindow ----
    print('\n[2] RegisterShellHookWindow')
    # 需要一个真实窗口来接收 shell hook（消息专用窗口不行）
    MSG_HWND = None
    hInst = k32.GetModuleHandleW(None)
    wc = ctypes.create_string_buffer(40)
    cls_name = 'ZDockTrackProbeWnd'
    u32.DefWindowProcW.restype = ctypes.c_long
    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_long, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    shell_cb = make_shell_cb()

    def wp(hwnd, msg, wparam, lparam):
        # ⚠ shell hook 的消息号是 RegisterWindowMessageW("SHELLHOOK") 的返回值，
        #   不是固定的 WM_*。第一版探针把"除 WM_CREATE 外全部"都当 hook 收了，
        #   结果 0x81/0x83/0x24 这些无关消息也混进来了（看着像收到了，其实不是）。
        if msg == SHELLHOOK_MSG[0]:
            try:
                shell_cb(hwnd, wparam, wparam, lparam)
            except Exception:
                pass
        return u32.DefWindowProcW(hwnd, msg, wparam, lparam)

    proc = WNDPROC(wp)

    class WNDCLASSEXW(ctypes.Structure):
        _fields_ = [('cbSize', wt.UINT), ('style', wt.UINT), ('lpfnWndProc', WNDPROC),
                    ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                    ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON), ('hCursor', wt.HANDLE),
                    ('hbrBackground', wt.HBRUSH), ('lpszMenuName', wt.LPCWSTR),
                    ('lpszClassName', wt.LPCWSTR), ('hIconSm', wt.HICON)]

    struct = WNDCLASSEXW()
    struct.cbSize = ctypes.sizeof(WNDCLASSEXW)
    struct.lpfnWndProc = proc
    struct.hInstance = hInst
    struct.lpszClassName = cls_name
    atom = u32.RegisterClassExW(ctypes.byref(struct))
    print('    RegisterClassExW atom=%d' % atom)
    SHELLHOOK_MSG[0] = u32.RegisterWindowMessageW('SHELLHOOK')
    print('    RegisterWindowMessageW("SHELLHOOK") = 0x%04X' % SHELLHOOK_MSG[0])
    MSG_HWND = u32.CreateWindowExW(0, cls_name, 'probe', 0,
                                   0, 0, 0, 0, None, None, hInst, None)
    print('    CreateWindowExW hwnd=%s' % MSG_HWND)

    ok = False
    if MSG_HWND:
        r = u32.RegisterShellHookWindow(MSG_HWND)
        print('    RegisterShellHookWindow = %s (err=%d)' % (r, ctypes.get_last_error()))
        ok = bool(r)
    results.append(('RegisterShellHookWindow 成功', ok))

    # ---- 3) 制造事件：起一个记事本 ----
    print('\n[3] 制造事件：启动 notepad')
    nb = subprocess.Popen([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'),
                                        'System32', 'notepad.exe')])

    # 消息泵
    msg = wt.MSG()
    t0 = time.time()
    while time.time() - t0 < 4.0:
        got = False
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(ctypes.byref(msg))
            u32.DispatchMessageW(ctypes.byref(msg))
            got = True
        if not got:
            time.sleep(0.02)

    print('    收到的事件统计：')
    for ev, lst in sorted(_hits.items(), key=lambda kv: str(kv[0])):
        if isinstance(ev, tuple):
            print('      SHELL %-18s x%d' % (HSHELL.get(ev[1], hex(ev[1])), len(lst)))
        else:
            print('      win:%-14s x%d' % (EVENT_NAMES.get(ev, hex(ev)), len(lst)))

    results.append(('收到 win event 事件', len([k for k in _hits if not isinstance(k, tuple)]) >= 1))
    results.append(('收到 shell hook 事件', len([k for k in _hits if isinstance(k, tuple)]) >= 1))

    # ---- 4) 过滤规则与信息提取 ----
    print('\n[4] 从事件窗口提取信息（过滤规则 + pid/路径）')
    hwnds = set()
    for ev, lst in _hits.items():
        if not isinstance(ev, tuple):
            for h in lst:
                if h:
                    hwnds.add(h)
    print('    去重后窗口数 %d' % len(hwnds))
    shown = 0
    for h in list(hwnds):
        ok2, why = is_trackable(h)
        if not ok2:
            continue
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        p = proc_path(pid.value)
        if p and shown < 8:
            print('      hwnd=%-9s pid=%-6d %s' % (h, pid.value, os.path.basename(p)))
            shown += 1
    results.append(('能从事件窗口解析出 exe 路径', shown > 0))

    # ---- 收尾 ----
    print('\n[5] 收尾：注销钩子 / 关闭探针窗口 / 杀掉 notepad')
    for h in hooks:
        u32.UnhookWinEvent(h)
    if MSG_HWND:
        u32.DeregisterShellHookWindow(MSG_HWND)
        u32.DestroyWindow(MSG_HWND)
    try:
        nb.terminate()
    except Exception:
        pass

    print('\n================ 结果 ================')
    allok = True
    for name, r in results:
        print('  %s  %s' % ('PASS' if r else 'FAIL', name))
        allok = allok and r
    print('======================================')
    return 0 if allok else 1


u32.SetWinEventHook.argtypes = [wt.UINT, wt.UINT, wt.HMODULE, ctypes.c_void_p,
                                wt.DWORD, wt.DWORD, wt.UINT]
u32.SetWinEventHook.restype = wt.HANDLE
u32.UnhookWinEvent.argtypes = [wt.HANDLE]
u32.RegisterShellHookWindow.argtypes = [wt.HWND]
u32.DeregisterShellHookWindow.argtypes = [wt.HWND]
u32.RegisterWindowMessageW.argtypes = [wt.LPCWSTR]
u32.RegisterWindowMessageW.restype = wt.UINT
u32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]
u32.PeekMessageW.restype = wt.BOOL
u32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
u32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
u32.DispatchMessageW.restype = ctypes.c_long
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
u32.RegisterClassExW.restype = wt.WORD
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
u32.CreateWindowExW.restype = wt.HWND
u32.DestroyWindow.argtypes = [wt.HWND]
k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
k32.GetModuleHandleW.restype = wt.HINSTANCE

if __name__ == '__main__':
    sys.exit(main())
