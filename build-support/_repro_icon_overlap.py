"""复现：ZDock 运行中启动新程序 → 临时图标加入后会不会叠在一起。

做法：
  1. 起一份 ZDock（config 用 资源管理器 + 回收站 两项）
  2. 截基线图
  3. **在 ZDock 还开着的时候**启动一个记事本 → WindowTracker 会发现它 → 加临时图标
  4. 再截一张 —— 对比两张，看临时图标有没有跟别的图标叠住
  5. 关掉记事本，再截一张（看临时图标撤掉后能不能复原）

⚠ 全程用 PrintWindow 拍 dock 窗口，不拍桌面。
⚠ 会短暂开一个记事本窗口，结束就关掉。

跑法： <python> build-support/_repro_icon_overlap.py
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

u32 = ctypes.WinDLL('user32')
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

ROOT = shot.ROOT
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
OUT = shot.OUT
RB = '::{645FF040-5081-101B-9F08-00AA002F954E}'


def title_of(hwnd):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return t.value


def find_dock(pid):
    return [w for w in shot.find_windows(pid=pid, class_name='ZDock') if title_of(w) == '']


def rect_of(hwnd):
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def main():
    # 可选：用指定的 exe 跑（用来对比"修复前 / 修复后"）。
    #   <python> _repro_icon_overlap.py --exe ..\_review\ZDock_0.1.10.exe --tag before
    exe_override = None
    tag = ''
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == '--exe' and i + 1 < len(argv):
            exe_override = argv[i + 1]
        if a == '--tag' and i + 1 < len(argv):
            tag = '_' + argv[i + 1]
    src_exe = exe_override or EXE

    # ⚠ 开头先把在跑的实例关掉 —— ZDock 单实例互斥体名是固定的，留着它探针会静默让位。
    #   按窗口类名找 pid，改名副本（用户验收时跑的 ZDock_0.1.x.exe）也杀得掉。
    kill_existing_zdock()
    subprocess.run(['taskkill', '/F', '/IM', 'notepad.exe'], capture_output=True)
    d = os.path.join(ROOT, '_tmp_overlap')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(src_exe, os.path.join(d, 'ZDock.exe'))
    print('用的 exe: %s' % src_exe)
    # 只放两项固定（左：资源管理器；右：回收站），中间留给运行中的应用
    cfg = {
        'iconSize': 48, 'iconGap': 12, 'hoverScale': 1.7, 'opacity': 0.8,
        'cornerRadius': 12, 'showIndicator': True,
        'dockEdge': 'bottom', 'dockAlign': 'center', 'dockOffset': 0,
        'autoHide': False, 'hideOnFullscreen': False, 'reserveWorkArea': False,
        'autoStart': False, 'monitorIndex': -1,
        'items': [
            {'path': r'C:\Windows\explorer.exe'},
            {'path': RB, 'name': '回收站', 'pinRight': True},
        ],
    }
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
    time.sleep(2.8)
    dock = find_dock(p.pid)
    if not dock:
        print('!! 没找到 dock')
        return 1
    hwnd = dock[0]

    print('--- 1) 基线（只有固定项 + 已有运行中的应用）---')
    print('   窗口矩形 =', rect_of(hwnd))
    data, W, H = shot.grab_printwindow(hwnd)
    if data:
        shot.write_png(os.path.join(OUT, 'overlap_1_before%s.png' % tag), W, H, data)
        print('   窗口 %dx%d' % (W, H))

    print('--- 2) 在 ZDock 运行中启动记事本 ---')
    np = subprocess.Popen([r'C:\Windows\System32\notepad.exe'])
    time.sleep(2.5)
    data2, W2, H2 = shot.grab_printwindow(hwnd)
    if data2:
        shot.write_png(os.path.join(OUT, 'overlap_2_after_notepad%s.png' % tag), W2, H2, data2)
        print('   窗口 %dx%d' % (W2, H2))

    print('--- 3) 关掉记事本（这一步会在旧版里留下"孤儿节点"）---')
    subprocess.run(['taskkill', '/F', '/IM', 'notepad.exe'], capture_output=True)
    time.sleep(2.0)
    r3 = rect_of(hwnd)
    data3, W3, H3 = shot.grab_printwindow(hwnd)
    if data3:
        shot.write_png(os.path.join(OUT, 'overlap_3_after_close%s.png' % tag), W3, H3, data3)
        print('   窗口 %dx%d' % (W3, H3))

    # ⚠ 关键的一步：**再开一个**。旧版里上一步留下的孤儿还挂在树上，
    #   而 items 已经少了一项 —— 面板宽度按少的算，新图标就会被挤出边界。
    print('--- 4) 再启动一个记事本（旧版会在这里露馅）---')
    subprocess.Popen([r'C:\Windows\System32\notepad.exe'])
    time.sleep(2.5)
    data4, W4, H4 = shot.grab_printwindow(hwnd)
    if data4:
        shot.write_png(os.path.join(OUT, 'overlap_4_again%s.png' % tag), W4, H4, data4)
        print('   窗口 %dx%d' % (W4, H4))

    # 把每步窗口宽度列出来，方便跟截图里的图标个数对一眼
    print('--- 四步窗口宽度 ---')
    print('   %-10s %d' % ('基线', W))
    print('   %-10s %d' % ('开程序', W2))
    print('   %-10s %d' % ('关程序', W3))
    print('   %-10s %d' % ('再开一次', W4))
    print('   参考：项数 n 对应宽度 ≈ (60n + 112) * dpi 物理像素')
    print('         4 项 ≈ 436   5 项 ≈ 510   6 项 ≈ 585')

    log = os.path.join(d, 'ZDock.log')
    if os.path.exists(log):
        lines = open(log, 'rb').read().decode('utf-8', 'replace').splitlines()
        print('--- 日志尾部（重排 / 分组变化）---')
        for l in lines[-30:]:
            if any(k in l for k in ('分组变化', '重排', '重建', '结构')):
                print('   ' + l.split(']')[-1].strip()[:120])

    subprocess.run(['taskkill', '/F', '/PID', str(p.pid)], capture_output=True)
    subprocess.run(['taskkill', '/F', '/IM', 'notepad.exe'], capture_output=True)
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
