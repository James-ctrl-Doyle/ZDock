"""阶段四验收探针：自动隐藏 / 全屏让位 / 工作区预留（AppBar）/ explorer 重启自愈。

设计原则（与前几阶段一致，重要）：
  · **绝不模拟物理输入**（SetCursorPos / mouse_event / keybd_event）——
    那会抢走用户的 WorkBuddy 焦点、任务被取消。
  · 全部走两条不抢焦点的路：
      1) **日志取证**：状态变化（滑出 / 滑入 / AppBar 注册 / 自愈）都写日志；
      2) **PostMessage 注入**：把 WM_MOUSEMOVE 投递到**热区窗口**触发滑入
         （热区窗口是我们自己建的，PostMessage 足以驱动它的 wndProc）。
  · 不占屏：探针不额外铺窗口。

⚠ 自动隐藏 / 工作区预留**默认都是关的**，所以探针要写一份临时 config.json
  把开关打开。**全程在隔离临时目录里跑**（复制一份 exe + config 过去），
  不碰用户手上的 build/bin/config.json。

验证项：
  1) 热区窗口存在（自动隐藏开）且几何符合任务书 §4：3px 厚、宽 max(dock,屏宽/2)、贴屏幕底边
  2) 热区窗口不抢焦点（WS_EX_NOACTIVATE / TOOLWINDOW / TOPMOST / LAYERED）
  3) 投递 WM_MOUSEMOVE 到热区 → 日志出现"鼠标进入，触发滑入"
  4) 自动隐藏关闭时**没有**热区窗口（不留看不见的吃点击窗口）
  5) 全屏让位：日志有"全屏应用 进入/退出"（用全屏窗口触发；见下）
  6) AppBar 默认关时不注册；开启后注册成功 + 工作区底边被抬升
  7) 强杀后工作区能自然恢复（红线 7）
  8) 全过程 ZDock 不崩

跑法： <python> build-support/_probe_stage4.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _probe_common import kill_existing_zdock   # noqa: E402
import tempfile
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowExW.restype = wt.HWND
u32.IsWindow.argtypes = [wt.HWND]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
u32.GetWindowLongPtrW.restype = ctypes.c_longlong
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]

WM_MOUSEMOVE = 0x0200

SPI_GETWORKAREA = 0x0030
SPI_SETWORKAREA = 0x002F

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


def work_area():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)


def monitor_rect_of(hwnd):
    """窗口所在监视器的 **完整矩形**（rcMonitor）。

    ⚠ dock / 热区的定位基准是它，不是工作区 —— 工作区会被 AppBar 预留改掉，
      拿工作区当基准等于自引用（dock 每注册一次就往上爬一层）。实测踩过。
    """
    MONITOR_DEFAULTTONEAREST = 2
    MI = type('MI', (ctypes.Structure,), {'_fields_': [
        ('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    u32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
    u32.MonitorFromWindow.restype = wt.HANDLE
    mi = MI(); mi.cbSize = ctypes.sizeof(MI)
    mh = u32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
    if mh and u32.GetMonitorInfoW(mh, ctypes.byref(mi)):
        r = mi.rcMonitor
        return (r.left, r.top, r.right, r.bottom)
    return (0, 0, u32.GetSystemMetrics(0), u32.GetSystemMetrics(1))


def clean_work_area():
    """把工作区复位到"没有任何 AppBar 占用"的干净状态。

    ⚠ 为什么必须有这一步：AppBar 的占用是**全局状态**，强杀不会自动清
    （这正是红线 7 要处理的问题）。探针跑第二轮时如果起点是脏的，
    "抬升了多少像素""重启后是否叠加"这类判据全部会算错 —— 实测踩过。
    干净值 = 主监视器的 rcMonitor，只保留系统任务栏占的边（本机任务栏在
    顶部自动隐藏 → 上边 38px）。
    """
    mon = wt.RECT()
    MONITORINFO = type('MONITORINFO', (ctypes.Structure,), {
        '_fields_': [('cbSize', wt.DWORD), ('rcMonitor', wt.RECT),
                     ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]})
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    u32.MonitorFromPoint.argtypes = [wt.POINT, wt.DWORD]
    u32.MonitorFromPoint.restype = wt.HANDLE
    u32.GetMonitorInfoW.argtypes = [wt.HANDLE, ctypes.c_void_p]
    mon_handle = u32.MonitorFromPoint(wt.POINT(0, 0), 1)   # MONITOR_DEFAULTTOPRIMARY
    if u32.GetMonitorInfoW(mon_handle, ctypes.byref(mi)):
        r = mi.rcMonitor
        # 保留顶部 38px（本机任务栏位置）。这个值从"干净时的测量"来。
        r.top = 38
        u32.SystemParametersInfoW(SPI_SETWORKAREA, 0, ctypes.byref(r), 1)
        time.sleep(0.4)


def probe_shell_stale():
    """检测 shell 里是否残留着一条**死 AppBar 记录**。

    ⚠ 背景（实测）：AppBar 的注销（ABM_REMOVE）是应用自己的责任。进程被
      taskkill /F 之后 shell **不会**自动清掉那条记录，工作区会一直保持缩进
      （等 15 秒也不恢复）。死记录活在 `Shell_TrayWnd` 的内存态里 —— 注册表
      没地方存它，只有**重启 explorer** 才会清空。

      表现为：把工作区数值复位成干净值之后，**任何** AppBar 注册都会被 shell
      算成"残留 + 新"，占两道（本机实测 95px 变成 183px）。

    检测手段：在干净工作区上注册一个**最小 AppBar**（厚度 1px），看工作区被
    吃掉多少。正常应当 ≈ 1px；若明显更大（>30px）说明有残留。
    检测完立刻完整注销 + 复位工作区。
    """
    clean_work_area()
    before = work_area()

    # 建一个最小 AppBar 宿主窗口
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    CW = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
    u32.DefWindowProcW.restype = ctypes.c_longlong
    cb = CW(lambda h, m, w, l: u32.DefWindowProcW(h, m, w, l))
    WC = type('WC', (ctypes.Structure,), {'_fields_': [
        ('style', wt.UINT), ('lpfnWndProc', CW), ('cbClsExtra', ctypes.c_int),
        ('cbWndExtra', ctypes.c_int), ('hInstance', wt.HINSTANCE), ('hIcon', wt.HANDLE),
        ('hCursor', wt.HANDLE), ('hbrBackground', wt.HANDLE), ('lpszMenuName', wt.LPCWSTR),
        ('lpszClassName', wt.LPCWSTR)]})
    u32.RegisterClassExW.argtypes = [ctypes.c_void_p]
    u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p]
    u32.CreateWindowExW.restype = wt.HWND
    k32.GetModuleHandleW.restype = wt.HINSTANCE
    hInst = k32.GetModuleHandleW(None)
    wc = WC(); wc.lpfnWndProc = cb; wc.hInstance = hInst
    wc.lpszClassName = 'ZDockStaleProbe'
    u32.RegisterClassExW(ctypes.byref(wc))
    hwnd = u32.CreateWindowExW(0, 'ZDockStaleProbe', 't', 0, 0, 0, 0, 0, None, None, hInst, None)

    ABM_NEW, ABM_REMOVE, ABM_QUERYPOS, ABM_SETPOS = 0, 1, 2, 3
    ABE_BOTTOM = 3
    sh32 = ctypes.WinDLL('shell32', use_last_error=True)

    class APPBARDATA(ctypes.Structure):
        _fields_ = [('cbSize', wt.DWORD), ('hWnd', wt.HWND), ('uCallbackMessage', wt.UINT),
                    ('uEdge', wt.UINT), ('rc', wt.RECT), ('lParam', wt.LPARAM)]
    ab = APPBARDATA(); ab.cbSize = ctypes.sizeof(APPBARDATA); ab.hWnd = hwnd
    ab.uCallbackMessage = u32.RegisterWindowMessageW('ZDockStaleProbeNotify')
    sh32.SHAppBarMessage(ABM_NEW, ctypes.byref(ab))
    ab.uEdge = ABE_BOTTOM
    ab.rc = wt.RECT(before[0], before[3] - 1, before[2], before[3])
    sh32.SHAppBarMessage(ABM_QUERYPOS, ctypes.byref(ab))
    ab.rc.top = ab.rc.bottom - 1
    sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(ab))
    time.sleep(0.5)
    after = work_area()
    eaten = before[3] - after[3]

    # 立刻完整注销（SETPOS 空 + REMOVE），再复位工作区
    e = APPBARDATA(); e.cbSize = ctypes.sizeof(APPBARDATA); e.hWnd = hwnd
    e.uEdge = ABE_BOTTOM; e.rc = wt.RECT(0, 0, 0, 0)
    sh32.SHAppBarMessage(ABM_SETPOS, ctypes.byref(e))
    d = APPBARDATA(); d.cbSize = ctypes.sizeof(APPBARDATA); d.hWnd = hwnd
    sh32.SHAppBarMessage(ABM_REMOVE, ctypes.byref(d))
    u32.DestroyWindow(hwnd)
    time.sleep(0.4)
    clean_work_area()
    return eaten


def find_windows(pid=None, class_name=None, title=None, visible_only=True):
    """按 pid / 类名 / 标题枚举顶层窗口，返回 [(hwnd, cls, rect)]。"""
    found = []

    def cb(hwnd, lp):
        if visible_only and not u32.IsWindowVisible(hwnd):
            return True
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(hwnd, buf, 256)
        cls = buf.value
        if class_name is not None and cls != class_name:
            return True
        if title is not None:
            t = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, t, 256)
            if t.value != title:
                return True
        r = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        found.append((hwnd, cls, (r.left, r.top, r.right, r.bottom)))
        return True

    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    u32.EnumWindows(CB(cb), 0)
    return found


def make_workdir(cfg: dict):
    """把 exe 复制到临时目录 + 写一份 config.json（隔离，不碰用户的配置）。"""
    d = tempfile.mkdtemp(prefix='zdock_stage4_')
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    with open(os.path.join(d, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return d


def read_log(d, n=4000):
    p = os.path.join(d, 'ZDock.log')
    if not os.path.exists(p):
        return []
    try:
        with open(p, 'r', encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []


def start(d):
    proc = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(1.8)
    return proc


def kill(proc=None):
    if proc is not None:
        try:
            proc.terminate()
        except Exception:
            pass
    time.sleep(0.4)
    # ⚠ 收尾清理也要按窗口类名找 —— 用户验收时跑的是改名副本，
    #   `/IM ZDock.exe` 对不上、杀不掉，会留着把后续探针全挡在单实例外面。
    kill_existing_zdock(verbose=False)


BASE_CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150,
    "bottomMargin": 6, "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "items": [{"path": r"C:\Windows\explorer.exe"},
              {"path": r"C:\Windows\System32\notepad.exe"}],
}

# ===========================================================================
# A) 自动隐藏关闭（默认）→ 没有热区窗口
# ===========================================================================
def case_aoff():
    print('\n== A) 自动隐藏关闭（默认）==')
    kill()
    d = make_workdir(dict(BASE_CFG))
    proc = start(d)
    if proc.poll() is not None:
        check('ZDock 启动', False)
        return
    check('ZDock 启动', True, 'pid=%s' % proc.pid)

    hz = find_windows(pid=proc.pid, class_name='ZDock.EdgeHotZone')
    check('自动隐藏关时**没有**热区窗口（不留吃点击的隐形窗）', len(hz) == 0,
          '找到 %d 个' % len(hz))

    lines = read_log(d)
    check('日志说明自动隐藏未启用', any('自动隐藏未启用' in l for l in lines))

    kill(proc)
    shutil.rmtree(d, ignore_errors=True)


# ===========================================================================
# B) 自动隐藏开启 → 热区几何 + 不抢焦点 + 触发滑入
# ===========================================================================
def case_auto():
    print('\n== B) 自动隐藏开启 ==')
    kill()
    cfg = dict(BASE_CFG)
    cfg.update({"autoHide": True, "autoHideDelayMs": 500,
                "slideInMs": 200, "slideOutMs": 300, "hideOnFullscreen": True})
    d = make_workdir(cfg)
    proc = start(d)
    if proc.poll() is not None:
        check('ZDock 启动', False)
        print('\n'.join('    ' + l for l in read_log(d, 25)))
        return
    check('ZDock 启动', True, 'pid=%s' % proc.pid)

    lines = read_log(d)
    check('日志说明自动隐藏已启用', any('自动隐藏已启用' in l for l in lines),
          next((l.split(']')[-1].strip() for l in lines if '自动隐藏已启用' in l), ''))

    hz = find_windows(pid=proc.pid, class_name='ZDock.EdgeHotZone')
    check('热区窗口存在', len(hz) == 1, '找到 %d 个' % len(hz))
    if not hz:
        kill(proc)
        shutil.rmtree(d, ignore_errors=True)
        return

    hzhwnd, _, hr = hz[0]
    wa = work_area()
    mon = monitor_rect_of(hzhwnd)
    thick = hr[3] - hr[1]
    width = hr[2] - hr[0]
    screen_w = mon[2] - mon[0]

    check('热区厚 3px（任务书 §4）', thick == 3, '实测 %dpx' % thick)
    # ⚠ 热区贴的是**屏幕**（rcMonitor）底边，不是工作区底边。
    #   理由同 dock 定位：工作区会被 AppBar 预留改掉，热区跟着爬就够不着了。
    check('热区贴屏幕底边', hr[3] == mon[3],
          '热区底=%d 屏幕底=%d（工作区底=%d）' % (hr[3], mon[3], wa[3]))
    check('热区宽 >= 屏宽/2', width >= screen_w // 2,
          '热区宽=%d 屏宽=%d（屏宽/2=%d）' % (width, screen_w, screen_w // 2))
    check('热区宽 >= dock 面板宽（探针用近似：热区宽 > 200）', width > 200, '宽=%d' % width)

    # 扩展样式
    ex = u32.GetWindowLongPtrW(hzhwnd, -20)   # GWL_EXSTYLE
    WS_EX_TOPMOST = 0x00000008
    WS_EX_TOOLWINDOW = 0x00000080
    WS_EX_NOACTIVATE = 0x08000000
    WS_EX_LAYERED = 0x00080000
    check('热区不抢焦点（NOACTIVATE）', bool(ex & WS_EX_NOACTIVATE), 'ex=0x%08X' % ex)
    check('热区不进 Alt+Tab（TOOLWINDOW）', bool(ex & WS_EX_TOOLWINDOW))
    check('热区置顶（TOPMOST）', bool(ex & WS_EX_TOPMOST))
    check('热区分层可用（LAYERED，用于全透明）', bool(ex & WS_EX_LAYERED))

    # ---- 投递 WM_MOUSEMOVE 到热区 → 应触发滑入 ----
    print('\n  [B2] 向热区投递 WM_MOUSEMOVE（不移动真实光标）')
    before = len(read_log(d))
    u32.PostMessageW(hzhwnd, WM_MOUSEMOVE, 0, 0)
    time.sleep(0.8)
    new = read_log(d)[before:]
    check('热区收到鼠标进入并打日志',
          any('鼠标进入' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '鼠标进入' in l), '（无）'))
    check('ZDock 仍存活', proc.poll() is None)

    kill(proc)
    shutil.rmtree(d, ignore_errors=True)


# ===========================================================================
# C) 工作区预留（AppBar）：默认关 → 开启后注册并缩进工作区
# ===========================================================================
def case_appbar():
    print('\n== C) 工作区预留（AppBar）==')
    kill()
    # ⚠ 起点必须是干净的，否则"抬升多少"算不准（实测踩过：脏起点下量出 183px，
    #   看着像"叠加了两份"，其实是 residue 没清）。
    #   清理分两步：
    #     1) probe_shell_stale() —— 走一次 NEW/SETPOS/REMOVE 循环把 shell 里的
    #        **死 AppBar 记录**扫掉（实测这一步对吃回 0~1px 有决定性作用）；
    #     2) clean_work_area() —— 把工作区数值写回干净值。
    eaten0 = probe_shell_stale()   # 内部已含 clean_work_area()
    if eaten0 > 20:
        print('  ⚠ 环境里仍有 %dpx 的 AppBar 残留（多次强杀累积）；'
              '本段判据可能偏大。重启 explorer 可彻底清空。' % eaten0)
    clean_work_area()
    print('  环境自检：干净工作区上注册 1px AppBar 只吃掉 %dpx' % eaten0)

    # C1) 默认关（不预留）：工作区不被我们改
    d = make_workdir(dict(BASE_CFG))
    proc = start(d)
    if proc.poll() is not None:
        check('C1 ZDock 启动', False)
        return
    wa_before = work_area()
    lines = read_log(d)
    check('C1 默认不注册 AppBar（日志无 [appbar] 已注册）',
          not any('[appbar] 已注册' in l for l in lines))
    kill(proc)
    time.sleep(0.5)
    wa_after = work_area()
    check('C1 关闭时不动工作区', wa_before[3] == wa_after[3],
          '启动前底=%d 关闭后底=%d' % (wa_before[3], wa_after[3]))
    shutil.rmtree(d, ignore_errors=True)
    time.sleep(0.5)

    # C2) 开启预留
    clean_work_area()
    wa_before = work_area()
    cfg = dict(BASE_CFG)
    cfg.update({"reserveWorkArea": True})
    d = make_workdir(cfg)
    proc = start(d)
    if proc.poll() is not None:
        check('C2 ZDock 启动', False)
        print('\n'.join('    ' + l for l in read_log(d, 25)))
        return
    check('C2 ZDock 启动', True, 'pid=%s' % proc.pid)

    lines = read_log(d)
    ok_reg = any('[appbar] 已注册' in l for l in lines)
    check('C2 注册为 AppBar 成功', ok_reg,
          next((l.split(']')[-1].strip() for l in lines if '[appbar] 已注册' in l), '（无）'))

    time.sleep(1.0)
    wa_reserved = work_area()
    check('C2 工作区底边被抬升（dock 占了一块）', wa_reserved[3] < wa_before[3],
          '预留前底=%d 预留后底=%d（抬升 %dpx）'
          % (wa_before[3], wa_reserved[3], wa_before[3] - wa_reserved[3]))
    # ⚠ 这条是**回归哨兵**：曾经因为"用工作区当定位基准"（自引用反馈回路），
    #   dock 每注册一次就往上爬一层，实测抬升 183px = 两层。修好后应当
    #   ≈ 单份面板高度（95px）。这里用宽区间卡住"只占一份"。
    lift_c2 = wa_before[3] - wa_reserved[3]
    check('C2 只占一份（没有被自引用回路叠加）', 0 < lift_c2 <= 120,
          '实际抬升 %dpx（单份面板高约 95px；>150 说明叠加了）' % lift_c2)

    # ---- 红线 7：强杀必须能恢复 ----
    #
    # ⚠ 这里有个**实测出来的系统限制**（见 _probe_appbar_recover.py）：
    #   AppBar 的注销（ABM_REMOVE）是应用自己的责任，进程被 taskkill /F 之后
    #   shell **不会**自动清掉那条记录 —— 工作区会一直保持缩进（实测等 15 秒也不恢复）。
    #   而且"让一个新 AppBar 走一遍 NEW/SETPOS/REMOVE"也清不掉别人的记录。
    #
    #   ZDock 的解法：注册前把"干净工作区"存进 `zdock-appbar.state`，
    #   正常退出删掉它。下次启动若发现文件还在，说明上次被强杀了 →
    #   用 SPI_SETWORKAREA 写回备份值（只写自己记的值，不碰别人的）。
    print('\n  [C3] 强杀 ZDock（红线 7：不留半占用状态 → 下次启动自愈）')
    check('C3 状态文件已生成（记录抢占前的工作区）',
          os.path.exists(os.path.join(d, 'zdock-appbar.state')))

    kill(proc)          # taskkill /F：不给 ABM_REMOVE 的机会
    time.sleep(1.0)
    wa_killed = work_area()
    check('C3 强杀后工作区确实还处于缩进（复现了系统限制）',
          wa_killed[3] == wa_reserved[3],
          '强杀后底=%d（预留时 %d）' % (wa_killed[3], wa_reserved[3]))

    # 再启动一次 → 启动自愈应当把工作区拉回来再重新占用
    proc2 = start(d)
    lines2 = read_log(d)
    heal_line = next((l for l in lines2 if '强杀自愈' in l), None)
    check('C3 重启后日志有强杀自愈', heal_line is not None,
          (heal_line.split(']')[-1].strip()[:100] if heal_line else '（无）'))
    # 自愈日志里应当出现备份的干净值（本机 = 底边 1440）
    if heal_line:
        restored_ok = ('(2560,1440)' in heal_line) or ('2560,1440' in heal_line)
        check('C3 自愈写回了备份的干净工作区', restored_ok,
              heal_line.split(']')[-1].strip()[:100])

    time.sleep(1.0)
    wa_restart = work_area()
    # 重启后 ZDock 会走自愈：① 用备份值把工作区写回干净值；
    # ② 跑一次 NEW/SETPOS(空)/REMOVE 把 shell 里的死记录扫掉；③ 再正常注册。
    # 正确结果：工作区 = 干净值 − **单份**占用（不是两份）。
    lines2b = read_log(d)
    purged = any('清残留' in l for l in lines2b)
    lift = wa_before[3] - wa_restart[3]
    check('C3 重启后只占一份（死记录被扫掉，没叠加）',
          0 < lift <= 120,
          '重启后底=%d 干净底=%d（占 %dpx；>150 说明叠加了）'
          % (wa_restart[3], wa_before[3], lift))
    check('C3 自愈时走了"清残留"循环', purged,
          next((l.split(']')[-1].strip() for l in lines2b if '清残留' in l), '（无）'))

    # 正常退出 → 应当彻底恢复
    hwnd2 = u32.FindWindowW('ZDock', None)
    if hwnd2:
        u32.PostMessageW(hwnd2, 0x0010, 0, 0)   # WM_CLOSE
        time.sleep(1.5)
    kill(proc2)
    time.sleep(1.0)
    wa_final = work_area()
    check('C3 正常退出后工作区彻底恢复 + 状态文件被删',
          wa_final[3] == wa_before[3] and not os.path.exists(os.path.join(d, 'zdock-appbar.state')),
          '退出后底=%d 原始底=%d 状态文件=%s'
          % (wa_final[3], wa_before[3],
             '在' if os.path.exists(os.path.join(d, 'zdock-appbar.state')) else '已删'))

    shutil.rmtree(d, ignore_errors=True)
    clean_work_area()


# ===========================================================================
# D) explorer 重启自愈（TaskbarCreated）
# ===========================================================================
def case_taskbar():
    print('\n== D) explorer 重启自愈（TaskbarCreated 广播）==')
    kill()
    cfg = dict(BASE_CFG)
    cfg.update({"autoHide": True, "reserveWorkArea": True})
    d = make_workdir(cfg)
    proc = start(d)
    if proc.poll() is not None:
        check('ZDock 启动', False)
        return
    check('ZDock 启动', True, 'pid=%s' % proc.pid)

    lines = read_log(d)
    check('启动时注册了 TaskbarCreated 消息',
          any('taskbarCreatedMsg' in l for l in lines),
          next((l.split(']')[-1].strip() for l in lines if 'taskbarCreatedMsg' in l), ''))

    # 广播 TaskbarCreated 给所有顶层窗口（这是 explorer 重启时系统做的事）
    print('\n  广播 TaskbarCreated 给所有顶层窗口（模拟 explorer 重启）')
    before = len(read_log(d))
    msg = u32.RegisterWindowMessageW('TaskbarCreated')
    u32.PostMessageW(0xFFFF, msg, 0, 0)   # HWND_BROADCAST
    time.sleep(1.5)

    new = read_log(d)[before:]
    check('收到 TaskbarCreated 广播', any('收到 TaskbarCreated 广播' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '收到 TaskbarCreated' in l), '（无）'))
    check('自愈完成（重注册 AppBar + 跟踪器）', any('自愈完成' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '自愈完成' in l), '（无）'))
    check('自愈后 ZDock 仍存活', proc.poll() is None)

    hz = find_windows(pid=proc.pid, class_name='ZDock.EdgeHotZone')
    check('自愈后热区窗口仍在（自动隐藏没被搞坏）', len(hz) == 1)

    kill(proc)
    time.sleep(1.0)
    shutil.rmtree(d, ignore_errors=True)


# ===========================================================================
# E) 全屏让位（任务书 §3："全屏应用前台 → 保持隐藏"）
# ===========================================================================
WM_APP = 0x8000
MSG_TEST_INJECT = WM_APP + 100    # 与 EdgeHotZone::kMsgTestInject 对齐


def dock_rect(pid):
    """取 dock 主窗口的屏幕矩形（用于判断"滑出 / 滑入"）。"""
    ws = find_windows(pid=pid, class_name='ZDock')
    return ws[0][2] if ws else None


def case_fullscreen():
    print('\n== E) 全屏让位 ==')
    print('  ⚠ 用"注入"驱动，不走 SetForegroundWindow —— 那会抢用户的输入焦点')
    kill()
    clean_work_area()
    cfg = dict(BASE_CFG)
    cfg.update({"autoHide": True, "hideOnFullscreen": True})
    d = make_workdir(cfg)
    proc = start(d)
    if proc.poll() is not None:
        check('E ZDock 启动', False)
        return
    check('E ZDock 启动', True, 'pid=%s' % proc.pid)

    # 注入载体 = 跟踪器那个**常驻**的 0x0 隐藏接收窗口（标题 ZDockTrack）。
    # ⚠ 以前挂在热区窗口上，但热区会随 autoHide 开关创建/销毁 ——
    #   全屏让位与自动隐藏解耦后，"不自动隐藏但要让位"的组合下根本没有热区。
    # 它不可见，所以 visible_only=False。
    tk = find_windows(pid=proc.pid, title='ZDockTrack', visible_only=False)
    if not tk:
        check('E 注入载体（跟踪器接收窗口）就绪', False)
        kill(proc)
        shutil.rmtree(d, ignore_errors=True)
        return
    inject_hwnd = tk[0][0]
    check('E 注入载体（跟踪器接收窗口）就绪', True, 'hwnd=%d' % inject_hwnd)

    rect0 = dock_rect(proc.pid)
    check('E 起点：dock 处于展开态', rect0 is not None)
    if not rect0:
        kill(proc); shutil.rmtree(d, ignore_errors=True); return
    mon = monitor_rect_of(inject_hwnd)

    # ---- 进入"全屏" → dock 应当滑出屏幕 ----
    before = len(read_log(d))
    u32.PostMessageW(inject_hwnd, MSG_TEST_INJECT, 1, 0)
    time.sleep(1.2)     # 滑出 300ms + 余量
    new = read_log(d)[before:]
    check('E 全屏进入：日志有 [dock] 全屏应用 进入',
          any('全屏应用 进入' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '全屏应用 进入' in l), '（无）'))
    check('E 全屏进入：触发滑出', any('滑出开始' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '滑出开始' in l), '（无）'))
    rect_fs = dock_rect(proc.pid)
    check('E 全屏进入：dock 确实滑出屏幕（顶边 >= 屏幕底 - 2）',
          rect_fs is not None and rect_fs[1] >= mon[3] - 2,
          '滑出后窗口顶=%s 屏幕底=%d' % (rect_fs[1] if rect_fs else '?', mon[3]))
    # 热区保留（设计如此：全屏时它已被 shouldHideNow 挡住，不会把 dock 拱出来；
    # 留着还能保住自动化测试的注入通道）。
    hz_fs = find_windows(pid=proc.pid, class_name='ZDock.EdgeHotZone')
    check('E 全屏进入：热区仍在（靠 shouldHideNow 拦住，不靠销毁）', len(hz_fs) == 1,
          '热区窗口数=%d' % len(hz_fs))

    # ---- 全屏期间碰热区也不该弹出来 ----
    before = len(read_log(d))
    if hz_fs:
        u32.PostMessageW(hz_fs[0][0], WM_MOUSEMOVE, 0, 0)
        time.sleep(1.0)
        new2 = read_log(d)[before:]
        rect_still = dock_rect(proc.pid)
        check('E 全屏期间碰热区：dock 保持隐藏（不拱出全屏画面）',
              rect_still is not None and rect_still[1] >= mon[3] - 2,
              '碰热区后窗口顶=%s' % (rect_still[1] if rect_still else '?'))

    # ---- 退出"全屏" → dock 应当滑回来 ----
    before = len(read_log(d))
    u32.PostMessageW(inject_hwnd, MSG_TEST_INJECT, 0, 0)
    time.sleep(0.35)    # 只等滑入动画（200ms）+ 一点余量：
                        # ⚠ 等太久的话，滑入结束后的"鼠标不在 dock 上 → 自动收回"
                        #   会把它收回去，这时再断言"在屏幕内"就是错的
                        #   （实测踩过：等 1.2s 拿到顶=1440，看着像没滑回）。
    new = read_log(d)[before:]
    check('E 全屏退出：日志有 [dock] 全屏应用 退出',
          any('全屏应用 退出' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '全屏应用 退出' in l), '（无）'))
    check('E 全屏退出：触发了滑入', any('滑入开始' in l for l in new),
          next((l.split(']')[-1].strip() for l in new if '滑入开始' in l), '（无）'))
    rect_back = dock_rect(proc.pid)
    check('E 全屏退出：dock 滑回屏幕内（顶边 < 屏幕底）',
          rect_back is not None and rect_back[1] < mon[3] - 2,
          '滑回后窗口顶=%s' % (rect_back[1] if rect_back else '?'))

    # 滑入后若鼠标不在 dock 上，应当自动收回（tickSlide 尾部的补判）
    time.sleep(1.5)
    rect_after = dock_rect(proc.pid)
    check('E 滑入后鼠标不在 dock 上 → 自动收回（不赖着不走）',
          rect_after is not None and rect_after[1] >= mon[3] - 2,
          '收回后窗口顶=%s（>= %d 表示已收回）' % (rect_after[1] if rect_after else '?', mon[3] - 2))

    check('E 全过程 ZDock 未崩', proc.poll() is None)
    kill(proc)
    time.sleep(1.0)
    shutil.rmtree(d, ignore_errors=True)
    clean_work_area()


def main():
    print('===== 阶段四探针：自动隐藏 / 全屏让位 / 工作区预留 =====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    kill()
    case_aoff()
    case_auto()
    case_appbar()
    case_taskbar()
    case_fullscreen()
    kill()

    print('\n================ 结果 ================')
    allok = True
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        allok = allok and ok
    n_pass = sum(1 for _, ok, _ in results if ok)
    print('======================================')
    print('  %d/%d 通过' % (n_pass, len(results)))
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
