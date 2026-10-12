"""定位 Dock 上"没有图标的应用"——复现 ZDock 的收录(shouldTrack)与分组(groupKeyOf)判定。

根因假说（来自 WindowTracker.cpp:266-272）：
    groupKeyOf 三级降级：aumid:  →  path:  →  hwnd:<地址>
    若 AUMID 与 exePath 都拿不到，分组键退化成 hwnd:，
    显示名退化成 "应用"（第 406 行），图标自然也没有。

本探针只读：EnumWindows 复现同一套过滤，列出所有"拿不到 exePath"的窗口
（= 必然退化成 hwnd: 分组）。不移动鼠标、不碰桌面。

跑法： <python> build-support/_probe_untracked_window.py
"""

import ctypes
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dwm = ctypes.WinDLL('dwmapi', use_last_error=True)

u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetWindow.argtypes = [wt.HWND, wt.UINT]
u32.GetWindow.restype = wt.HWND
u32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
u32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]
dwm.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]

GWL_EXSTYLE = -20
WS_EX_TOOLWINDOW = 0x00000080
DWMWA_CLOAKED = 14
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def exe_path_of(pid):
    """拿进程映像全路径；失败返回 (None, winerr)。"""
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None, ctypes.get_last_error()
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value, 0
        return None, ctypes.get_last_error()
    finally:
        k32.CloseHandle(h)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD),
                ('th32ProcessID', wt.DWORD), ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_wchar * 260)]


def image_name_of(pid):
    """用 Toolhelp 快照拿映像名——**不需要 OpenProcess**，权限被拒时照样能读到。"""
    TH32CS_SNAPPROCESS = 0x00000002
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wt.HANDLE(-1).value or not snap:
        return None
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        if k32.Process32FirstW(snap, ctypes.byref(e)):
            while True:
                if e.th32ProcessID == pid:
                    return e.szExeFile
                if not k32.Process32NextW(snap, ctypes.byref(e)):
                    break
        return None
    finally:
        k32.CloseHandle(snap)


def should_track(hwnd):
    """复现 WindowTracker::shouldTrack（WindowTracker.cpp:250-264）。"""
    if not u32.IsWindowVisible(hwnd):
        return False
    if u32.GetWindow(hwnd, 4):  # GW_OWNER
        return False
    if u32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW:
        return False
    cloaked = wt.DWORD(0)
    if dwm.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), 4) == 0 and cloaked.value:
        return False
    return True


tracked = []      # (hwnd, pid, exePath, title, cls)
orphans = []      # exePath 拿不到的


def cb(hwnd, _lp):
    if not should_track(hwnd):
        return True
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    path, err = exe_path_of(pid.value)
    cls = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, cls, 256)
    title = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(hwnd, title, 512)
    rec = (hwnd, pid.value, path, title.value, cls.value)
    tracked.append(rec)
    if not path:
        orphans.append(rec + (err,))
    return True


ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [ENUMPROC, wt.LPARAM]
u32.EnumWindows(ENUMPROC(cb), 0)

print(f'收录窗口总数：{len(tracked)}')
print(f'其中 exePath 拿不到（必然退化成 hwnd: 分组、"应用"名、无图标）：{len(orphans)}')
print('=' * 78)
for hwnd, pid, path, title, cls, err in orphans:
    print(f'  hwnd=0x{hwnd:08X} ({hwnd})  pid={pid}  OpenProcess/Query 错误码={err}')
    print(f'    映像名(免权限): {image_name_of(pid)}')
    print(f'    标题: {title!r}')
    print(f'    类名: {cls}')
print('=' * 78)
print('全部收录窗口：')
for hwnd, pid, path, title, cls in tracked:
    tag = 'OK ' if path else '?? '
    print(f'  {tag} pid={pid:<7} {path or "(拿不到路径)":<60} | {title[:40]!r}')
