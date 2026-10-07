"""阶段六（二/三）：开机自启 + 设置窗口 + 多显示器锚定 验证。

三块分别怎么验：

**开机自启**（任务书 §2 #26）—— 直接查注册表
  · config 里 autoStart=true  → 启动后 `HKCU\...\Run` 里应有 `ZDock`，值 = exe 完整路径（带引号）
  · config 里 autoStart=false → 启动后该值应被删掉
  ⚠ **这个探针会真的读写用户的注册表 Run 项** —— 所以开头先把原值备份、
    结束时无论成败都恢复（否则就把用户的开机自启改了）。

**设置窗口**（任务书 §2 #25）—— 用消息注入打开（探针点不到菜单，那要抢焦点）
  · 投 WM_APP+101 之后应出现标题为 `ZDockSettings` 的窗口
  · 窗口是独立顶层窗口、尺寸与预期一致
  · 窗口里**画出了东西**（数强调色 #4CC2FF 的像素 —— 分组标题用它）

**多显示器锚定**（任务书 §2 #31）—— 本机只有一台显示器，所以验的是"等价与兜底"
  · monitorIndex=0 与 -1（跟随）应给出同一个位置
  · monitorIndex=5（越界）应退回主屏、且**不能崩**

跑法： <python> build-support/_probe_settings.py
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
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
adv = ctypes.WinDLL('advapi32', use_last_error=True)

u32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetDpiForWindow.argtypes = [wt.HWND]
u32.GetDpiForWindow.restype = ctypes.c_uint
u32.GetWindowDC.argtypes = [wt.HWND]
u32.GetWindowDC.restype = wt.HDC
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
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

RUN_KEY = r'Software\Microsoft\Windows\CurrentVersion\Run'
VALUE_NAME = 'ZDock'
WM_APP_OPEN_SETTINGS = 0x8000 + 101

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print('  %s  %s%s' % ('PASS' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))


# ---------------------------------------------------------------------------
# 注册表
# ---------------------------------------------------------------------------
def reg_read():
    """读 Run 项里的 ZDock 值；没有返回 None。"""
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_READ) as k:
            v, _ = winreg.QueryValueEx(k, VALUE_NAME)
            return v
    except FileNotFoundError:
        return None
    except OSError:
        return None


def reg_write(value):
    """写 / 删 Run 项里的 ZDock 值（value=None 表示删）。"""
    import winreg
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if value is None:
            try:
                winreg.DeleteValue(k, VALUE_NAME)
            except FileNotFoundError:
                pass
        else:
            winreg.SetValueEx(k, VALUE_NAME, 0, winreg.REG_SZ, value)


# ---------------------------------------------------------------------------
# 窗口
# ---------------------------------------------------------------------------
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


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def rect_of(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


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


def count_nonbg(hwnd, bg=(28, 28, 28), tol=6):
    """数"不是背景色"的像素 —— 用来证明窗口里确实画了东西。"""
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
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
        buf = ctypes.create_string_buffer(w * h * 4)
        if g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0):
            data = buf.raw
            for i in range(0, len(data) - 3, 4):
                b, g, rr = data[i], data[i + 1], data[i + 2]
                if abs(b - bg[0]) > tol or abs(g - bg[1]) > tol or abs(rr - bg[2]) > tol:
                    hits += 1
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, src)
    return hits if ok else -1


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


def start(cfg_overrides):
    """起一份隔离的 ZDock，返回 (Popen, 工作目录)。"""
    kill()
    d = os.path.join(ROOT, '_tmp_settings')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg.update({'autoHide': False, 'reserveWorkArea': False})
    cfg.update(cfg_overrides)
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.6)
    return p, d


def dock_hwnd(pid):
    ws = [w for w in find_windows(pid=pid, class_name='ZDock') if title_of(w) == '']
    return ws[0] if ws else None


def main():
    print('===== 阶段六：自启 / 设置窗口 / 多显示器锚定 =====')
    if not os.path.exists(EXE):
        print('!! 找不到 %s' % EXE)
        return 1

    # ⚠ 备份用户的 Run 项，结束时恢复 —— 绝不能把用户的开机自启改掉
    saved = reg_read()
    print('  注册表原值：%s' % ('（无）' if saved is None else saved))

    try:
        # ---------------- 1. 开机自启 ----------------
        print('\n  [1] 开机自启（写注册表）')
        p, d = start({'autoStart': True})
        if p.poll() is not None:
            check('autoStart=true 时 ZDock 启动', False)
        else:
            check('autoStart=true 时 ZDock 启动', True)
            val = reg_read()
            exe_in_dir = os.path.join(d, 'ZDock.exe')
            check('注册表里写进了 Run 项', val is not None, '值=%s' % val)
            check('值是**带引号的 exe 完整路径**',
                  val is not None and val == '"%s"' % exe_in_dir,
                  '期望 "%s"' % exe_in_dir)
            log = read_log(d)
            check('日志确认同步动作', any('autostart' in l for l in log),
                  next((l.split(']')[-1].strip() for l in log if 'autostart' in l), '（无）'))
        kill()
        time.sleep(0.5)

        # 关掉自启 → 注册表里的值应该被删掉
        p, d = start({'autoStart': False})
        if p.poll() is None:
            check('autoStart=false 时注册表值被删除', reg_read() is None,
                  '现在=%s' % reg_read())
        kill()
        time.sleep(0.5)

        # ---------------- 2. 设置窗口 ----------------
        print('\n  [2] 设置窗口（消息注入打开）')
        p, d = start({'autoStart': False})
        if p.poll() is not None:
            check('设置窗口用例：ZDock 启动', False)
        else:
            dock = dock_hwnd(p.pid)
            check('设置窗口用例：找到 dock', dock is not None)
            if dock:
                before = find_windows(pid=p.pid, title='ZDockSettings')
                check('打开之前没有设置窗口', len(before) == 0, '现有 %d 个' % len(before))
                # 注入打开
                u32.PostMessageW(dock, WM_APP_OPEN_SETTINGS, 0, 0)
                time.sleep(1.2)
                wins = find_windows(pid=p.pid, title='ZDockSettings', visible_only=True)
                check('注入后设置窗口出现了', len(wins) == 1, '可见窗口数=%d' % len(wins))
                if wins:
                    sw = wins[0]
                    sr = rect_of(sw)
                    dpi = u32.GetDpiForWindow(sw) / 96.0
                    # 与 SettingsWin.h 的 kWinW / kWinH 对齐
                    # ⚠ 改窗口尺寸时这里的期望值要一起改（已改三次：700→790→720x560→520x560）
                    exp_w = int(round(520 * dpi))
                    exp_h = int(round(560 * dpi))
                    got_w, got_h = sr[2] - sr[0], sr[3] - sr[1]
                    check('设置窗口尺寸符合预期',
                          abs(got_w - exp_w) <= 4 and abs(got_h - exp_h) <= 4,
                          '实测 %dx%d 期望 %dx%d' % (got_w, got_h, exp_w, exp_h))
                    n = count_nonbg(sw)
                    check('设置窗口里画出了内容（不是一块空白）', n > 2000,
                          '非背景像素 %d' % n)
                    lg = read_log(d)
                    check('日志确认设置窗口已打开', any('打开设置窗口' in l for l in lg),
                          next((l.split(']')[-1].strip() for l in lg if '打开设置窗口' in l), '（无）'))
            check('ZDock 未崩（设置窗口用例）', p.poll() is None)
        kill()
        time.sleep(0.5)

        # ---------------- 3. 多显示器锚定 ----------------
        print('\n  [3] 多显示器锚定（本机只有一台，验等价与兜底）')
        pos = {}
        for tag, idx in (('follow', -1), ('zero', 0), ('oob', 5)):
            p, d = start({'monitorIndex': idx})
            if p.poll() is not None:
                check('monitorIndex=%s 时 ZDock 启动' % tag, False)
                continue
            dk = dock_hwnd(p.pid)
            check('monitorIndex=%s：找到 dock' % tag, dk is not None)
            if dk:
                pos[tag] = rect_of(dk)
            kill()
            time.sleep(0.4)

        if 'follow' in pos and 'zero' in pos:
            check('monitorIndex=0 与 -1（跟随）位置一致（单屏下的必然结果）',
                  pos['follow'] == pos['zero'],
                  'follow=%s zero=%s' % (pos['follow'], pos['zero']))
        if 'oob' in pos and 'follow' in pos:
            check('monitorIndex=5（越界）退回主屏、位置不跑偏',
                  pos['oob'] == pos['follow'],
                  'oob=%s follow=%s' % (pos['oob'], pos['follow']))

    finally:
        # ⚠ 无论成败都恢复用户的注册表
        kill()
        reg_write(saved)
        print('\n  注册表已恢复：%s' % ('（删掉）' if saved is None else saved))
        shutil.rmtree(os.path.join(ROOT, '_tmp_settings'), ignore_errors=True)

    print('\n================ 结果 ================')
    for name, ok, _ in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
    print('======================================')
    n_pass = sum(1 for _, ok, _ in results if ok)
    print('  %d/%d 通过' % (n_pass, len(results)))
    return 0 if n_pass == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
