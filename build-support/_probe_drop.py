"""阶段五：拖文件到图标（任务书 §2 #20）验证。

四条分支都走一遍：
  A) 拖文件到**文件夹图标**  → 复制进该文件夹（可完整断言：文件真的出现了）
  B) 拖文件到**程序图标**    → 走"用该程序打开"分支（用一个不存在的路径，
                               所以只会记"打开失败" —— 这样零副作用地证明分支被走到）
  C) 拖到**非图标区**（halo）→ 应被忽略
  D) 拖**目录**进来          → 应被跳过并记日志

⚠ 怎么做到全自动：`WM_DROPFILES` 的 `HDROP` 本质上就是一个 `DROPFILES` 结构 +
  双 \0 结尾的文件名列表，放在一块全局内存里。所以可以自己 `GlobalAlloc` 造一个，
   再 `PostMessage(hwnd, WM_DROPFILES, hDrop, 0)` —— **不用真拖鼠标**（真拖要抢焦点、
   而且没法自动化）。

⚠ 不抢焦点、不模拟物理输入。全程在隔离临时目录里跑。

⚠ 图标坐标是**按布局常量算出来的**（不是扫一大串位置）：
   面板在窗口内的偏移 = (kSideSlack, kHaloH) = (48, 72) 逻辑像素
   图标 i 的中心 = (kSideSlack + kPadX + i*(iconBase+iconGap) + iconBase/2,
                  kHaloH + kPadY + iconBase/2)          [逻辑像素 → ×dpi = 物理]

跑法： <python> build-support/_probe_drop.py
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
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
k32.GlobalAlloc.restype = wt.HGLOBAL
k32.GlobalLock.argtypes = [wt.HGLOBAL]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalUnlock.argtypes = [wt.HGLOBAL]

WM_DROPFILES = 0x0233
GMEM_MOVEABLE = 0x0002
GMEM_ZEROINIT = 0x0040

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')

# 与 Src/DockWin.h 的布局常量保持一致（逻辑像素）
K_PAD_X, K_PAD_Y, K_HALO_H, K_SIDE_SLACK = 14.0, 8.0, 72.0, 48.0

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


class DROPFILES(ctypes.Structure):
    _fields_ = [('pFiles', wt.DWORD), ('pt', wt.POINT), ('fNC', wt.BOOL), ('fWide', wt.BOOL)]


def make_hdrop(paths, client_pt):
    """按 DROPFILES 结构造一个 HDROP（GMEM_MOVEABLE 全局内存）。"""
    blob = ''
    for p in paths:
        blob += p + '\0'
    blob += '\0'
    wide = blob.encode('utf-16-le')
    sz = ctypes.sizeof(DROPFILES)
    h = k32.GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sz + len(wide))
    if not h:
        return None, 'GlobalAlloc 失败'
    p = k32.GlobalLock(h)
    if not p:
        return None, 'GlobalLock 失败'
    df = DROPFILES()
    df.pFiles = sz
    df.pt = wt.POINT(int(client_pt[0]), int(client_pt[1]))
    df.fNC = False
    df.fWide = True
    ctypes.memmove(p, ctypes.byref(df), sz)
    ctypes.memmove(p + sz, wide, len(wide))
    k32.GlobalUnlock(h)
    return h, None


def find_windows(pid=None, class_name=None):
    out = []

    def cb(hwnd, lp):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        if class_name is not None:
            c = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(hwnd, c, 256)
            if c.value != class_name:
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


def read_log(d, n=None):
    p = os.path.join(d, 'ZDock.log')
    if not os.path.exists(p):
        return []
    with open(p, 'rb') as f:
        lines = f.read().decode('utf-8', errors='replace').splitlines()
    return lines[-n:] if n else lines


def kill():
    # ⚠ 不能只按映像名杀 —— 验收时跑的是改名副本（ZDock_0.1.x.exe），对不上。
    #   按窗口类名找 pid 才杀得掉；见 _probe_common.py 的说明。
    kill_existing_zdock(verbose=False)


def main():
    print('===== 阶段五：拖文件到图标 =====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    kill()
    d = os.path.join(ROOT, '_tmp_drop')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))

    # 目标文件夹（图标 0）+ 一个不存在的 exe（图标 1）
    target_dir = os.path.join(d, 'target_dir')
    os.makedirs(target_dir, exist_ok=True)
    fake_exe = os.path.join(d, 'no_such_program.exe')

    # 待拖入的文件（放另一个目录，避免和 target_dir 混在一起）
    src_dir = os.path.join(d, 'incoming')
    os.makedirs(src_dir, exist_ok=True)
    f1 = os.path.join(src_dir, 'alpha.txt')
    f2 = os.path.join(src_dir, 'beta.txt')
    for f in (f1, f2):
        with open(f, 'w', encoding='utf-8') as fh:
            fh.write('drop test\n')
    sub_dir = os.path.join(src_dir, 'a_folder')
    os.makedirs(sub_dir, exist_ok=True)

    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg['autoHide'] = False
    cfg['reserveWorkArea'] = False
    cfg['iconSize'] = 48
    cfg['iconGap'] = 12
    cfg['items'] = [{'path': target_dir, 'name': '目标文件夹'},
                    {'path': fake_exe, 'name': '假程序'}]
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.5)
    if p.poll() is not None:
        check('ZDock 启动', False)
        print('\n'.join(read_log(d, 20)))
        return 1
    check('ZDock 启动', True, 'pid=%s' % p.pid)

    docks = find_windows(pid=p.pid, class_name='ZDock')
    if not docks:
        check('找到 dock 窗口', False)
        kill(); shutil.rmtree(d, ignore_errors=True); return 1
    dock = docks[0]
    check('找到 dock 窗口', True, 'hwnd=%d' % dock)

    lines = read_log(d)
    check('日志确认拖放受体已挂上', any('已注册拖放受体' in l for l in lines),
          next((l.split(']')[-1].strip() for l in lines if '已注册拖放受体' in l), '（无）'))

    # ---- 按布局常量算图标中心（客户区坐标）----
    dpi = u32.GetDpiForWindow(dock) / 96.0
    icon = float(cfg['iconSize'])
    gap = float(cfg['iconGap'])
    win = rect_of(dock)

    def icon_center(i):
        lx = K_SIDE_SLACK + K_PAD_X + i * (icon + gap) + icon / 2.0
        ly = K_HALO_H + K_PAD_Y + icon / 2.0
        return (lx * dpi, ly * dpi)

    c0 = icon_center(0)
    c1 = icon_center(1)
    print('  dpi=%.2f 窗口=%s 图标0中心=(%.0f,%.0f) 图标1中心=(%.0f,%.0f)'
          % (dpi, win, c0[0], c0[1], c1[0], c1[1]))
    check('算出两个图标中心（在窗口内）',
          0 < c0[0] < win[2] - win[0] and 0 < c0[1] < win[3] - win[1]
          and 0 < c1[0] < win[2] - win[0] and c1[0] > c0[0],
          '窗口 %dx%d' % (win[2] - win[0], win[3] - win[1]))

    # ---- A) 拖两个文件到文件夹图标 → 复制 ----
    print('\n  [A] 拖 2 个文件到「文件夹图标」→ 应复制进去')
    before = len(read_log(d))
    h, err = make_hdrop([f1, f2], c0)
    if not h:
        check('A 构造 HDROP', False, err)
    else:
        u32.PostMessageW(dock, WM_DROPFILES, h, 0)
        time.sleep(1.5)
        new = read_log(d)[before:]
        alive = p.poll() is None
        check('A ZDock 没崩（跨进程 HDROP 是否可用）', alive, 'exit=%s' % p.poll())
        if alive:
            copied = sorted(os.path.basename(x) for x in os.listdir(target_dir))
            check('A 两个文件都被复制进目标文件夹', copied == ['alpha.txt', 'beta.txt'],
                  '目标文件夹内容=%s' % copied)
            check('A 日志确认复制成功', any('已复制' in l for l in new),
                  next((l.split(']')[-1].strip() for l in new if '已复制' in l), '（无）'))

    # ---- B) 拖文件到程序图标（路径不存在）→ 走"打开"分支 ----
    print('\n  [B] 拖 1 个文件到「程序图标」（不存在的 exe）→ 应走"用该程序打开"分支')
    before = len(read_log(d))
    h, err = make_hdrop([f1], c1)
    if h:
        u32.PostMessageW(dock, WM_DROPFILES, h, 0)
        time.sleep(1.5)
        new = read_log(d)[before:]
        check('B ZDock 没崩', p.poll() is None)
        check('B 走到了"用该程序打开"分支（日志有 打开失败/已用…打开）',
              any(('打开失败' in l) or ('已用' in l and '打开' in l) for l in new),
              next((l.split(']')[-1].strip() for l in new
                    if '打开失败' in l or '已用' in l), '（无）'))

    # ---- C) 拖到非图标区（halo 左上角）→ 忽略 ----
    print('\n  [C] 拖到窗口左上角 halo 区（不是图标）→ 应被忽略')
    before = len(read_log(d))
    h, err = make_hdrop([f1], (5, 5))
    if h:
        u32.PostMessageW(dock, WM_DROPFILES, h, 0)
        time.sleep(1.2)
        new = read_log(d)[before:]
        check('C 日志确认落点不在图标上', any('落点不在图标上' in l for l in new),
              next((l.split(']')[-1].strip() for l in new if '落点不在图标上' in l), '（无）'))
        copied = os.listdir(target_dir)
        check('C 没有误复制', 'alpha.txt' in copied and len(copied) == 2,
              '目标文件夹=%s' % sorted(copied))

    # ---- D) 拖目录进来 → 跳过 ----
    print('\n  [D] 拖一个目录到文件夹图标 → 应跳过并记日志')
    before = len(read_log(d))
    h, err = make_hdrop([sub_dir], c0)
    if h:
        u32.PostMessageW(dock, WM_DROPFILES, h, 0)
        time.sleep(1.2)
        new = read_log(d)[before:]
        check('D 日志确认跳过目录', any('跳过目录' in l for l in new),
              next((l.split(']')[-1].strip() for l in new if '跳过目录' in l), '（无）'))
        check('D 目标文件夹里没有出现那个子目录', 'a_folder' not in os.listdir(target_dir),
              '目标文件夹=%s' % sorted(os.listdir(target_dir)))

    check('全过程 ZDock 未崩', p.poll() is None)

    hw = u32.FindWindowW('ZDock', None)
    if hw:
        u32.PostMessageW(hw, 0x0010, 0, 0)
        time.sleep(1.2)
    kill()
    time.sleep(0.6)
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
