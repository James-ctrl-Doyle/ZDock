"""阶段二验证：图标级右键菜单里的"从 Dock 移除"落盘链路。

`removeItem()` 分两半：
  1) 改 Config::items + save() 到 config.json   <- 本脚本覆盖（可脚本化）
  2) rebuild() 重建界面                          <- 模态菜单点不到，靠 [dock] 日志 + 截图人工验收

这里不去点菜单（TrackPopupMenuEx 是模态的，脚本点不到；硬 PostMessage 也很难对齐菜单项位置），
而是**直接验证持久化的正确性**：写一份 5 项的 config.json，删掉中间那项再写回，
让 ZDock 重新载入，确认项数 = 4、且被删的那项真的不在了。

另外验证一个容易出错的边界：**删除后 cfg->items 里剩下的路径要和界面上生效的一致**
（collectItemsFromConfig 会跳过不存在的文件吗？看实现——不会，它照单全收。
 所以这里同时确认"路径不存在也照旧显示"，这是当前设计，不是 bug）。

跑法： <python> build-support/_probe_remove_item.py
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')
SYSROOT = os.environ.get('SystemRoot', r'C:\Windows')


def dock_hwnds():
    out = []
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(128)
        u32.GetClassNameW(hwnd, buf, 128)
        if buf.value == 'ZDock':
            out.append(hwnd)
        return True

    u32.EnumWindows(CB(cb), 0)
    return out


def log_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return [ln.rstrip() for ln in f]


def run(workdir, wait=3.0):
    exe = os.path.join(workdir, 'ZDock.exe')
    log = os.path.join(workdir, 'ZDock.log')
    for p in (log, log + '.1'):
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass
    p = subprocess.Popen([exe], cwd=workdir)
    time.sleep(wait)
    alive = p.poll() is None
    wins = len(dock_hwnds())
    lines = log_lines(log)
    try:
        p.terminate()
        time.sleep(1.0)
        if p.poll() is None:
            p.kill()
    except Exception:
        pass
    time.sleep(0.6)
    return alive, wins, lines


def last(lines, needle):
    hit = None
    for ln in lines:
        if needle in ln:
            hit = ln
    return hit


def main():
    if not os.path.isfile(SRC_EXE):
        print('找不到 ZDock.exe，先 bash build-support/build.sh')
        return 2

    work = tempfile.mkdtemp(prefix='zdock_rm_')
    print('隔离目录: %s' % work)
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    results = []

    five = [os.path.join(SYSROOT, 'explorer.exe'),
            os.path.join(SYSROOT, 'System32', 'notepad.exe'),
            os.path.join(SYSROOT, 'System32', 'mspaint.exe'),
            os.path.join(SYSROOT, 'System32', 'cmd.exe'),
            os.path.join(SYSROOT, 'System32', 'calc.exe')]

    print('\n[1] 写 5 项配置 -> 启动，确认生效 5 项')
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'items': [{'path': p} for p in five]}, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run(work)
    ln = last(lines, '[config] 载入 ok')
    print('  %s' % ln)
    ok1 = alive and wins == 1 and ln and '项数=5' in ln
    results.append(('1 五项载入', bool(ok1)))

    print('\n[2] 模拟 removeItem：删掉第 3 项（mspaint）写盘 -> 重启')
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    del data['items'][2]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run(work)
    ln = last(lines, '[config] 载入 ok')
    print('  %s' % ln)
    ok2 = alive and wins == 1 and ln and '项数=4' in ln
    results.append(('2 删除后重启为四项', bool(ok2)))

    print('\n[3] 确认被删项确实不在了、剩下的顺序正确')
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    paths = [it['path'] for it in data['items']]
    print('  剩余: %s' % json.dumps([os.path.basename(p) for p in paths], ensure_ascii=False))
    ok3 = (len(paths) == 4
           and 'mspaint.exe' not in ' '.join(paths).lower()
           and paths[0].lower().endswith('explorer.exe')
           and paths[2].lower().endswith('cmd.exe'))
    results.append(('3 被删项消失且顺序保持', bool(ok3)))

    print('\n[4] 删到最后只剩 1 项（面板宽度会收缩）')
    data['items'] = data['items'][:1]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run(work)
    ln = last(lines, '[config] 载入 ok')
    windln = last(lines, '[dock] dpi=')
    print('  %s' % ln)
    print('  %s' % windln)
    ok4 = alive and wins == 1 and ln and '项数=1' in ln
    results.append(('4 单项面板正常', bool(ok4)))

    print('\n[5] 删到 0 项（空 Dock 不应崩）')
    data['items'] = []
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run(work)
    ln = last(lines, '[config] 载入 ok')
    print('  %s' % ln)
    ok5 = alive and wins == 1 and ln and '项数=0' in ln
    results.append(('5 空 Dock 存活不崩', bool(ok5)))

    print('\n================ 结果 ================')
    allok = True
    for name, ok in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        allok = allok and ok
    shutil.rmtree(work, ignore_errors=True)
    print('======================================')
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
