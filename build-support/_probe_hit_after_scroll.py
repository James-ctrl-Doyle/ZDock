"""判决：设置窗口滚动之后，控件的**命中区域**有没有跟着滚动一起移动。

⚠ 判据用**点击**，不用 hover。原因：探针起的是后台窗口，
   `SetCursorPos` 驱动的鼠标移动不会被送到它，`PostMessage(WM_MOUSEMOVE)`
   也驱动不了 Button 的 hover（实测前后两帧像素完全相同）。
   而**点击**是纯消息驱动的（`Button::onDown/onUp` 只看 lParam 的客户区坐标），
   窗口是不是前台都能生效 —— 这是唯一能确定性地测"命中区域在哪"的手段。

每次点击完点「×」关掉设置窗口（`SettingsWin::close()` 会写 config.json），
然后读配置看**哪个设置项被改了** —— 那就是这次点击真正打到的控件。

滚动量不是假设的：每次都从截图里量分隔线的位移反推出来。

跑法： <python> build-support/_probe_hit_after_scroll.py
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
from _probe_common import kill_existing_zdock   # noqa: E402
import _shot_stage4 as shot   # noqa: E402

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
u32.PostMessageW.restype = wt.BOOL
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
OUT = shot.OUT
WM_APP_OPEN_SETTINGS = 0x8000 + 101
WM_MOUSEWHEEL = 0x020A
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202

# 基准配置：所有会被"误点"影响到的字段都取非默认值，这样"没被点到"和
# "被点到但值恰好相同"能区分开。
BASE_CFG = {
    'dockEdge': 'bottom', 'dockAlign': 'start', 'dockOffset': 0,
    'iconGap': 12, 'iconSize': 48,
    'autoHide': False, 'hideOnFullscreen': False,
    'reserveWorkArea': False, 'autoStart': False,
}

T0 = time.time()


def log(msg):
    print('  [%6.1fs] %s' % (time.time() - T0, msg))
    sys.stdout.flush()


def lp(x, y):
    """lParam 打包：低 16 位 x、高 16 位 y。（坐标可为 0..65535）"""
    return ctypes.c_ssize_t(((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)).value


def title_of(h):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return t.value


def divider_rows(data, W, H, dpi):
    """从截图里找横向分隔线，返回**客户区逻辑 y** 列表（kLine = 0x303030）。"""
    hits = []
    for y in range(H):
        n = 0
        for x in range(int(60 * dpi), min(int(600 * dpi), W), 3):
            i = (y * W + x) * 4          # BGRA
            b, g, r = data[i], data[i + 1], data[i + 2]
            if r == g == b and 44 <= r <= 52:
                n += 1
        if n > 120:
            hits.append(y)
    merged = []
    for v in hits:
        if not merged or v - merged[-1][-1] > 2:
            merged.append([v])
        else:
            merged[-1].append(v)
    return [round(sum(g) / len(g) / dpi, 1) for g in merged]


def best_shift(a, b):
    """b 相对 a 的整体位移（物理像素）—— 取配对最多的那个位移。"""
    if not a or not b:
        return None, 0
    best, cnt = 0, -1
    for s in range(0, 401):
        c = sum(1 for x in b if any(abs((x + s) - y) <= 2 for y in a))
        if c > cnt:
            best, cnt = s, c
    return best, cnt


def run_case(notches, click_xy, label, shot_name=None):
    """一个用例：起 dock → 开设置 → 滚 notches 格 → 点 click_xy（客户区逻辑）→ 关窗 → 读配置。"""
    print()
    print('=' * 72)
    print('用例：%s' % label)
    print('  滚动 %d 格（每格 60 逻辑）, 点击客户区逻辑 (%s)' % (notches, click_xy))
    kill_existing_zdock(verbose=False)
    d = os.path.join(ROOT, '_tmp_hit')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    cfg = dict(BASE_CFG)
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    env = dict(os.environ)
    env['ZDOCK_VERBOSE_SETTINGS'] = '1'   # 让程序自己报 scrollY（比拿分隔线反推准）
    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d, env=env)
    time.sleep(2.6)

    docks = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock') if title_of(w) == '']
    if not docks:
        print('  !! 没找到 dock 窗口')
        return None
    u32.PostMessageW(docks[0], WM_APP_OPEN_SETTINGS, 0, 0)
    time.sleep(1.3)

    sw = [w for w in shot.find_windows(pid=p.pid, class_name='ZDock')
          if title_of(w) == 'ZDockSettings']
    if not sw:
        print('  !! 没找到设置窗口')
        subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
        return None
    sw = sw[0]
    r = wt.RECT()
    u32.GetWindowRect(sw, ctypes.byref(r))
    dpi = u32.GetDpiForWindow(sw) / 96.0
    log('设置窗口 (%d,%d)-(%d,%d)  dpi=%.3f' % (r.left, r.top, r.right, r.bottom, dpi))

    def grab(tag):
        data, W, H = shot.grab_printwindow(sw)
        if data and shot_name:
            shot.write_png(os.path.join(OUT, '%s_%s.png' % (shot_name, tag)), W, H, data)
        return (data, W, H) if data else (None, 0, 0)

    d0, W, H = grab('0_before')
    base = divider_rows(d0, W, H, dpi) if d0 else []
    log('滚动前分隔线（客户区逻辑 y）: %s' % base)

    # 滚轮：lParam 是**屏幕**坐标
    if notches:
        sx = r.left + int(round(300 * dpi))
        sy = r.top + int(round(300 * dpi))
        # ⚠ delta **必须为负**（滚轮向下）。`ScrollerBox::onWheel` 是
        #   `setScroll(scrollY - space)`，space = delta/120*60*dpi ——
        #   传 +120 等于"向上滚"，已经在顶部就一动不动（这个坑我踩过：第一版探针
        #   发了 4 格正数滚轮，还以为"滚轮不生效"，其实是在往顶上顶）。
        for _ in range(notches):
            u32.PostMessageW(sw, WM_MOUSEWHEEL,
                             ctypes.c_size_t(((-120) << 16) & 0xFFFFFFFF),
                             lp(sx, sy))
            time.sleep(0.08)
        time.sleep(0.7)
    d1, W, H = grab('1_scrolled')
    aft = divider_rows(d1, W, H, dpi) if d1 else []
    log('滚动后分隔线: %s' % aft)
    shift, cnt = best_shift(base, aft)
    if shift is not None:
        log('=> 由分隔线位移反推的滚动量 ≈ %d 物理 px = %.1f 逻辑 px' % (shift, shift / dpi))

    # 点击（客户区逻辑坐标）
    if click_xy:
        # ⚠ 鼠标消息的 lParam 是**物理客户区坐标**（不是逻辑、也不是屏幕坐标）。
        #   我第一版把逻辑坐标直接塞进去，所有点击都落在别处 -> "点击落空"。
        cxp = int(round(click_xy[0] * dpi))
        cyp = int(round(click_xy[1] * dpi))
        u32.PostMessageW(sw, WM_LBUTTONDOWN, 0, lp(cxp, cyp))
        time.sleep(0.12)
        u32.PostMessageW(sw, WM_LBUTTONUP, 0, lp(cxp, cyp))
        time.sleep(0.7)
        grab('2_clicked')
        log('已在客户区逻辑 (%d,%d) 点了一下（物理 %d,%d）' % (click_xy[0], click_xy[1], cxp, cyp))

    # 点「×」关窗 → close() 里 saveConfig()
    xc = int(round(488 * dpi))     # 物理客户区坐标
    yc = int(round(27 * dpi))
    u32.PostMessageW(sw, WM_LBUTTONDOWN, 0, lp(xc, yc))
    time.sleep(0.12)
    u32.PostMessageW(sw, WM_LBUTTONUP, 0, lp(xc, yc))
    time.sleep(1.2)

    # 读程序自报的 scrollY（诊断开关打开时才有）
    logpath = os.path.join(d, 'ZDock.log')   # ⚠ 别叫 lp —— 会把上面的 lp() 函数遮住
    if os.path.exists(logpath):
        hl = [l.split(']', 1)[-1].strip() for l in
              open(logpath, 'rb').read().decode('utf-8', 'replace').splitlines() if '[hit]' in l]
        for l in hl[::-1]:
            if 'after-wheel' in l:
                log('程序自报：%s' % l)
                break

    saved = None
    cp = os.path.join(d, 'config.json')
    if os.path.exists(cp):
        try:
            saved = json.load(open(cp, encoding='utf-8'))
        except Exception as e:
            log('读 config.json 失败: %s' % e)
    if saved is None:
        log('!! 没读到 config.json')
    else:
        diff = {k: (BASE_CFG.get(k), saved.get(k)) for k in BASE_CFG
                if saved.get(k) != BASE_CFG.get(k)}
        if diff:
            log('=> 被改动的配置项：')
            for k, (a, b) in diff.items():
                log('     %-18s %s -> %s' % (k, a, b))
        else:
            log('=> 没有任何配置项被改动（这次点击落空了）')

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    time.sleep(0.5)
    shutil.rmtree(d, ignore_errors=True)
    return saved


# 行位置（客户区逻辑 y，未滚动时）—— 与 SettingsWin 的布局常量对齐：
#   kTitleH=46, kRowH=39, kSectionH=52, 起始 y=8
#   停靠边 行 = 346..385（按钮 351..379），对齐 385..424，沿边偏移 424..463，
#   显示器 463..502，自动隐藏 554..593，全屏时让位 593..632
UNSCROLLED = {
    '停靠边/下': (229, 411),      # 按钮 x: 198..260
    '停靠边/上': (299, 411),      # 268..330
    '停靠边/左': (369, 411),      # 338..400
    '对齐/末端': (369, 449),      # 对齐 行的第 3 个按钮
}

# 滚动 4 格 ≈ 240 逻辑（每格 60）。滚动后：
#   「停靠边/上」显示在客户区逻辑 y ≈ 397-240 = 157..185（中心 171）
#   「全屏时让位」显示在 ≈ 640-240 = 400..437 —— 正好是用户鼠标在图里的位置
CASES = [
    # (滚几格, 点哪, 说明, 存图前缀)
    (0, UNSCROLLED['停靠边/上'], '对照：不滚动，点「停靠边/上」(411) -> 期望 dockEdge=top', 'hits_a'),
    (0, UNSCROLLED['对齐/末端'], '对照：不滚动，点「对齐/末端」(449) -> 期望 dockAlign=end', 'hits_b'),
    (4, (299, 171), '滚动 4 格后，点「停靠边/上」**滚动后的显示位置**(171) -> 期望 dockEdge=top', 'hits_c'),
    (4, (298, 405), '滚动 4 格后，点**用户的鼠标位置**(405) -> 期望 hideOnFullscreen=true（那是「全屏时让位」）', 'hits_d'),
]


def main():
    print('===== 命中区域判决探针（滚动后点击） =====')
    for notches, xy, label, name in CASES:
        run_case(notches, xy, label, shot_name=name)
    print()
    print('说明：把每条的「被改动的配置项」放到一起看 ——')
    print('  · 对照组改对了   -> 探针的点击机制没问题')
    print('  · 滚动后点「显示位置(170)」如果改的是 iconGap（而不是 dockEdge）')
    print('      -> 命中区域**没跟着滚动**（控件按未滚动坐标命中）')
    print('  · 滚动后点 (405) 如果改的是 dockEdge=top（而不是 hideOnFullscreen）')
    print('      -> 同样是命中区域没跟着滚动')
    return 0


if __name__ == '__main__':
    sys.exit(main())
