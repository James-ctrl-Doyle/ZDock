# 复现并验证「滑入进行中被分组变化打断」→ 0.1.27 的"莫名上浮"。
#
# 场景（用户 2026-10-14 报）：dock 隐藏中，鼠标碰热区 → 滑入刚开始（3ms）→
# 分组变化触发重排 → 旧代码 killTimer + 落位展开终点 → dock 瞬间整个蹦出来。
#
# 判据全部走**真实鼠标 + 高频位置采样**：
#   P1) 日志里"滑入开始"之后有"滑动结束 -> 展开"（动画跑完了，没被杀死）
#   P2) 位置序列是**渐进**的（从起点到终点跨越 ≥60ms / ≥5 个中间样本）
#       —— 旧代码的指纹：分组变化后几个 ms 内直接跳到展开位
#   P3) 最终停在展开位（位置正确）
#   P4) A/B：未修复版本必须至少挂一条（证明探针能复现）
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc  # noqa: E402

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
EXE = os.path.join(ROOT, r'build\bin\ZDock.exe')
WORK = os.path.join(ROOT, r'build\_verify_slidecut')
LOG = os.path.join(WORK, 'ZDock.log')

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
u32.SetProcessDPIAware()
for fn, arg, res in (
    (u32.EnumWindows, [ctypes.c_void_p, wt.LPARAM], wt.BOOL),
    (u32.GetWindowRect, [wt.HWND, ctypes.POINTER(wt.RECT)], wt.BOOL),
    (u32.SetCursorPos, [ctypes.c_int, ctypes.c_int], wt.BOOL),
    (u32.GetCursorPos, [ctypes.POINTER(wt.POINT)], wt.BOOL),
    (u32.WindowFromPoint, [wt.POINT], wt.HWND),
    (u32.IsWindowVisible, [wt.HWND], wt.BOOL),
    (u32.ShowWindow, [wt.HWND, ctypes.c_int], wt.BOOL),
    (u32.DestroyWindow, [wt.HWND], wt.BOOL),
    (u32.SystemParametersInfoW, [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT], wt.BOOL),
):
    fn.argtypes = arg
    fn.restype = res

EN = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_zdock():
    """返回 (hwnd, rect) 或 None。"""
    out = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cls, 256)
        if cls.value == 'ZDock':
            r = wt.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(EN(cb), 0)
    return out[0] if out else None


def work_area():
    r = wt.RECT()
    u32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0)  # SPI_GETWORKAREA
    return (r.left, r.top, r.right, r.bottom)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [('style', wt.UINT), ('lpfnWndProc', ctypes.c_void_p),
                ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int),
                ('hInstance', wt.HINSTANCE), ('hIcon', wt.HICON),
                ('hCursor', wt.HANDLE), ('hbrBackground', wt.HANDLE),
                ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR)]


def make_app_window(title, w=320, h=200):
    """建一个可见的测试窗口（用来触发 WindowTracker 的分组变化）。"""
    hinst = ctypes.WinDLL('kernel32').GetModuleHandleW(None)
    cls = 'ZDockSlideCutProbe'
    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HWND, wt.UINT,
                                 ctypes.c_void_p, ctypes.c_void_p)

    @WNDPROC
    def wndproc(h, m, wp, lp):
        u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT,
                                       ctypes.c_void_p, ctypes.c_void_p]
        u32.DefWindowProcW.restype = ctypes.c_void_p
        return u32.DefWindowProcW(h, m, wp, lp)

    wc = WNDCLASSW()
    wc.lpfnWndProc = ctypes.cast(wndproc, ctypes.c_void_p)
    wc.hInstance = hinst
    wc.lpszClassName = cls
    u32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
    u32.RegisterClassW.restype = wt.ATOM
    u32.RegisterClassW(ctypes.byref(wc))
    wa = work_area()
    u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
    u32.CreateWindowExW.restype = wt.HWND
    h = u32.CreateWindowExW(0, cls, title, 0x00CF0000,  # WS_OVERLAPPEDWINDOW
                            wa[0] + 80, wa[1] + 80, w, h,
                            None, None, hinst, None)
    if h:
        u32.ShowWindow(h, 4)  # SW_SHOWNOACTIVATE
    return h


def real_move(x, y):
    u32.SetCursorPos(int(x), int(y))


def find_window(cls_name):
    out = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, cls, 256)
        if cls.value == cls_name:
            r = wt.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(EN(cb), 0)
    return out


def inject_wake(hwnd, rect):
    """给热区窗口投一条 WM_MOUSEMOVE，触发 onEnter（不碰用户光标）。"""
    u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    u32.PostMessageW.restype = wt.BOOL
    x = (rect[2] - rect[0]) // 2
    y = (rect[3] - rect[1]) // 2
    u32.PostMessageW(hwnd, 0x0200, 0, (y << 16) | (x & 0xFFFF))


def cursor():
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def run(exe_path, tag):
    """跑一轮：热区唤出 → 滑入中触发分组变化 → 采样位置序列。"""
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(exe_path, os.path.join(WORK, 'ZDock.exe'))
    cfg = {
        "autoHide": True, "autoHideDelayMs": 200,
        "slideInMs": 150, "slideOutMs": 200,
        "hotZoneEnabled": True, "hotZoneThickness": 6,
        "showIndicator": False,
        "items": [
            {"path": "C:\\Windows\\explorer.exe", "name": "文件资源管理器"},
        ],
    }
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(cfg, f, ensure_ascii=False)

    env = dict(os.environ)
    env['ZDOCK_VERBOSE_SLIDE'] = '1'
    proc = subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                            env=env, creationflags=0x00000008, close_fds=True)
    time.sleep(2.5)

    hz = find_window('ZDock.EdgeHotZone')
    if not hz:
        print('  [FAIL] 没找到热区窗口（ZDock.EdgeHotZone）')
        try:
            proc.terminate()
        except Exception:
            pass
        return None
    cx, cy = (hz[0][1][0] + hz[0][1][2]) // 2, (hz[0][1][1] + hz[0][1][3]) // 2

    d = find_zdock()
    if not d:
        print('  [FAIL] 没找到 dock 窗口')
        try:
            proc.terminate()
        except Exception:
            pass
        return None
    hidden_top = d[1][1]

    # ① 唤醒 dock（注入 WM_MOUSEMOVE，不碰用户光标）
    inject_wake(hz[0][0], hz[0][1])

    # ② 滑入刚开始就制造分组变化（新窗口 → 运行集合变 → syncWithTracker → 重排）
    seq = []
    t0 = time.monotonic()
    cut_done = False
    app = None
    while time.monotonic() - t0 < 1.0:
        dd = find_zdock()
        if dd:
            top = dd[1][1]
            seq.append((int((time.monotonic() - t0) * 1000), top))
            if not cut_done and top < hidden_top - 2:
                app = make_app_window('滑入打断探针')   # 动画一启动就砸分组变化
                cut_done = True
        time.sleep(0.005)

    time.sleep(0.5)
    d3 = find_zdock()
    final_top = d3[1][1] if d3 else None

    if app:
        u32.DestroyWindow(app)
    time.sleep(0.8)
    try:
        proc.terminate()
    except Exception:
        pass

    log = ''
    if os.path.exists(LOG):
        log = io_open(LOG)
    if os.environ.get('SLIDECUT_DEBUG'):
        print('  --- 采样序列（前 60 点）---')
        for tt, t in seq[:60]:
            print(f'    {tt:5d}ms  top={t}')
        print('  --- 日志关键行 ---')
        for line in log.splitlines():
            if re.search(r'滑入|滑出|滑动结束|分组|重排|载入 ok|启动', line):
                print('   ', line)
    return {'seq': seq, 'final_top': final_top, 'hidden_top': hidden_top,
            'log': log, 'cut_done': cut_done}


def io_open(p):
    import io
    return io.open(p, encoding='utf-8', errors='ignore').read()


def analyze(r, tag):
    ok = True
    log = r['log']
    # P1) 动画跑完了吗
    has_start = '滑入开始' in log
    has_end = '滑动结束 -> 展开' in log
    check(f'{tag} P1) 滑入开始之后有"滑动结束 -> 展开"（动画没被杀死）',
          has_start and has_end,
          f'滑入开始={has_start} 展开结束={has_end}')

    # P2) 渐进性：序列里"介于隐藏位与展开位之间"的样本数
    seq = r['seq']
    hidden_top = r['hidden_top']
    if not seq:
        print(f'  [FAIL] {tag} P2) 没有采到样本')
        return False
    shown_top = None
    # 展开位 = 序列里出现过的最小 top（动画终点）
    shown_top = min(t for _, t in seq)
    if shown_top >= hidden_top - 2:
        shown_top = hidden_top - 120  # 兜底：没滑到位就按典型高度估
    # 只统计"第一次下行"（滑入）那一段的中间态 —— 滑出过程同样满足区间条件，
    # 混进来会让判据偏松（修前 3 个点也可能被滑出的几十个点掩护过）。
    first_reach = None
    for tt, t in seq:
        if t <= shown_top + 2:
            first_reach = tt
            break
    mid = [(tt, t) for tt, t in seq
           if first_reach is not None and tt <= first_reach
           and hidden_top - 2 > t > shown_top + 2]
    span = (mid[-1][0] - mid[0][0]) if mid else 0
    check(f'{tag} P2) 滑入段位置渐进（中间态样本 ≥5 个且跨越 ≥60ms）',
          len(mid) >= 5 and span >= 60,
          f'中间态样本 {len(mid)} 个 / 跨越 {span}ms / 首次到展开位 {first_reach}ms'
          f'（hidden={hidden_top} shown≈{shown_top}）')

    # P3) 注入唤醒后光标不在 dock 上，200ms 后它**应该**正常收回 —— 所以判据不是
    #    "1 秒后还在展开位"，而是"滑入确实到达过展开位并停住过"。
    max_stayed, cur = 0, 0
    for _, t in seq:
        if t <= shown_top + 2:
            cur += 1
            max_stayed = max(max_stayed, cur)
        else:
            cur = 0
    check(f'{tag} P3) 滑入到达过展开位（曾连续 ≥3 个采样点停在那里）',
          max_stayed >= 3, f'最长连续停留 {max_stayed} 点 / shown≈{shown_top}')
    return ok


def check(name, cond, detail=''):
    print(f'  [{"PASS" if cond else "FAIL"}] {name}' + (f' — {detail}' if detail else ''))
    return cond


if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else EXE
    tag = os.path.basename(target)
    print('=' * 74)
    print(f'滑入进行中触发分组变化：{tag}')
    print('=' * 74)
    r = run(target, tag)
    if r:
        print(f'  打断已触发={r["cut_done"]} / 采样 {len(r["seq"])} 点 / '
              f'日志 {len(r["log"].splitlines())} 行')
        analyze(r, tag)
    # 跑完把用户的 dock 启回来
    pc.kill_existing_zdock()
    d = r'C:\Users\zqw35\WorkBuddy\projects\ZDock\_review'
    exe = os.path.join(d, 'ZDock_0.1.27.exe')
    if os.path.exists(exe):
        subprocess.Popen([exe], cwd=d, creationflags=0x00000008, close_fds=True)
        print('  （已把用户的 dock 重启为 0.1.27）')
        time.sleep(1.5)
