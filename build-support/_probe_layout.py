"""阶段六：位置配置（停靠边 / 对齐 / 沿边偏移）验证。

覆盖：
  · 四个停靠边各一条：面板该贴哪条屏幕边、是否换轴（左右停靠时窗口变成竖的）
  · 三种对齐（start / center / end）在 bottom 边上的位置
  · dockOffset 生效
  · 热区跟着停靠边走（贴对应的屏幕边）
  · opacity 变更真的写进了生效配置

⚠ 判定用的"面板矩形"= 窗口矩形 + panelOrigin×dpi：
   dock 窗口比面板大一圈（留白给图标放大和指示器用），各家停靠边的留白位置不同 ——
   **不能拿窗口矩形当面板矩形**（阶段六的预览位置探针就为这事连踩两次假失败）。
   面板长宽是**换轴**的：上下停靠时宽 = 图标排、左右停靠时高 = 图标排。

⚠ 全程离屏验证，不模拟输入、不抢焦点。

跑法： <python> build-support/_probe_layout.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

u32 = ctypes.WinDLL('user32', use_last_error=True)

u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
u32.MonitorFromWindow.restype = wt.HANDLE
u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

# 与 Src/DockWin.h 的布局常量保持一致（逻辑像素）
K_PAD_X, K_PAD_Y, K_HALO_H, K_SIDE_SLACK = 14.0, 8.0, 72.0, 48.0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')

MARGIN = 6.0          # config 里的 bottomMargin
ICON, GAP = 48.0, 12.0
N_ITEMS = 4           # 测试用固定 4 项

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


def find_windows(pid=None, class_name=None, title=None, visible_only=True):
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


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def monitor_of(hwnd):
    MI = type('MI', (ctypes.Structure,), {'_fields_': [
        ('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    mi = MI(); mi.cbSize = ctypes.sizeof(MI)
    u32.GetMonitorInfoW(u32.MonitorFromWindow(hwnd, 2), ctypes.byref(mi))
    m = mi.rcMonitor
    return (m.left, m.top, m.right, m.bottom)


def panel_origin(edge):
    """面板左上角相对窗口左上角的偏移（逻辑像素）—— 与 DockWin::panelOrigin 一致。"""
    return {
        'bottom': (K_SIDE_SLACK, K_HALO_H),
        'top':    (K_SIDE_SLACK, 0.0),
        'left':   (0.0, K_SIDE_SLACK),
        'right':  (K_HALO_H, K_SIDE_SLACK),
    }[edge]


def actual_item_count(lines, fallback=N_ITEMS):
    """从日志里读 dock **实际**显示的项数。

    ⚠ 不能拿 config 里的项数当实际项数：在跑但没被固定的应用会被补成**临时图标**
      （本机 WorkBuddy / Chrome / Steam / 游戏 一堆），dock 会从 4 项涨到八九项，
      面板宽度也随之变 —— 第一版探针按 config 的 4 项算面板矩形，
      于是"居中"和"端对齐"全判成了假失败。
    """
    for l in reversed(lines):
        if 'Dock 共' in l and '项' in l:
            try:
                seg = l.split('Dock 共')[1]
                return int(seg.strip().split(' ')[0])
            except Exception:                      # noqa: BLE001
                pass
    return fallback


def panel_rect(hwnd, edge, dpi, n_items):
    """把窗口矩形换算成**面板**矩形（物理像素）。"""
    wr = rect_of(hwnd)
    ox, oy = panel_origin(edge)
    along = n_items * ICON + (n_items - 1) * GAP + 2 * K_PAD_X
    across = ICON + 2 * K_PAD_Y + 4 + 3      # kIndicatorDia + kIndicatorGap
    pw = along if edge in ('bottom', 'top') else across
    ph = across if edge in ('bottom', 'top') else along
    left = wr[0] + int(round(ox * dpi))
    top = wr[1] + int(round(oy * dpi))
    return (left, top, left + int(round(pw * dpi)), top + int(round(ph * dpi)))


def read_log(d, n=None):
    p = os.path.join(d, 'ZDock.log')
    if not os.path.exists(p):
        return []
    with open(p, 'rb') as f:
        lines = f.read().decode('utf-8', errors='replace').splitlines()
    return lines[-n:] if n else lines


def kill():
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True)


def run_case(edge, align, offset=0.0, opacity=0.8, auto_hide=False):
    """起一份 ZDock，返回 (窗口矩形, 面板矩形, dpi, 屏幕矩形, 热区矩形, 日志, 实际项数)。"""
    kill()
    d = os.path.join(ROOT, '_tmp_layout')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))

    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg.update({'autoHide': auto_hide, 'reserveWorkArea': False, 'iconSize': ICON, 'iconGap': GAP,
                'dockEdge': edge, 'dockAlign': align, 'dockOffset': offset, 'opacity': opacity})
    # 固定 4 项，保证面板尺寸可预测
    win = os.environ.get('WINDIR', r'C:\Windows')
    cand = [os.path.join(win, 'explorer.exe'),
            os.path.join(win, 'System32', 'notepad.exe'),
            os.path.join(win, 'System32', 'mspaint.exe'),
            os.path.join(win, 'System32', 'cmd.exe')]
    cfg['items'] = [{'path': p} for p in cand if os.path.exists(p)][:N_ITEMS]
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.8)          # 等 tracker 建完临时图标、dock 完成最终重排
    if p.poll() is not None:
        return None
    docks = [w for w in find_windows(pid=p.pid, class_name='ZDock') if title_of(w) == '']
    hz = find_windows(pid=p.pid, class_name='ZDock.EdgeHotZone')
    if not docks:
        subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
        return None
    dock = docks[0]
    dpi = u32.GetDpiForWindow(dock) / 96.0
    lg = read_log(d)
    n = actual_item_count(lg)
    res = (rect_of(dock), panel_rect(dock, edge, dpi, n), dpi,
           monitor_of(dock), rect_of(hz[0]) if hz else None, lg, n)
    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.4)
    shutil.rmtree(d, ignore_errors=True)
    return res


def main():
    print('===== 阶段六：位置配置（停靠边 / 对齐 / 偏移）=====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    # ---- 四个停靠边（center 对齐）----
    for edge in ('bottom', 'top', 'left', 'right'):
        print('\n  [%s + center]' % edge)
        r = run_case(edge, 'center')
        if not r:
            check('%s 启动并找到窗口' % edge, False)
            continue
        wr, pr, dpi, mon, hzr, lg, n = r
        m = int(round(MARGIN * dpi))
        pwpx, phpx = pr[2] - pr[0], pr[3] - pr[1]
        print('    [debug] dpi=%.2f 项数=%d 屏幕=%s 窗口=%s 面板=%s (宽%d 高%d)'
              % (dpi, n, mon, wr, pr, pwpx, phpx))

        # 换轴：左右停靠时面板应该"高 > 宽"
        if edge in ('left', 'right'):
            check('%s 面板换轴（竖起来）' % edge, phpx > pwpx,
                  '面板 %dx%d（宽应小于高）' % (pwpx, phpx))
        else:
            check('%s 面板不换轴（横着）' % edge, pwpx > phpx,
                  '面板 %dx%d' % (pwpx, phpx))

        # 贴边
        if edge == 'bottom':
            check('bottom 面板底边贴屏幕底 − margin', abs(pr[3] - (mon[3] - m)) <= 2,
                  '面板底=%d 期望=%d' % (pr[3], mon[3] - m))
            check('bottom 面板水平居中', abs((pr[0] + pr[2]) // 2 - (mon[0] + mon[2]) // 2) <= 3,
                  '面板中心x=%d 屏幕中心x=%d' % ((pr[0] + pr[2]) // 2, (mon[0] + mon[2]) // 2))
        elif edge == 'top':
            check('top 面板顶边贴屏幕顶 + margin', abs(pr[1] - (mon[1] + m)) <= 2,
                  '面板顶=%d 期望=%d' % (pr[1], mon[1] + m))
            check('top 面板水平居中', abs((pr[0] + pr[2]) // 2 - (mon[0] + mon[2]) // 2) <= 3,
                  '面板中心x=%d' % ((pr[0] + pr[2]) // 2))
        elif edge == 'left':
            check('left 面板左边贴屏幕左 + margin', abs(pr[0] - (mon[0] + m)) <= 2,
                  '面板左=%d 期望=%d' % (pr[0], mon[0] + m))
            check('left 面板垂直居中', abs((pr[1] + pr[3]) // 2 - (mon[1] + mon[3]) // 2) <= 3,
                  '面板中心y=%d' % ((pr[1] + pr[3]) // 2))
        else:
            check('right 面板右边贴屏幕右 − margin', abs(pr[2] - (mon[2] - m)) <= 2,
                  '面板右=%d 期望=%d' % (pr[2], mon[2] - m))
            check('right 面板垂直居中', abs((pr[1] + pr[3]) // 2 - (mon[1] + mon[3]) // 2) <= 3,
                  '面板中心y=%d' % ((pr[1] + pr[3]) // 2))

        # 热区不在这里测：热区只在 autoHide 开启时存在，而那个模式下 dock 会因为
        # 鼠标不在上面而滑出、位置断言全废。热区单独用一个用例测（见文件末尾）。

    # ---- 对齐（在 bottom 边上测）----
    for align in ('start', 'end'):
        print('\n  [bottom + %s]' % align)
        r = run_case('bottom', align)
        if not r:
            check('bottom+%s 启动' % align, False)
            continue
        wr, pr, dpi, mon, _, _, _ = r
        if align == 'start':
            check('start 面板左边界贴屏幕左边', abs(pr[0] - mon[0]) <= 2,
                  '面板左=%d 屏幕左=%d' % (pr[0], mon[0]))
        else:
            check('end 面板右边界贴屏幕右边', abs(pr[2] - mon[2]) <= 2,
                  '面板右=%d 屏幕右=%d' % (pr[2], mon[2]))

    # ---- 沿边偏移 ----
    print('\n  [bottom + center + offset=120]')
    r = run_case('bottom', 'center', offset=120.0)
    if not r:
        check('offset 用例启动', False)
    else:
        _, pr, dpi, mon, _, _, _ = r
        want = (mon[0] + mon[2]) // 2 + int(round(120.0 * dpi))
        check('offset 把面板沿边推移了 120 逻辑像素',
              abs((pr[0] + pr[2]) // 2 - want) <= 3,
              '面板中心=%d 期望=%d' % ((pr[0] + pr[2]) // 2, want))

    # ---- 不透明度写进配置并生效 ----
    print('\n  [opacity=0.55]')
    r = run_case('bottom', 'center', opacity=0.55)
    if not r:
        check('opacity 用例启动', False)
    else:
        _, _, _, _, _, lg, _ = r
        check('opacity 被正确载入', any('opacity=0.55' in l for l in lg),
              next((l.split(']')[-1].strip()[:90] for l in lg if 'opacity=' in l), '（无）'))

    # ---- 热区跟着停靠边走（要 autoHide 开，否则热区窗口根本不存在）----
    print('\n  [left + autoHide：热区该贴屏幕左边]')
    r = run_case('left', 'center', auto_hide=True)
    if not r:
        check('left 热区用例启动', False)
    else:
        _, _, _, mon, hzr, _, _ = r
        if hzr:
            check('left 热区贴屏幕左边', abs(hzr[0] - mon[0]) <= 1,
                  '热区左=%d 屏幕左=%d' % (hzr[0], mon[0]))
            check('left 热区换轴成竖条', (hzr[3] - hzr[1]) > (hzr[2] - hzr[0]),
                  '热区 %dx%d' % (hzr[2] - hzr[0], hzr[3] - hzr[1]))
        else:
            check('left 热区存在', False, '没找到热区窗口')

    print('\n================ 结果 ================')
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
    print('======================================')
    n_pass = sum(1 for _, ok, _ in results if ok)
    print('  %d/%d 通过' % (n_pass, len(results)))
    return 0 if n_pass == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
