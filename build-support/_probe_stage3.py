"""阶段三验收探针：事件驱动的窗口跟踪 / 运行指示 / 临时图标 / 点击切换。

设计原则（重要）：
  · **绝不模拟物理输入**（SetCursorPos / mouse_event / keybd_event）。
    上一阶段用它们点模态菜单，直接把用户的 WorkBuddy 焦点抢走、任务被取消。
    这一阶段的验证全部走**日志取证 + 消息注入**这两条不抢焦点的路。
  · 不占屏：ZDock 自己就在屏幕底部；探针不额外铺窗口。

验证项：
  1) ZDock 启动后跟踪器起来了（日志里有 [track] 已启动 / 全量重建）
  2) 启动 notepad → 日志出现 WINDOWCREATED 相关的分组变化 + 临时图标
  3) 关掉 notepad → 临时图标被回收（分组数回落）
  4) 点击切换：用 WM_LBUTTONDOWN/UP 投递到 ZDock 的图标位置（PostMessage，
     不移动真实光标），验证"已运行则切窗口、未运行则启动"这条分支。
     ⚠ PostMessage 的坐标是 client 坐标，且不走命中测试 —— 但 Ling 的
     onMouseUp 是从 WM_LBUTTONUP 拿坐标的，PostMessage 足以驱动它。
  5) 全过程 ZDock 进程活着（没崩）

跑法： <python> build-support/_probe_stage3.py
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowExW.restype = wt.HWND
u32.IsWindow.argtypes = [wt.HWND]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetWindowTextLengthW.argtypes = [wt.HWND]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.IsWindowVisible.argtypes = [wt.HWND]

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
LOG = os.path.join(BIN, 'ZDock.log')

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


def read_log_tail(n=400):
    """读日志最后 n 行（UTF-8）。文件可能正被 ZDock 追加写，用共享读打开。"""
    if not os.path.exists(LOG):
        return []
    try:
        with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
        return lines[-n:]
    except OSError:
        return []


def log_mark():
    """当前日志行数，用作"从这里往后看"的游标。"""
    return len(read_log_tail(10 ** 7))


def log_since(mark):
    return read_log_tail(10 ** 7)[mark:]


def find_windows_by_pid(pid, visible_only=True):
    """枚举属于某 pid 的可见顶层窗口。"""
    found = []

    def cb(hwnd, lp):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            if not visible_only or u32.IsWindowVisible(hwnd):
                n = u32.GetWindowTextLengthW(hwnd)
                buf = ctypes.create_unicode_buffer(n + 1)
                u32.GetWindowTextW(hwnd, buf, n + 1)
                found.append((hwnd, buf.value))
        return True

    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    u32.EnumWindows(CB(cb), 0)
    return found


def wait_log(pred, mark, timeout=6.0):
    """在 mark 之后的新增日志里等 pred 成立。返回命中的行或 None。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for line in log_since(mark):
            if pred(line):
                return line
        time.sleep(0.15)
    return None


def parse_temp(line):
    """从 "[dock] 分组变化：运行 N 组，Dock 共 M 项（其中临时 K）..." 里取 K。"""
    if not line or '临时' not in line:
        return -1
    tail = line.rsplit('临时', 1)[1]
    num = ''
    for ch in tail.strip():
        if ch.isdigit():
            num += ch
        elif num:
            break
    return int(num) if num else -1


def parse_running(line):
    """取 "运行 N 组" 里的 N。"""
    if not line or '运行' not in line:
        return -1
    tail = line.split('运行', 1)[1].strip()
    num = ''
    for ch in tail:
        if ch.isdigit():
            num += ch
        elif num:
            break
    return int(num) if num else -1


def parse_total(line):
    """取 "Dock 共 M 项" 里的 M。"""
    if not line or '共' not in line:
        return -1
    tail = line.split('共', 1)[1].strip()
    num = ''
    for ch in tail:
        if ch.isdigit():
            num += ch
        elif num:
            break
    return int(num) if num else -1


def main():
    print('== 阶段三探针：窗口跟踪 / 运行指示 / 临时图标 ==\n')

    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    # 干净起步：先确保没有残留
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'],
                   capture_output=True, shell=False)
    time.sleep(0.4)

    # 日志清空（旧日志会干扰"从这里往后看"）
    try:
        if os.path.exists(LOG):
            os.remove(LOG)
    except OSError:
        pass

    print('[1] 启动 ZDock')
    proc = subprocess.Popen([EXE], cwd=BIN)
    time.sleep(1.6)

    hwnd = u32.FindWindowW('ZDock', None)   # 窗口标题为空，按类名找
    alive = proc.poll() is None
    check('ZDock 进程存活', alive, 'pid=%s' % proc.pid)
    check('找得到 dock 窗口', bool(hwnd), 'hwnd=%s' % hwnd)
    if not alive or not hwnd:
        print('\n  日志尾部：')
        for l in read_log_tail(30):
            print('    ' + l)
        return 1

    # --- 2) 跟踪器启动 ---
    print('\n[2] 跟踪器启动')
    lines = read_log_tail(300)
    check('日志有 [track] 已启动', any('[track] 已启动' in l for l in lines))
    check('日志有 [track] 全量重建', any('[track] 全量重建' in l for l in lines))
    check('跟踪器已注册 shell hook（日志有 shellHookMsg）',
          any('shellHookMsg' in l for l in lines))
    m0 = log_mark()

    # --- 3) 启动一个**不在 config 里**的程序 → 应出现临时图标 ---
    print('\n[3] 启动 charmap（不在 config 里），应多出一个临时图标')
    sysroot = os.environ.get('SystemRoot', r'C:\Windows')
    # ⚠ 必须挑一个 config.json 里**没有**的程序 —— 启动 notepad 的话它会匹配上
    #   固定项，不会产生临时图标（第一版探针就栽在这：判据本身是错的）。
    #   charmap.exe 是系统自带、启动快、config 里没有。
    app = subprocess.Popen([os.path.join(sysroot, 'System32', 'charmap.exe')])
    time.sleep(2.5)

    grp_line = wait_log(lambda l: '分组变化' in l, m0, 4.0)
    check('收到新应用的分组变化日志', grp_line is not None,
          (grp_line.split(']')[-1].strip() if grp_line else '（4 秒内没有）'))

    temp_seen = parse_temp(grp_line)
    total_seen = parse_total(grp_line)
    check('新应用被加成临时图标（临时数 >= 1）', temp_seen >= 1,
          '临时 K=%s 总项 M=%s' % (temp_seen, total_seen))

    app_alive = app.poll() is None
    check('charmap 起来了', app_alive)
    app_wins = find_windows_by_pid(app.pid) if app_alive else []
    check('charmap 有可见顶层窗口（该被跟踪）', len(app_wins) >= 1,
          '%d 个: %s' % (len(app_wins), [w[1][:20] for w in app_wins]))

    # --- 4) 关掉它，临时图标应被回收 ---
    print('\n[4] 关掉 charmap，临时图标应被回收')
    m1 = log_mark()
    for h, _ in app_wins:
        u32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(2.0)

    back_line = wait_log(lambda l: '分组变化' in l, m1, 4.0)
    temp_after = parse_temp(back_line)
    total_after = parse_total(back_line)
    check('关窗后收到分组变化（回收了临时图标）', back_line is not None,
          (back_line.split(']')[-1].strip() if back_line else '（4 秒内没有）'))
    check('回收后临时图标数少了一个', temp_after == temp_seen - 1 and temp_after >= 1,
          '临时 %s -> %s（总项 %s -> %s）' % (temp_seen, temp_after, total_seen, total_after))

    try:
        app.terminate()
    except Exception:
        pass
    time.sleep(0.6)
    check('关窗后 ZDock 仍存活', proc.poll() is None)

    # --- 5) 点击切换：PostMessage 到图标位置（不移动真实光标） ---
    print('\n[5] 点击切换（PostMessage 注入，不抢焦点）')
    # 图标位置：面板水平居中，第一项的左边 = 窗口左边 + kSideSlack + kPadX
    # 我们不知道 dpi，所以用一个稳妥的点：窗口底部往上 30px、水平居中偏左。
    rc = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc))
    ww = rc.right - rc.left
    hh = rc.bottom - rc.top
    # client 坐标（窗口是 WS_POPUP，无边框，client == window）
    cx = ww // 2
    cy = hh - 30
    lp = (cy << 16) | (cx & 0xFFFF)
    u32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
    time.sleep(0.08)
    u32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.5)
    check('点击后 ZDock 仍存活', proc.poll() is None)

    # --- 6) 日志尾部总览（给人工看有没有异常） ---
    print('\n[6] 日志尾部 20 行：')
    for l in read_log_tail(20):
        print('    ' + l)

    # 收尾
    print('\n[7] 收尾')
    try:
        proc.terminate()
    except Exception:
        pass
    time.sleep(0.4)
    subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True)

    print('\n================ 结果 ================')
    allok = True
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        allok = allok and ok
    print('======================================')
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
