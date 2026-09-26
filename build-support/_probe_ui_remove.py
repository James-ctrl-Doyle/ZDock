"""真实 UI 验收：右键点图标 -> 弹出菜单 -> 点"从 Dock 移除" -> config.json 落盘。

`TrackPopupMenuEx` 是模态的，脚本没法"捕获"菜单窗口（它属于系统、不在本进程窗口树里，
PrintWindow 拍不到）。所以这里用**真实鼠标 + 键盘**：
  1) 把鼠标移到某个图标的中心（坐标由 GetWindowRect + 布局常量算出来）
  2) 右键 down -> 菜单弹出
  3) 用键盘 Down 走到"从 Dock 移除"（菜单项顺序固定：标题/分隔/打开/管理员打开/分隔/移除）
  4) 回车
  5) 读 config.json 确认少了一项

⚠ 全程在隔离目录里跑（ZDock.exe 是复制的），不动用户手上的配置。
⚠ 菜单项用键盘走：菜单里第 0 项是标题（不可点），所以"移除"是第 3 个可选项。
   稳妥起见这里用 End 键跳到**最后一项**（移除恰好在最后）。

跑法： <python> build-support/_probe_ui_remove.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRect.restype = wt.BOOL
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.POINTER(ctypes.c_ulong)]
u32.GetForegroundWindow.restype = wt.HWND
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
SYSROOT = os.environ.get('SystemRoot', r'C:\Windows')

VK_DOWN, VK_RETURN, VK_END = 0x28, 0x0D, 0x23
VK_RBUTTON = 0x02
KEYEVENTF_KEYUP = 0x0002


def key(vk, up=False):
    u32.keybd_event(vk, 0, KEYEVENTF_KEYUP if up else 0, None)


def tap(vk):
    key(vk)
    time.sleep(0.05)
    key(vk, up=True)
    time.sleep(0.08)


def main():
    if not os.path.isfile(SRC_EXE):
        print('找不到 ZDock.exe')
        return 2

    work = tempfile.mkdtemp(prefix='zdock_uirm_')
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    exe = os.path.join(work, 'ZDock.exe')
    print('隔离目录: %s' % work)

    items = [os.path.join(SYSROOT, 'explorer.exe'),
             os.path.join(SYSROOT, 'System32', 'notepad.exe'),
             os.path.join(SYSROOT, 'System32', 'mspaint.exe'),
             os.path.join(SYSROOT, 'System32', 'cmd.exe')]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'items': [{'path': p} for p in items]}, f, ensure_ascii=False, indent=2)

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    p = subprocess.Popen([exe], cwd=work)
    time.sleep(3.5)
    if p.poll() is not None:
        print('ZDock 没起来')
        return 2

    hwnd = u32.FindWindowW('ZDock', None)
    if not hwnd:
        print('找不到 ZDock 窗口')
        p.terminate()
        return 2

    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    dpi = u32.GetDpiForWindow(hwnd) / 96.0
    print('窗口 rect=(%d,%d,%d,%d) dpi=%.2f' % (rc.left, rc.top, rc.right, rc.bottom, dpi))

    # 布局常量（DockWin.cpp）：kSideSlack=48 kPadX=14 haloH=72 padY=8 iconGap=12
    kSideSlack, kPadX, kPadY, kHaloH, iconGap = 48, 14, 8, 72, 12
    iconSize = 48
    winW = rc.right - rc.left
    winH = rc.bottom - rc.top
    n = len(items)
    iconW_logical = n * iconSize + (n - 1) * iconGap
    panelW_logical = iconW_logical + 2 * kPadX
    panel_left_phys = (winW - panelW_logical * dpi) / 2.0
    row_left = panel_left_phys + kPadX * dpi
    icon_top = kHaloH * dpi + kPadY * dpi

    # 右击第 3 个图标（mspaint，index=2）的中心
    idx = 2
    cx = rc.left + row_left + (idx * (iconSize + iconGap) + iconSize / 2.0) * dpi
    cy = rc.top + icon_top + iconSize / 2.0 * dpi
    print('右键点 index=%d 于 client=(%.0f,%.0f) screen=(%.0f,%.0f)' % (idx, cx - rc.left, cy - rc.top, cx, cy))

    u32.SetCursorPos(int(cx), int(cy))
    time.sleep(0.4)
    # ⚠ lp 必须是 **client 坐标**：Ling 的 winProc 用 GET_X_LPARAM(lParam) 直接当
    #   客户区坐标传给 onMouseUp，传屏幕坐标会算出窗口外的点 → 命中不到图标。
    cxp, cyp = int(cx - rc.left), int(cy - rc.top)
    lp = (cyp << 16) | (cxp & 0xFFFF)
    print('  client=(%d,%d) lp=0x%08X' % (cxp, cyp, lp))
    u32.PostMessageW(hwnd, 0x0204, 0, lp)   # WM_RBUTTONDOWN
    time.sleep(0.1)
    u32.PostMessageW(hwnd, 0x0205, 0, lp)   # WM_RBUTTONUP
    time.sleep(1.2)   # 菜单已弹出并模态

    # 诊断：菜单弹出时前台窗口属于哪个进程（菜单是系统 #32768 类窗口）
    fg = u32.GetForegroundWindow()
    buf = ctypes.create_unicode_buffer(128)
    u32.GetClassNameW(fg, buf, 128)
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(fg, ctypes.byref(pid))
    print('  前台窗口 class=%r pid=%d (dock pid=%d)' % (buf.value, pid.value, p.pid))

    # 找菜单窗口（类名 #32768，属本线程）
    hmenu = u32.FindWindowW('#32768', None)
    print('  菜单窗口 hwnd=%s' % hmenu)

    if hmenu:
        mr = wt.RECT()
        u32.GetWindowRect(hmenu, ctypes.byref(mr))
        print('  菜单 rect=(%d,%d,%d,%d) 高=%d' % (mr.left, mr.top, mr.right, mr.bottom, mr.bottom - mr.top))
        # 菜单结构（自顶向下）：标题 / sep / 打开 / 管理员打开 / sep / 从 Dock 移除
        # 每项约 SM_CYMENU 高 + 上下各 3px 边距。用键盘更稳：
        # 先把鼠标移进菜单（建立 hover），再按 End + Enter。
        # ⚠ 菜单是 dock 线程的模态循环，keybd_event 需要该线程有输入焦点。
        #    dock 带 WS_EX_NOACTIVATE，可能拿不到焦点 → 用物理鼠标点击最稳。
        n_items = 6
        item_h = (mr.bottom - mr.top) / n_items
        # 点最后一项（从 Dock 移除）中心
        tx = mr.right - 20
        ty = int(mr.top + item_h * 5.5)
        print('  物理点击"从 Dock 移除"于 (%d,%d)' % (tx, ty))
        u32.SetCursorPos(tx, ty)
        time.sleep(0.25)
        u32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.06)
        u32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    else:
        print('  菜单没找到 -> 回退用键盘')
        tap(VK_END)
        tap(VK_RETURN)
    time.sleep(1.5)

    alive = p.poll() is None
    wins = 1 if u32.FindWindowW('ZDock', None) else 0
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    names = [os.path.basename(it['path']) for it in data['items']]
    print('存活=%s 窗口数=%d' % (alive, wins))
    print('移除后 config items: %s' % json.dumps(names, ensure_ascii=False))

    log = os.path.join(work, 'ZDock.log')
    if os.path.exists(log):
        with open(log, 'r', encoding='utf-8', errors='replace') as f:
            lines = [ln.rstrip() for ln in f]
        for ln in lines[-12:]:
            print('  | ' + ln)

    ok = alive and len(names) == 3 and not any('mspaint' in x.lower() for x in names)

    u32.SetCursorPos(saved.x, saved.y)
    try:
        p.terminate()
        time.sleep(1.0)
        if p.poll() is None:
            p.kill()
    except Exception:
        pass

    print('\n结果: %s' % ('PASS 右键移除落到 config.json' if ok else 'FAIL'))
    shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
