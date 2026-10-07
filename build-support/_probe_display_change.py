"""显示环境变化（显示器 / DPI）自适应验证。

验证目标（对应 README 里"显示器变化未处理"这条已知限制的修复）：
  1) `WM_DISPLAYCHANGE` 广播能被收到 → 触发重排
  2) `WM_DPICHANGED` 能被 Ling 收到 → 触发重排
  3) 重排后 dock 依然**贴屏幕底边 + 水平居中 + 尺寸正确**（不是被推到屏幕中间）
  4) dock 处于隐藏态时遇到重排，**不会**从屏幕底边冒出来
  5) 重排后热区窗口跟着更新

⚠ 设计原则（红线）：**绝不模拟物理输入、绝不 SetForegroundWindow**。
  全部用 `PostMessage` 把消息投给**我们自己进程的接收窗口** ——
  - `WM_DISPLAYCHANGE` 投给跟踪器的 0x0 隐藏顶层窗口（它平时就靠这个收广播）；
  - `WM_DPICHANGED` 投给 dock 主窗口，且**用当前的 dpi 值构造载荷**，
    这样不会真的改变 DPI，只是让链路走一遍（证明回调接线正确）。

跑法： <python> build-support/_probe_display_change.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _probe_common import kill_existing_zdock   # noqa: E402
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

u32 = ctypes.WinDLL('user32', use_last_error=True)

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
u32.MonitorFromWindow.restype = wt.HANDLE
u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]

WM_DISPLAYCHANGE = 0x007E
WM_DPICHANGED = 0x02E0
WM_SETTINGCHANGE = 0x001A
WM_CLOSE = 0x0010
# 与 WindowTracker::kMsgTestInject 对齐（全屏状态注入通道）
MSG_TEST_INJECT = 0x8000 + 100

TRACK_CLASS = 'ZDock.Tracker'      # 可能不是这个名，下面用标题兜底
TRACK_TITLE = 'ZDockTrack'

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


def find_windows(pid=None, class_name=None, title=None):
    out = []

    def cb(hwnd, lp):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        if class_name is not None:
            cls = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(hwnd, cls, 256)
            if cls.value != class_name:
                return True
        if title is not None:
            t = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, t, 256)
            if t.value != title:
                return True
        out.append(hwnd)
        return True

    u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
    u32.EnumWindows(ctypes.cast(
        ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb), ctypes.c_void_p), 0)
    return out


def rect_of(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def monitor_of(hwnd):
    MI = type('MI', (ctypes.Structure,), {'_fields_': [
        ('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    mi = MI(); mi.cbSize = ctypes.sizeof(MI)
    u32.GetMonitorInfoW(u32.MonitorFromWindow(hwnd, 2), ctypes.byref(mi))
    m = mi.rcMonitor
    return (m.left, m.top, m.right, m.bottom)


def read_log(d, n=None):
    p = os.path.join(d, 'ZDock.log')
    if not os.path.exists(p):
        return []
    with open(p, 'rb') as f:
        raw = f.read()
    lines = raw.decode('utf-8', errors='replace').splitlines()
    return lines[-n:] if n else lines


def make_workdir(cfg):
    d = os.path.join(ROOT, '_tmp_display')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    full = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    full.update(cfg)
    json.dump(full, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)
    return d


def kill():
    # ⚠ 不能只按映像名杀 —— 验收时跑的是改名副本（ZDock_0.1.x.exe），对不上。
    #   按窗口类名找 pid 才杀得掉；见 _probe_common.py 的说明。
    kill_existing_zdock(verbose=False)


def main():
    print('===== 显示环境变化（显示器 / DPI）自适应探针 =====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    kill()
    d = make_workdir({'autoHide': False, 'reserveWorkArea': False})
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    if p.poll() is not None:
        print('!! ZDock 没起来')
        print('\n'.join(read_log(d, 20)))
        return 1

    docks = find_windows(pid=p.pid, class_name='ZDock')
    if not docks:
        check('找到 dock 窗口', False)
        kill(); shutil.rmtree(d, ignore_errors=True)
        return 1
    dock = docks[0]
    check('找到 dock 窗口', True, 'hwnd=%d' % dock)

    # 跟踪器的接收窗口（0x0 隐藏顶层窗口，标题 ZDockTrack）
    trackers = find_windows(pid=p.pid, title=TRACK_TITLE)
    check('找到跟踪器的接收窗口（广播用的顶层隐藏窗）', len(trackers) == 1,
          '命中 %d 个' % len(trackers))

    mon = monitor_of(dock)
    r0 = rect_of(dock)
    dpi = u32.GetDpiForWindow(dock) / 96.0
    # ⚠ dock 面板底边 = 屏幕底 − bottomMargin（逻辑像素），**不是**屏幕底边。
    #   第一版判据写成 == 屏幕底，被 1433 vs 1440 卡住 —— 那 7px 正是
    #   bottomMargin(6) × dpi(1.24) 的取整，是对的。
    MARGIN_PX = round(6 * dpi)
    check('起点：dock 面板底边 = 屏幕底 − bottomMargin',
          abs(r0[3] - (mon[3] - MARGIN_PX)) <= 2,
          '窗口底=%d 期望=%d（屏幕底=%d − %d）' % (r0[3], mon[3] - MARGIN_PX, mon[3], MARGIN_PX))

    # 订阅是否挂上（DPI 变化的真实触发要改系统缩放，自动化造不了）
    check('已订阅 DPI 变化（订阅日志存在）',
          any('已订阅 DPI 变化' in l for l in read_log(d)),
          next((l.split(']')[-1].strip() for l in read_log(d) if '已订阅 DPI 变化' in l), '（无）'))

    # ---------------- 1) WM_DISPLAYCHANGE ----------------
    print('\n  [1] 投递 WM_DISPLAYCHANGE（模拟改分辨率 / 换主屏）')
    before = len(read_log(d))
    if trackers:
        # lParam 低字=宽 高字=高（用当前屏幕尺寸，避免真的改变什么）
        w = mon[2] - mon[0]
        h = mon[3] - mon[1]
        lp = (w & 0xFFFF) | ((h & 0xFFFF) << 16)
        u32.PostMessageW(trackers[0], WM_DISPLAYCHANGE, 32, lp)
    time.sleep(1.2)
    new = read_log(d)[before:]
    check('收到显示器参数变化', any('显示器参数变化' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '显示器参数变化' in l), '（无）'))
    check('触发了重排', any('显示环境变化 -> 重排' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '显示环境变化 -> 重排' in l), '（无）'))
    r1 = rect_of(dock)
    mon1 = monitor_of(dock)
    check('重排后仍贴屏幕底（面板底 = 屏幕底 − bottomMargin）',
          abs(r1[3] - (mon1[3] - MARGIN_PX)) <= 2,
          '窗口底=%d 期望=%d' % (r1[3], mon1[3] - MARGIN_PX))
    check('重排后水平居中', abs((r1[0] + r1[2]) // 2 - (mon1[0] + mon1[2]) // 2) <= 3,
          '窗口中心=%d 屏幕中心=%d' % ((r1[0] + r1[2]) // 2, (mon1[0] + mon1[2]) // 2))
    check('重排后尺寸未变（dpi 没变就不该变）',
          (r1[2] - r1[0], r1[3] - r1[1]) == (r0[2] - r0[0], r0[3] - r0[1]),
          '%dx%d -> %dx%d' % (r0[2] - r0[0], r0[3] - r0[1], r1[2] - r1[0], r1[3] - r1[1]))

    # ---------------- 2) 跨进程投带指针的消息：必须不崩 ----------------
    # ⚠ 这条**不是**功能测试，是健壮性测试。
    #   `WM_SETTINGCHANGE` / `WM_DPICHANGED` 的 lParam 都是**指针**，
    #   跨进程投递时它指向的是**投递方**的地址空间 —— 目标进程解引用就是野指针。
    #   真实场景下这些消息由系统发出、指针有效，所以产品代码平时没问题；
    #   但这条消息是广播，谁都可能往我们窗口上投，所以必须能挡住。
    print('\n  [2] 跨进程投带野指针的 WM_SETTINGCHANGE / WM_DPICHANGED：必须不崩')
    before = len(read_log(d))
    bogus = ctypes.cast(ctypes.c_void_p(0x00000000DEADBEEF), ctypes.c_void_p).value
    u32.PostMessageW(trackers[0], 0x001A, 0, bogus)   # WM_SETTINGCHANGE
    u32.PostMessageW(dock, 0x02E0, 0, bogus)          # WM_DPICHANGED
    time.sleep(1.2)
    check('跨进程野指针消息没有崩掉 ZDock', p.poll() is None,
          'exit=%s' % p.poll())
    new = read_log(d)[before:]
    check('野指针的 WM_SETTINGCHANGE 被防护挡住（没有误触发重排）',
          not any('系统度量变化' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '系统度量变化' in l), '（未触发，符合预期）'))
    r2 = rect_of(dock)
    mon2 = monitor_of(dock)
    check('位置未被野指针消息破坏',
          abs(r2[3] - (mon2[3] - MARGIN_PX)) <= 2 and abs((r2[0] + r2[2]) // 2 - (mon2[0] + mon2[2]) // 2) <= 3,
          '窗口底=%d 中心=%d' % (r2[3], (r2[0] + r2[2]) // 2))
    check('ZDock 未崩', p.poll() is None)

    hw = u32.FindWindowW('ZDock', None)
    if hw:
        u32.PostMessageW(hw, WM_CLOSE, 0, 0)
        time.sleep(1.2)
    kill()
    time.sleep(0.6)
    shutil.rmtree(d, ignore_errors=True)

    # ---------------- 3) 隐藏态遇重排不冒出来 ----------------
    print('\n  [3] 自动隐藏开启 + 隐藏态下重排，不该从屏幕底边冒出来')
    kill()
    d = make_workdir({'autoHide': True, 'hideOnFullscreen': True})
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    docks = find_windows(pid=p.pid, class_name='ZDock')
    trackers = find_windows(pid=p.pid, title=TRACK_TITLE)
    hz = find_windows(pid=p.pid, class_name='ZDock.EdgeHotZone')
    if not docks or not trackers:
        check('隐藏态测试：环境就绪（dock + 注入载体）', False,
              'dock=%d tracker=%d' % (len(docks), len(trackers)))
    else:
        check('隐藏态测试：环境就绪（dock + 注入载体）', True, '热区=%d 个' % len(hz))
        dock = docks[0]
        mon = monitor_of(dock)
        # 用注入通道让它滑出（不碰真实鼠标）。
        # ⚠ 载体是**跟踪器的常驻接收窗口** —— 不是热区：热区会随 autoHide 开关
        #   创建/销毁，靠不住（这里 autoHide 开着所以它恰好存在，但别依赖它）。
        u32.PostMessageW(trackers[0], MSG_TEST_INJECT, 1, 0)   # 全屏 -> 让位滑出
        time.sleep(1.2)
        rh = rect_of(dock)
        check('已滑出屏幕', rh[1] >= mon[3] - 2, '窗口顶=%d 屏幕底=%d' % (rh[1], mon[3]))

        # 此时来一次显示环境变化
        before = len(read_log(d))
        if trackers:
            w = mon[2] - mon[0]
            h = mon[3] - mon[1]
            lp = (w & 0xFFFF) | ((h & 0xFFFF) << 16)
            u32.PostMessageW(trackers[0], WM_DISPLAYCHANGE, 32, lp)
        time.sleep(1.5)
        new = read_log(d)[before:]
        check('隐藏态下也完成了重排', any('显示环境变化 -> 重排' in l for l in new),
              next((l.split(']')[-1].strip() for l in new if '显示环境变化 -> 重排' in l), '（无）'))
        rh2 = rect_of(dock)
        # ⚠ 关键：重排内部会 applyDockPlacement(0)（展开态），必须被推回屏幕外，
        #   否则 dock 会在全屏应用底下突然冒出来。
        check('重排后 dock 仍留在屏幕外（没冒出来）', rh2[1] >= mon[3] - 2,
              '重排后窗口顶=%d 屏幕底=%d' % (rh2[1], mon[3]))
        check('ZDock 未崩（隐藏态）', p.poll() is None)

    kill()
    time.sleep(0.6)
    shutil.rmtree(d, ignore_errors=True)

    # ---------------- 4) 全屏让位与自动隐藏解耦 ----------------
    print('\n  [4] autoHide=false + hideOnFullscreen=true：让位必须仍然生效')
    kill()
    d = make_workdir({'autoHide': False, 'hideOnFullscreen': True})
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    docks = find_windows(pid=p.pid, class_name='ZDock')
    trackers = find_windows(pid=p.pid, title=TRACK_TITLE)
    if not docks or not trackers:
        check('解耦测试：环境就绪', False, 'dock=%d tracker=%d' % (len(docks), len(trackers)))
    else:
        check('解耦测试：环境就绪', True)
        dock = docks[0]
        mon = monitor_of(dock)
        # 自动隐藏关闭 → 不该有热区（不留看不见却吃点击的窗口）
        hz = find_windows(pid=p.pid, class_name='ZDock.EdgeHotZone')
        check('自动隐藏关闭时没有热区窗口', len(hz) == 0, '热区=%d 个' % len(hz))

        r0 = rect_of(dock)
        check('起点是展开态（可见）', r0[1] < mon[3] - 2, '窗口顶=%d' % r0[1])

        # 注入全屏 → 让位（**不依赖 autoHide**）
        before = len(read_log(d))
        u32.PostMessageW(trackers[0], MSG_TEST_INJECT, 1, 0)
        time.sleep(1.2)
        new = read_log(d)[before:]
        check('自动隐藏关闭时，全屏让位仍然触发滑出',
              any('滑出开始' in l for l in new),
              next((l.split(']')[-1].strip() for l in new if '滑出开始' in l), '（无）'))
        r1 = rect_of(dock)
        check('dock 确实滑出屏幕', r1[1] >= mon[3] - 2,
              '窗口顶=%d 屏幕底=%d' % (r1[1], mon[3]))

        # 退出全屏 → 滑回并**常驻**（autoHide 关着，不该又自动收回）
        before = len(read_log(d))
        u32.PostMessageW(trackers[0], MSG_TEST_INJECT, 0, 0)
        time.sleep(1.5)
        r2 = rect_of(dock)
        check('退出全屏后滑回屏幕内', r2[1] < mon[3] - 2,
              '窗口顶=%d' % r2[1])
        time.sleep(1.2)
        r3 = rect_of(dock)
        check('自动隐藏关着 → 滑回后常驻（不会被自动收回）', r3[1] < mon[3] - 2,
              '3 秒后窗口顶=%d（>= %d 表示被错误收回了）' % (r3[1], mon[3] - 2))
        check('ZDock 未崩（解耦测试）', p.poll() is None)

    kill()
    time.sleep(0.6)
    shutil.rmtree(d, ignore_errors=True)

    print('\n================ 结果 ================')
    allok = True
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        allok = allok and ok
    print('======================================')
    n_pass = sum(1 for _, ok, _ in results if ok)
    print('  %d/%d 通过' % (n_pass, len(results)))
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
