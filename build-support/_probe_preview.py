"""阶段五：悬停预览（任务书 §2 #17）验证。

验证目标：
  1) 悬停图标满 ~300ms → 弹出预览窗口
  2) 预览窗口里**真的有目标窗口的画面**（DWM 缩略图合成成功）
  3) 预览浮在图标**上方**
  4) 鼠标离开图标 → 预览收起
  5) 不悬停时**不创建**预览窗口（按需创建）

⚠ 怎么做成"零打扰"：
   探针自己开一个**品红底的普通窗口**当预览目标 —— ZDock 会把它当成"在跑但没固定"
   的应用补一个临时图标（config 里 items 留空，所以它就是唯一的图标，索引 0）。
   于是整个链路可以自动跑，**不需要启动 notepad 之类的程序**，也不会弹任何窗口
   （品红窗口放在屏幕左上角、160x120、SW_SHOWNOACTIVATE）。

⚠ 唯一必须动真实光标的地方是"让 ZDock 认为鼠标悬停在图标上" —— hover 判定读的是
   `GetCursorPos()`。所以这里用 `SetCursorPos`，但**用完立刻恢复**原位置，
   而且全程只有几百毫秒（和阶段一测悬停放大是同一套做法）。

跑法： <python> build-support/_probe_preview.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.GetWindowDC.argtypes = [wt.HWND]
u32.GetWindowDC.restype = wt.HDC
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p]
u32.CreateWindowExW.restype = wt.HWND
u32.DefWindowProcW.restype = ctypes.c_longlong
k32.GetModuleHandleW.restype = wt.HINSTANCE

g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g32.CreateCompatibleBitmap.restype = wt.HBITMAP
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.SelectObject.restype = wt.HGDIOBJ
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteDC.argtypes = [wt.HDC]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')

# 与 Src/DockWin.h 的布局常量一致（逻辑像素）
K_PAD_X, K_PAD_Y, K_HALO_H, K_SIDE_SLACK = 14.0, 8.0, 72.0, 48.0

MAGENTA = (255, 0, 255)

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wt.DWORD),
                ('biClrImportant', wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wt.DWORD * 3)]


g32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                          ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), ctypes.c_uint]


def count_color(hwnd, want, tol=40):
    """PrintWindow 拍窗口自身，数指定颜色的像素（-1 = 拍失败）。"""
    r = wt.RECT()
    if not u32.GetWindowRect(hwnd, ctypes.byref(r)):
        return -1
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return -1
    src = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(src)
    bmp = g32.CreateCompatibleBitmap(src, w, h)
    old = g32.SelectObject(mem, bmp)
    ok = bool(u32.PrintWindow(hwnd, mem, 2))
    hits = 0
    if ok:
        bi = BITMAPINFO()
        bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.bmiHeader.biWidth = w
        bi.bmiHeader.biHeight = -h
        bi.bmiHeader.biPlanes = 1
        bi.bmiHeader.biBitCount = 32
        bi.bmiHeader.biCompression = 0
        buf = ctypes.create_string_buffer(w * h * 4)
        if g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0):
            data = buf.raw
            for i in range(0, len(data) - 3, 4):
                if (abs(data[i + 2] - want[0]) < tol and abs(data[i + 1] - want[1]) < tol
                        and abs(data[i] - want[2]) < tol):
                    hits += 1
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, src)
    return hits if ok else -1


def find_windows(pid=None, class_name=None, title=None, visible_only=False):
    out = []

    def cb(hwnd, lp):
        if visible_only and not u32.IsWindowVisible(hwnd):
            return True
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        if class_name is not None:
            c = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(hwnd, c, 256)
            if c.value != class_name:
                return True
        if title is not None:
            t = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, t, 256)
            if t.value != title:
                return True
        out.append(hwnd)
        return True

    u32.EnumWindows(ctypes.cast(
        ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb), ctypes.c_void_p), 0)
    return out


def rect_of(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def _title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def find_temp_icon_index(lines, needle):
    """从 tracker 的"全量重建"日志里推出某个分组排第几 —— 那**就是**图标索引。

    ⚠ 为什么能这么推：`DockWin::syncWithTracker()` 里补临时图标是
      `for (const auto& g : tracker.groups())`，也就是完全按 tracker 的分组表顺序；
      而 tracker 在 `rebuildAll()` 里正是按同一顺序打 `分组 key=...` 日志。
      本机 config 里 items 是空的，所以图标索引 == 分组序号。

    ⚠ 不能假设"品红窗口就是索引 0"：这台机器上 WorkBuddy / Chrome / explorer
      / cmd 都在跑，它们都会变成临时图标（实测日志里有 5 个分组）。
    """
    start = 0
    for i, l in enumerate(lines):
        if '全量重建' in l:
            start = i
    keys = []
    for l in lines[start:]:
        if '分组 key=' in l:
            keys.append(l.split('分组 key=')[1].split(' ')[0].lower())
    for i, k in enumerate(keys):
        if needle.lower() in k:
            return i, keys
    return -1, keys


def read_log(d, n=None):
    p = os.path.join(d, 'ZDock.log')
    if not os.path.exists(p):
        return []
    with open(p, 'rb') as f:
        lines = f.read().decode('utf-8', errors='replace').splitlines()
    return lines[-n:] if n else lines


def kill_zdock():
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True)


# ---------------------------------------------------------------------------
# 品红窗口（预览目标）
# ---------------------------------------------------------------------------
CW = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


def _wndproc(h, m, w, l):
    if m == 0x000F:   # WM_PAINT
        # PAINTSTRUCT 在 x64 下约 72 字节，给足冗余
        ps = ctypes.create_string_buffer(128)
        dc = u32.BeginPaint(h, ps)
        r = wt.RECT()
        u32.GetClientRect(h, ctypes.byref(r))
        br = g32.CreateSolidBrush(0x00FF00FF)   # BGR 品红
        u32.FillRect(dc, ctypes.byref(r), br)
        g32.DeleteObject(br)
        u32.EndPaint(h, ps)
        return 0
    return u32.DefWindowProcW(h, m, w, l)


u32.TryGetWindowExStyle = getattr(u32, 'TryGetWindowExStyle', None)
# ⚠ 所有要传 lParam 的 API 都必须显式 argtypes，否则 64 位指针会在
#   ctypes 回调里被当成 int 溢出（"int too long to convert"）。
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.BeginPaint.argtypes = [wt.HWND, ctypes.c_void_p]
u32.BeginPaint.restype = wt.HDC
u32.EndPaint.argtypes = [wt.HWND, ctypes.c_void_p]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
g32.CreateSolidBrush.argtypes = [wt.DWORD]
g32.CreateSolidBrush.restype = wt.HGDIOBJ
u32.FillRect.argtypes = [wt.HDC, ctypes.c_void_p, wt.HGDIOBJ]

_target_hwnd = None


def start_target_window():
    """在**本线程**建一个品红普通窗口，返回 hwnd（消息泵由调用方负责）。"""
    global _target_hwnd
    proc = CW(_wndproc)
    # ⚠ 是 WNDCLASSEXW —— 第一个字段就是 cbSize，**必须**设置，
    #   否则 RegisterClassExW 直接失败（踩过：抄了个 WNDCLASS 旧结构，
    #   结果类没注册成功、CreateWindowEx 返回 NULL，症状是"窗口建不出来"）。
    WC = type('WNDCLASSEXW', (ctypes.Structure,), {'_fields_': [
        ('cbSize', wt.UINT), ('style', wt.UINT), ('lpfnWndProc', CW),
        ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
        ('hInstance', wt.HINSTANCE), ('hIcon', wt.HANDLE), ('hCursor', wt.HANDLE),
        ('hbrBackground', wt.HANDLE), ('lpszMenuName', wt.LPCWSTR),
        ('lpszClassName', wt.LPCWSTR), ('hIconSm', wt.HANDLE)]})
    wc = WC()
    wc.cbSize = ctypes.sizeof(WC)
    wc.lpfnWndProc = proc
    wc.hInstance = k32.GetModuleHandleW(None)
    wc.lpszClassName = 'ZDockPreviewTarget'
    if not u32.RegisterClassExW(ctypes.byref(wc)):
        err = ctypes.get_last_error()
        if err not in (1410,):    # ERROR_CLASS_ALREADY_EXISTS
            print('  RegisterClassExW 失败 err=%d' % err)
            return None
    _target_hwnd = u32.CreateWindowExW(0, 'ZDockPreviewTarget', 'PreviewTarget', 0x80000000,
                                       20, 20, 160, 120, None, None,
                                       k32.GetModuleHandleW(None), None)
    if _target_hwnd:
        u32.ShowWindow(_target_hwnd, 4)     # SW_SHOWNOACTIVATE
        u32.UpdateWindow(_target_hwnd)
    return _target_hwnd


def pump(ms):
    end = time.time() + ms / 1000.0
    msg = ctypes.create_string_buffer(64)
    while time.time() < end:
        while u32.PeekMessageW(msg, None, 0, 0, 1):
            u32.TranslateMessage(msg)
            u32.DispatchMessageW(msg)
        time.sleep(0.01)


def main():
    print('===== 阶段五：悬停预览 =====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    kill_zdock()
    d = os.path.join(ROOT, '_tmp_preview')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))

    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg['autoHide'] = False
    cfg['reserveWorkArea'] = False
    cfg['iconSize'] = 48
    cfg['iconGap'] = 12
    cfg['items'] = []          # 空 Dock：唯一的图标会是品红窗口那个"临时图标"（索引 0）
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    # 光标原位置（用完恢复）
    orig = wt.POINT()
    u32.GetCursorPos(ctypes.byref(orig))

    target = start_target_window()
    if not target:
        print('!! 品红目标窗口创建失败')
        return 1
    print('  预览目标（品红窗口）hwnd=%d' % target)

    zproc = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    if zproc.poll() is not None:
        check('ZDock 启动', False)
        print('\n'.join(read_log(d, 20)))
        return 1
    check('ZDock 启动', True, 'pid=%s' % zproc.pid)

    # dock 主窗口：类名 ZDock、**标题为空**（Ling 建的窗口没设标题）、可见。
    # ⚠ 预览窗口类名也是 ZDock（Ling 的窗口类名是全局统一的），靠标题区分：
    #   预览那个显式设了 "ZDockPreview"。
    all_zdock = find_windows(pid=zproc.pid, class_name='ZDock')
    docks = [w for w in all_zdock if _title_of(w) == '' and u32.IsWindowVisible(w)]
    if not docks:
        check('找到 dock 主窗口', False)
        print('  诊断：该 pid 的所有顶层窗口 ——')
        for w in find_windows(pid=zproc.pid):
            c = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(w, c, 256)
            print('    hwnd=%-10d cls=%-32s title=%-18s rect=%s vis=%s'
                  % (w, c.value, _title_of(w), rect_of(w), u32.IsWindowVisible(w)))
        kill_zdock(); pump(300); shutil.rmtree(d, ignore_errors=True)
        return 1
    dock = min(docks, key=lambda w: rect_of(w)[2] - rect_of(w)[0])
    check('找到 dock 主窗口', True, 'hwnd=%d rect=%s' % (dock, rect_of(dock)))
    dock = docks[0]
    check('找到 dock 主窗口', True, 'hwnd=%d' % dock)

    # 等 ZDock 把品红窗口加成临时图标，并从日志推出它的**图标索引**
    idx, keys = -1, []
    for _ in range(20):
        pump(250)
        idx, keys = find_temp_icon_index(read_log(d), 'python.exe')
        if idx >= 0:
            break
    check('ZDock 把品红窗口加成临时图标（分组表里能定位到它）', idx >= 0,
          '分组顺序=%s → 图标索引=%d' % (keys, idx))
    if idx < 0:
        print('  日志尾部：')
        for l in read_log(d, 10):
            print('    ' + l)
        kill_zdock(); pump(300); shutil.rmtree(d, ignore_errors=True)
        return 1

    dpi = u32.GetDpiForWindow(dock) / 96.0
    icon = float(cfg['iconSize'])
    gap = float(cfg['iconGap'])
    # 该索引的图标中心（客户区 → 屏幕）
    lx = K_SIDE_SLACK + K_PAD_X + idx * (icon + gap) + icon / 2.0
    ly = K_HALO_H + K_PAD_Y + icon / 2.0
    wr = rect_of(dock)
    cx = wr[0] + int(lx * dpi)
    cy = wr[1] + int(ly * dpi)
    print('  dpi=%.2f dock=%s 图标%d中心(屏幕)=(%d,%d)' % (dpi, wr, idx, cx, cy))

    # 悬停之前：不该有预览窗口
    pre = find_windows(pid=zproc.pid, title='ZDockPreview')
    check('悬停之前没有预览窗口（按需创建）', len(pre) == 0, '现有 %d 个' % len(pre))

    # ---- 悬停 300ms+ ----
    print('\n  [1] 把光标移到图标 0 上并等待 600ms')
    u32.SetCursorPos(cx, cy)
    ok_hover = False
    for _ in range(12):
        pump(100)
        if find_windows(pid=zproc.pid, title='ZDockPreview', visible_only=True):
            ok_hover = True
            break
    prev = find_windows(pid=zproc.pid, title='ZDockPreview', visible_only=True)
    check('悬停后弹出了预览窗口', len(prev) == 1, '可见预览窗口数=%d' % len(prev))

    if prev:
        pv = prev[0]
        pr, dr = rect_of(pv), rect_of(dock)
        # ⚠ 判据基准要一路往上对齐到**图标顶边**，不是 dock 窗口顶边、也不是面板顶边：
        #   dock 窗口顶 → +kHaloH(72) 透明预留区 → 面板顶 → +kPadY(8) 内边距 → **图标顶边**。
        #   预览贴在图标顶边是设计意图（锚点就是图标放大后的顶边中点，不该盖住图标）。
        #   前两版分别拿"窗口顶边"和"面板顶边"当基准，各差一层，都是假失败。
        icon_top = dr[1] + int(round((K_HALO_H + K_PAD_Y) * dpi))
        check('预览贴在图标上方（不盖住图标与面板）', pr[3] <= icon_top + 2,
              '预览底=%d 图标顶=%d（dock窗口顶=%d + halo %d + 内边距 %d）'
              % (pr[3], icon_top, dr[1],
                 int(round(K_HALO_H * dpi)), int(round(K_PAD_Y * dpi))))
        check('预览水平方向大致对齐图标',
              abs((pr[0] + pr[2]) // 2 - cx) <= 40,
              '预览中心=%d 图标x=%d' % ((pr[0] + pr[2]) // 2, cx))
        mag = count_color(pv, MAGENTA)
        check('预览窗口里出现了目标窗口的画面（DWM 缩略图生效）', mag > 500,
              '品红像素=%d（>500 说明画面上来了）' % mag)
        lg = read_log(d)
        check('日志确认预览已显示', any('显示预览' in l for l in lg),
              next((l.split(']')[-1].strip() for l in lg if '显示预览' in l), '（无）'))
        # 确认预览的**是品红窗口那个分组**（证明索引没算错）
        prev_line = next((l for l in lg if '显示预览' in l), '')
        check('预览的目标就是品红窗口所在分组（Python）',
              'Python' in prev_line or 'python' in prev_line.lower(),
              prev_line.split(']')[-1].strip()[:110] if prev_line else '（无）')
    else:
        check('预览贴在面板上方（不盖住图标与面板）', False, '没有预览窗口')
        check('预览窗口里出现了目标窗口的画面（DWM 缩略图生效）', False, '没有预览窗口')
        check('日志确认预览已显示', False, '没有预览窗口')

    # ---- 移开光标 → 预览收起 ----
    print('\n  [2] 把光标移开 → 预览应收起')
    u32.SetCursorPos(orig.x, orig.y)
    gone = False
    for _ in range(15):
        pump(100)
        if not find_windows(pid=zproc.pid, title='ZDockPreview', visible_only=True):
            gone = True
            break
    check('移开后预览收起', gone,
          '可见预览窗口数=%d' % len(find_windows(pid=zproc.pid, title='ZDockPreview', visible_only=True)))

    check('ZDock 未崩', zproc.poll() is None)

    # 收尾
    u32.SetCursorPos(orig.x, orig.y)
    u32.DestroyWindow(target)
    kill_zdock()
    pump(400)
    shutil.rmtree(d, ignore_errors=True)

    print('\n================ 结果 ================')
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
    print('======================================')
    n_pass = sum(1 for _, ok, _ in results if ok)
    print('  %d/%d 通过' % (n_pass, len(results)))
    return 0 if n_pass == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
