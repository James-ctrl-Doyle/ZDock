"""验证"添加程序"：右键空白 -> 添加程序… -> 文件选择框 -> 选中 exe -> 落盘。

文件选择框（class #32770）是模态的，脚本这样走：
  1) 空白处右键 -> 全局菜单 -> 点"添加程序…"（第 1 项）
  2) 等 #32770 出现
  3) 在"文件名"编辑框里键入一个已知 exe 的完整路径，回车
  4) 读 config.json 确认多了一项

⚠ 隔离目录运行，不动用户配置。
⚠ 拍窗口一律 PrintWindow；这里不截图。
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
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.c_void_p]
u32.IsWindow.argtypes = [wt.HWND]
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
SYSROOT = os.environ.get('SystemRoot', r'C:\Windows')


def click(x, y):
    u32.SetCursorPos(int(x), int(y))
    time.sleep(0.25)
    u32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.06)
    u32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def type_text(s):
    """逐字符 keybd_event 打 ASCII（这里路径全是 ASCII）。"""
    VK = {}
    for c in 'abcdefghijklmnopqrstuvwxyz':
        VK[c] = ord(c.upper())
    for c in '0123456789':
        VK[c] = ord(c)
    VK[':'] = 0xBA     # OEM_1
    VK['\\'] = 0xDC    # OEM_5
    VK['.'] = 0xBE     # OEM_PERIOD
    VK['_'] = 0xBD     # OEM_MINUS
    VK['-'] = 0xBD
    for ch in s:
        vk = VK.get(ch.lower())
        if vk is None:
            continue
        shift = ch.isupper()
        if shift:
            u32.keybd_event(0x10, 0, 0, None)      # SHIFT down
        u32.keybd_event(vk, 0, 0, None)
        time.sleep(0.012)
        u32.keybd_event(vk, 0, 2, None)
        if shift:
            u32.keybd_event(0x10, 0, 2, None)
        time.sleep(0.012)


def main():
    work = tempfile.mkdtemp(prefix='zdock_add_')
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    exe = os.path.join(work, 'ZDock.exe')
    print('隔离目录: %s' % work)

    base = [os.path.join(SYSROOT, 'explorer.exe'),
            os.path.join(SYSROOT, 'System32', 'notepad.exe')]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'items': [{'path': p} for p in base]}, f, ensure_ascii=False, indent=2)

    want = os.path.join(SYSROOT, 'System32', 'mspaint.exe')
    print('准备添加: %s' % want)

    saved = wt.POINT()
    u32.GetCursorPos(ctypes.byref(saved))

    p = subprocess.Popen([exe], cwd=work)
    time.sleep(3.5)
    hwnd = u32.FindWindowW('ZDock', None)
    if not hwnd:
        print('窗口没起来')
        p.terminate()
        return 2

    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    W = rc.right - rc.left
    dpi = u32.GetDpiForWindow(hwnd) / 96.0

    # --- 空白处右键（面板最右侧的 padding 区，不在任何图标上）---
    kPadX, kHaloH, kPadY = 14, 72, 8
    panelW_logical = 2 * 48 + 1 * 12 + 2 * kPadX
    panel_left = (W - panelW_logical * dpi) / 2.0
    bx = rc.left + panel_left + (panelW_logical - 4) * dpi   # 最右 4 逻辑像素的 padding
    by = rc.top + (kHaloH + kPadY + 24) * dpi
    print('空白处右键于 screen=(%.0f,%.0f)' % (bx, by))

    cxp, cyp = int(bx - rc.left), int(by - rc.top)
    lp = (cyp << 16) | (cxp & 0xFFFF)
    # ⚠ 菜单弹出点用的是 GetCursorPos()（Ling 与我们都这么写），所以必须
    #   **真的把鼠标移过去**再投右键消息 —— 只 PostMessage 不移动光标的话，
    #   菜单会弹在光标残留的位置（脚本第一版就踩了这个，菜单跑到 (506,902)）。
    u32.SetCursorPos(int(bx), int(by))
    time.sleep(0.4)
    u32.PostMessageW(hwnd, 0x0204, 0, lp)
    time.sleep(0.1)
    u32.PostMessageW(hwnd, 0x0205, 0, lp)
    time.sleep(1.3)

    hmenu = u32.FindWindowW('#32768', None)
    if not hmenu:
        print('全局菜单没弹出')
        p.terminate()
        return 2
    mr = wt.RECT()
    u32.GetWindowRect(hmenu, ctypes.byref(mr))
    mh = mr.bottom - mr.top
    print('全局菜单 rect=(%d,%d,%d,%d) 高=%d' % (mr.left, mr.top, mr.right, mr.bottom, mh))

    # 菜单项：添加程序… / 重新载入配置 / --- / 退出 ZDock / --- / 版本
    # "添加程序…"是第 1 项 -> 中心 x 靠左内缩、y = top + item_h*0.5
    n_items = 6
    item_h = mh / n_items
    click(mr.left + 40, mr.top + item_h * 0.5)
    time.sleep(2.0)

    dlg = u32.FindWindowW('#32770', None)
    print('文件选择框 hwnd=%s' % dlg)
    if not dlg:
        print('对话框没出现 -> FAIL')
        u32.SetCursorPos(saved.x, saved.y)
        p.terminate()
        shutil.rmtree(work, ignore_errors=True)
        return 1

    # 直接找到"文件名"编辑框（ComboBoxEx32 -> ComboBox -> Edit），
    # 用 WM_SETTEXT 写路径 —— 比模拟键盘稳得多（不依赖焦点、不依赖输入法）。
    u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
    u32.FindWindowExW.restype = wt.HWND
    u32.SendMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, ctypes.c_void_p]

    def find_edit(root):
        """深度优先找第一个 Edit 控件。"""
        combo = u32.FindWindowExW(root, None, 'ComboBoxEx32', None)
        if combo:
            cb = u32.FindWindowExW(combo, None, 'ComboBox', None)
            if cb:
                ed = u32.FindWindowExW(cb, None, 'Edit', None)
                if ed:
                    return ed
        return u32.FindWindowExW(root, None, 'Edit', None)

    time.sleep(0.6)
    edit = find_edit(dlg)
    print('文件名编辑框 hwnd=%s' % edit)
    u32.SetForegroundWindow(dlg)
    time.sleep(0.3)
    if edit:
        u32.SendMessageW(edit, 0x000C, 0, ctypes.c_wchar_p(want))   # WM_SETTEXT
        time.sleep(0.3)
    else:
        print('  找不到编辑框 -> 回退模拟键盘')
        type_text(want)
        time.sleep(0.4)
    # 点对话框的"打开"按钮（IDOK=1），比回车更稳
    ok_btn = u32.GetDlgItem(dlg, 1)
    print('打开按钮 hwnd=%s' % ok_btn)
    if ok_btn:
        u32.SendMessageW(ok_btn, 0x00F5, 0, None)   # BM_CLICK
    else:
        u32.keybd_event(0x0D, 0, 0, None)
        time.sleep(0.08)
        u32.keybd_event(0x0D, 0, 2, None)
    time.sleep(2.5)

    alive = p.poll() is None
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    names = [os.path.basename(it['path']) for it in data['items']]
    print('存活=%s' % alive)
    print('添加后 items: %s' % json.dumps(names, ensure_ascii=False))

    log = os.path.join(work, 'ZDock.log')
    if os.path.exists(log):
        with open(log, 'r', encoding='utf-8', errors='replace') as f:
            for ln in [x.rstrip() for x in f][-8:]:
                print('  | ' + ln)

    ok = alive and len(names) == 3 and any('mspaint' in x.lower() for x in names)

    u32.SetCursorPos(saved.x, saved.y)
    try:
        p.terminate()
        time.sleep(1.0)
        if p.poll() is None:
            p.kill()
    except Exception:
        pass
    shutil.rmtree(work, ignore_errors=True)
    print('\n结果: %s' % ('PASS 添加程序落到 config.json' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
