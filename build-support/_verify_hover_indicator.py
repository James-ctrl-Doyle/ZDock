"""验证 0.1.20 的两个修复（可对着旧版跑，用于复现）。

  ① 鱼眼放大按**实际位置**算 —— 旧版用 items 数组下标差，导致悬停左边的图标时
     最右边的"回收站"跟着放大（`pinRight` 项在数组里紧挨着）。
  ② 指示器默认熄灭 —— 旧版 `IndicatorNode::setOn(false)` 会 early-return，
     `visual.Opacity` 默认 1 从没被关掉 → 所有指示器一直亮着。

用法：
    <python> build-support/_verify_hover_indicator.py [--exe <path>]
不带 --exe 时用 build/bin/ZDock.exe（当前构建）。

⚠ 会移动鼠标（悬停到最左边的图标上），结束**还原原位**；开跑前采样光标位置，
  只要在动就放弃。

判据：
  A) showIndicator=false 时，面板底部的蓝色圆点簇数应为 **0**
  B) 悬停"最左边那个图标"时，**最右侧 15% 区域**的像素变化应≈0（回收站不该跟着放大），
     同时最左侧区域**必须明显变化**（悬停的那个确实放大了 —— 否则说明探针没生效）
"""

import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc          # noqa: E402
import _shot_stage4 as shot         # noqa: E402

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
try:
    u32.SetProcessDPIAware()
except Exception:
    pass
u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
WORK = os.path.join(ROOT, r'build\_verify_hover_ind')
OUT = os.path.join(ROOT, r'build\_review')

# 用用户那份 items（含右侧固定的回收站、以及没在运行的 TIM / Steam）
ITEMS = [
    {"path": "C:\\Windows\\explorer.exe"},
    {"path": "::{645FF040-5081-101B-9F08-00AA002F954E}", "name": "回收站", "pinRight": True},
    {"path": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "name": "Google Chrome"},
    {"path": "C:\\Program1\\TIM_3.4.8_Green\\Bin\\TIM.exe", "name": "TIM"},
    {"path": "C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe", "name": "Steam"},
]

BASE = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": False, "autoHideDelayMs": 500, "slideInMs": 200, "slideOutMs": 300,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": False,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": ITEMS,
}

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f'   {detail}' if detail else ''))


def cursor():
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def start(exe, **over):
    pc.kill_existing_zdock()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    shutil.copy2(exe, os.path.join(WORK, 'ZDock.exe'))
    cfg = dict(BASE)
    cfg.update(over)
    with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
        json.dump(cfg, f, ensure_ascii=False)
    subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                     creationflags=0x00000008, close_fds=True)
    time.sleep(2.2)


def dock_window():
    ws = shot.find_windows(class_name='ZDock')
    return ws[0] if ws else None


def grab():
    hwnd = dock_window()
    if not hwnd:
        return None, 0, 0
    data, W, H = shot.grab_printwindow(hwnd[0] if isinstance(hwnd, tuple) else hwnd)
    return data, W, H


def save(name, data, W, H):
    if data:
        shot.write_png(os.path.join(OUT, name), W, H, data)


def diff_count(a, b, W, H, x0, x1, y0, y1, thresh=40):
    """两图在指定区域里"显著不同"的采样点数（step 2，省时间）。"""
    n = 0
    for y in range(y0, y1, 2):
        b0 = y * W * 4
        for x in range(x0, x1, 2):
            o = b0 + x * 4
            d = abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2])
            if d > thresh:
                n += 1
    return n


def blue_dot_clusters(data, W, H, y0, y1):
    """面板底部蓝色圆点（指示器 #4CC2FF）的簇数。"""
    cols = set()
    for y in range(y0, min(y1, H)):
        base = y * W * 4
        for x in range(W):
            o = base + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            if b > 200 and g > 150 and r < 130 and (b - r) > 90:
                cols.add(x)
    segs, prev = 0, None
    for x in sorted(cols):
        if prev is None or x - prev > 12:
            segs += 1
        prev = x
    return segs


ap = argparse.ArgumentParser()
ap.add_argument('--exe', default=os.path.join(ROOT, r'build\bin\ZDock.exe'))
args = ap.parse_args()
print(f'被测 exe：{args.exe}')
print()

p0 = cursor()
time.sleep(0.18)
p1 = cursor()
time.sleep(0.18)
p2 = cursor()
if not (p0 == p1 == p2):
    print(f'⚠ 鼠标正在移动（{p0}->{p1}->{p2}），放弃（不做任何移动）。')
    sys.exit(2)

orig = cursor()

# =================== 场景 1：showIndicator=false ===================
print('=' * 70)
print('场景 1：showIndicator=false —— 面板底部不该有任何指示器')
print('=' * 70)
start(args.exe, showIndicator=False)

data, W, H = grab()
if not data:
    print('!! 拍不到 dock 窗口'); pc.kill_existing_zdock(); sys.exit(1)
print(f'  dock 截图 {W}x{H}')
save('verify_idle_old.png' if 'ZDock_0.1.19' in args.exe else 'verify_idle_new.png', data, W, H)

# 图标区大概在图片上半部，指示器在最底部
dots = blue_dot_clusters(data, W, H, int(H * 0.82), H)
check('A) showIndicator=false → 指示器簇数 = 0', dots == 0, f'实测 {dots} 簇')

# =================== 场景 2：悬停最左侧图标 ===================
print()
print('=' * 70)
print('场景 2：把鼠标悬停到**最左边**的图标上 —— 最右边的回收站不该跟着放大')
print('=' * 70)

# 用 idle 图找最左侧图标的中心列：从图片下半部（面板）扫"有内容的列"
hwnd = dock_window()
hwnd = hwnd[0] if isinstance(hwnd, tuple) else hwnd
rc = wt.RECT()
u32.GetWindowRect(hwnd, ctypes.byref(rc))
wleft, wtop = rc.left, rc.top

# 图标区：面板的上半部分（halo 之下）
icon_top_px = int(H * 0.25)
icon_bot_px = int(H * 0.80)
cols = []
for x in range(W):
    hit = False
    for y in range(icon_top_px, icon_bot_px, 2):
        o = (y * W + x) * 4
        # 图标是彩色的/亮的；面板背景很暗
        if data[o] + data[o + 1] + data[o + 2] > 200:
            hit = True
            break
    cols.append(hit)
segs, st = [], None
for x, h in enumerate(cols):
    if h and st is None:
        st = x
    elif not h and st is not None:
        if x - st >= 12:
            segs.append((st, x - 1))
        st = None
if st is not None and W - st >= 12:
    segs.append((st, W - 1))
print(f'  扫描到 {len(segs)} 个图标列段：{segs}')
if not segs:
    print('  !! 没扫到图标，判据无法判定'); pc.kill_existing_zdock(); sys.exit(1)

first = segs[0]
cx = (first[0] + first[1]) // 2
cy = (icon_top_px + icon_bot_px) // 2
# ⚠ WM_LBUTTONDOWN/UP 不用，只做悬停；SetCursorPos 用的是屏幕坐标
u32.SetCursorPos(rc.left + cx, rc.top + cy)
time.sleep(0.9)          # 等 150ms 动画走完 + 事件送达

data2, W2, H2 = grab()
save('verify_hover_old.png' if 'ZDock_0.1.19' in args.exe else 'verify_hover_new.png', data2, W2, H2)

x_right0 = int(W * 0.85)
x_left1 = int(W * 0.25)
d_right = diff_count(data, data2, W, H, x_right0, W, int(H * 0.2), H)
d_left = diff_count(data, data2, W, H, 0, x_left1, int(H * 0.2), H)
print(f'  左侧 25% 区域差异点数 = {d_left}')
print(f'  右侧 15% 区域差异点数 = {d_right}（回收站所在）')

check('B1) 悬停的（最左）图标确实放大了 —— 左侧有显著变化', d_left > 40, f'{d_left} 点')
check('B2) 最右的回收站**没有**变化', d_right <= 20, f'{d_right} 点')

u32.SetCursorPos(*orig)
time.sleep(0.3)

# =================== 场景 3：showIndicator=true，只有运行中的才亮 ===================
print()
print('=' * 70)
print('场景 3：showIndicator=true —— 运行中的项才该有点（TIM/Steam 没运行）')
print('=' * 70)
start(args.exe, showIndicator=True)
data3, W3, H3 = grab()
if data3:
    save('verify_ind_on.png', data3, W3, H3)
    dots3 = blue_dot_clusters(data3, W3, H3, int(H3 * 0.82), H3)
    # 运行中的是 explorer / WorkBuddy / Chrome（3 个左右）；TIM / Steam / 回收站不该亮
    check('C) showIndicator=true → 指示器簇数在 1~4 之间（不是全部 6 个都亮）',
          1 <= dots3 <= 4, f'实测 {dots3} 簇（若 =6 说明连没运行的也亮了）')
else:
    check('C) 拍不到截图', False)

pc.kill_existing_zdock()
u32.SetCursorPos(*orig)

print()
print('=' * 70)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print(f'截图目录：{OUT}')
print('=' * 70)
