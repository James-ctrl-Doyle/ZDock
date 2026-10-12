"""验证「缩放动画中图标被 region 裁切」已修（0.1.24）。

根因：`updateHitRegion()` 用 `node->scale()`（**目标值**）算窗口 region，而
`SetWindowRgn` **会裁剪绘制** —— scale 动画是 150ms 缓动、视觉滞后于返回值：
缩小的那一瞬间 `scale()` 已回到 1.0，那块就不进 region 了，但视觉上图标还在峰值大小、
上半部分溢出面板 → 被切掉。用户 2026-10-12 报的"快速滑动鼠标时图标不完整"。

场景：悬停一个图标（放大）→ 移开（触发缩小）→ **立刻**截图（动画进行中）。
判据：
  C1) 静止时窗口 region 的 bounding box 应覆盖**峰值**高度（不是只有面板高）
  C2) 缩小动画进行中抓的图，图标轮廓完整（人工核对 verify_shrinking.png）

⚠ 会移动鼠标约 1.5 秒，结束还原原位；开跑前采样光标位置，在动就放弃。
"""

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
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindowRgn.argtypes = [wt.HWND, wt.HANDLE]
g32.GetRgnBox.argtypes = [wt.HANDLE, ctypes.POINTER(wt.RECT)]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

ROOT = r'C:\Users\zqw35\WorkBuddy\projects\ZDock'
WORK = os.path.join(ROOT, r'build\_verify_clip')
OUT = os.path.join(ROOT, r'build\_review')

# autoHide 关掉，方便把光标停在图标上做悬停
CFG = {
    "iconSize": 48, "iconGap": 12, "hoverScale": 1.7, "animMs": 150, "bottomMargin": 6,
    "bgColor": "#1A1A1ACC", "cornerRadius": 12,
    "autoHide": False, "autoHideDelayMs": 200, "slideInMs": 120, "slideOutMs": 200,
    "hotZoneThickness": 4, "hotZoneSpan": "half", "hideOnFullscreen": False,
    "reserveWorkArea": False, "dockEdge": "bottom", "dockAlign": "center", "dockOffset": 0,
    "opacity": 0.8, "showIndicator": False, "autoStart": False, "monitorIndex": -1,
    "items": [{"path": "C:\\Windows\\explorer.exe"},
              {"path": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "name": "Google Chrome"},
              {"path": "::{645FF040-5081-101B-9F08-00AA002F954E}", "name": "回收站", "pinRight": True}],
}

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(f'  [{"PASS" if ok else "FAIL"}] {name}' + (f'   {detail}' if detail else ''))


def windows_of(cls):
    out = []

    def cb(h, _lp):
        b = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, b, 256)
        if b.value == cls:
            r = wt.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            out.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u32.EnumWindows(ctypes.cast(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb),
                                ctypes.c_void_p), 0)
    return out


def cursor():
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


p0 = cursor()
time.sleep(0.18)
p1 = cursor()
time.sleep(0.18)
p2 = cursor()
if not (p0 == p1 == p2):
    print(f'⚠ 鼠标正在移动（{p0}->{p1}->{p2}），放弃。')
    sys.exit(2)
orig = cursor()

pc.kill_existing_zdock()
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
shutil.copy2(os.path.join(ROOT, r'build\bin\ZDock.exe'), os.path.join(WORK, 'ZDock.exe'))
with open(os.path.join(WORK, 'config.json'), 'w', encoding='utf-8') as f:
    json.dump(CFG, f, ensure_ascii=False)
subprocess.Popen([os.path.join(WORK, 'ZDock.exe')], cwd=WORK,
                 creationflags=0x00000008, close_fds=True)
time.sleep(2.2)

dw = windows_of('ZDock')
if not dw:
    print('!! 没找到 dock 窗口')
    pc.kill_existing_zdock()
    sys.exit(1)
hwnd, rect = dw[0]
W = rect[2] - rect[0]
H = rect[3] - rect[1]

# ---- C1：静止时 region 是否已覆盖峰值高度 ----
g32.CreateRectRgn.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
g32.CreateRectRgn.restype = wt.HANDLE
g32.DeleteObject.argtypes = [wt.HANDLE]
rgn = g32.CreateRectRgn(0, 0, 0, 0)
got = u32.GetWindowRgn(hwnd, rgn)
box = wt.RECT()
if got and g32.GetRgnBox(rgn, ctypes.byref(box)):
    reg_h = box.bottom - box.top
    # 面板高 ≈ 窗口高 × 0.68；修后留了峰值余量，region 应明显更大（会超出窗口被系统裁）
    print(f'  窗口 {W}x{H} / region 高 = {reg_h}（box={box.left},{box.top},{box.right},{box.bottom}）')
    check('C1) region 已留出峰值余量（明显高于面板高）',
          reg_h >= H * 0.7, f'region 高 {reg_h} / 窗口高 {H}（修前 ≈120）')
else:
    check('C1) region 已按**峰值**留出空间（高度接近窗口高）', False, 'GetWindowRgn 失败')
g32.DeleteObject(rgn)

# ---- C2：悬停放大 → 移开（触发缩小）→ 立刻截图 ----
try:
    # 用"最左图标"的中心做悬停（图标 x 由窗口左缘 + 面板留白近似）
    icon_cx = rect[0] + int(W * 0.22)
    icon_cy = rect[1] + int(H * 0.52)     # 面板内图标的中部（从抓图量出来约 0.52）
    u32.SetCursorPos(icon_cx, icon_cy)
    time.sleep(0.6)                       # 等放大动画走完
    save1, W1, H1 = shot.grab_printwindow(hwnd)
    if save1:
        shot.write_png(os.path.join(OUT, 'verify_grown.png'), W1, H1, save1)

    # 移开 → 触发缩小；**立刻**抓（动画只有 150ms，这里 ~30ms 就在中途）
    u32.SetCursorPos(orig[0], orig[1])
    time.sleep(0.03)
    data2, W2, H2 = shot.grab_printwindow(hwnd)
    if data2:
        shot.write_png(os.path.join(OUT, 'verify_shrinking.png'), W2, H2, data2)
        check('C2) 抓到"缩小动画进行中"的截图（人工核对图标是否完整）', True, f'{W2}x{H2}')
    else:
        check('C2) 抓到"缩小动画进行中"的截图（人工核对图标是否完整）', False, 'PrintWindow 失败')

    # 顺带再抓一张静止态（缩小完成后）
    time.sleep(0.5)
    data3, W3, H3 = shot.grab_printwindow(hwnd)
    if data3:
        shot.write_png(os.path.join(OUT, 'verify_rest.png'), W3, H3, data3)
finally:
    u32.SetCursorPos(*orig)
    time.sleep(0.1)

pc.kill_existing_zdock()

print()
print('=' * 70)
npass = sum(1 for _, ok, _ in results if ok)
print(f'RESULT: {npass}/{len(results)} PASS')
for n, ok, d in results:
    if not ok:
        print(f'  FAIL: {n}   {d}')
print('=' * 70)
