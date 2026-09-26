"""阶段二验证：config.json 的生成 / 读取 / 兜底 / 落盘。

全程在**隔离目录**里跑：把 ZDock.exe 复制到临时目录再启动，
这样绝不会碰到用户手上的 build/bin/config.json，也不动任何用户文件。

覆盖：
  A) 首次启动 -> 自动写出带默认值的 config.json
  B) 改 iconSize/hoverScale/items -> 重启后生效（日志里的载入行能看出来）
  C) 字段填坏值（iconSize=9999）-> 回默认，且**不覆盖**用户文件
  D) items 为空数组 -> Dock 就 0 项（用户故意清空，不该被塞回默认）
  E) 移除项落盘：直接改 config.json 模拟"移除后写盘"的形态并验证能被读回

跑法： <python> build-support/_probe_config.py
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # projects/ZDock
SRC_EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')


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


def run_once(workdir, wait=3.0):
    """起一个 ZDock（隔离目录），返回 (存活, 窗口数, 日志行)。跑完杀掉。

    ⚠ 每轮开始前清日志：ZPin 那边踩过这个坑 —— 不清的话 find_line 会命中
      上一轮留下的旧行，测试"假失败"（这里第一版就中招了）。
    """
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


def find_line(lines, needle):
    """取**最后一条**匹配（一轮日志里同一句可能打多次）。"""
    hit = None
    for ln in lines:
        if needle in ln:
            hit = ln
    return hit


def main():
    if not os.path.isfile(SRC_EXE):
        print('找不到 ZDock.exe，先 bash build-support/build.sh')
        return 2

    work = tempfile.mkdtemp(prefix='zdock_cfg_')
    print('隔离目录: %s' % work)
    shutil.copy2(SRC_EXE, os.path.join(work, 'ZDock.exe'))
    cfg = os.path.join(work, 'config.json')
    results = []

    # ---------------- A) 首次启动写出默认配置 ----------------
    print('\n[A] 首次启动（无 config.json）')
    alive, wins, lines = run_once(work)
    exists = os.path.exists(cfg)
    print('  存活=%s 窗口数=%d config 生成=%s' % (alive, wins, exists))
    if exists:
        with open(cfg, 'r', encoding='utf-8') as f:
            data = json.load(f)
        print('  内容: %s' % json.dumps(data, ensure_ascii=False))
    okA = alive and wins == 1 and exists
    results.append(('A 首次启动生成默认配置', okA))

    # ---------------- B) 改值后重启生效 ----------------
    print('\n[B] 改 iconSize=64 / hoverScale=2.2 / bottomMargin=90')
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    data['iconSize'] = 64
    data['hoverScale'] = 2.2
    data['bottomMargin'] = 90
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run_once(work)
    loadln = find_line(lines, '[config] 载入 ok')
    print('  %s' % loadln)
    okB = alive and loadln and 'icon=64' in loadln and 'hover=2.20' in loadln and 'bottom=90' in loadln
    results.append(('B 改值重启后生效', bool(okB)))

    # ---------------- C) 坏值兜底且不覆盖用户文件 ----------------
    print('\n[C] 坏值 iconSize=9999 + 非法 bgColor')
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    data['iconSize'] = 9999
    data['bgColor'] = '不是颜色'
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    before = open(cfg, 'rb').read()
    alive, wins, lines = run_once(work)
    after = open(cfg, 'rb').read()
    gate = find_line(lines, 'iconSize = 9999')
    colorln = find_line(lines, 'bgColor')
    loadln = find_line(lines, '[config] 载入 ok')
    print('  闸门日志: %s' % gate)
    print('  颜色日志: %s' % colorln)
    print('  %s' % loadln)
    print('  用户文件未被改写: %s' % (before == after))
    okC = (alive and gate and colorln and before == after
           and loadln and 'icon=48' in loadln)
    results.append(('C 坏值回默认且不覆盖用户文件', bool(okC)))

    # ---------------- D) items 空数组 = 用户故意清空 ----------------
    print('\n[D] items = []（故意清空）')
    with open(cfg, 'r', encoding='utf-8') as f:
        data = json.load(f)
    data['items'] = []
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run_once(work)
    loadln = find_line(lines, '[config] 载入 ok')
    print('  %s' % loadln)
    okD = alive and loadln and '项数=0' in loadln
    results.append(('D items=[] 保持空（不被塞回默认）', bool(okD)))

    # ---------------- E) 还原为默认项，验证多项载入 ----------------
    print('\n[E] 还原 items（2 项）')
    data['items'] = [
        {'path': os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'explorer.exe')},
        {'path': os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'notepad.exe')},
    ]
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    alive, wins, lines = run_once(work)
    loadln = find_line(lines, '[config] 载入 ok')
    dockln = find_line(lines, '[dock] 配置里')
    print('  %s' % loadln)
    print('  %s' % dockln)
    okE = alive and wins == 1 and loadln and '项数=2' in loadln
    results.append(('E 多项载入正常', bool(okE)))

    # ---------------- F) 坏 JSON 不覆盖 ----------------
    print('\n[F] 整份 JSON 语法坏掉')
    bad = b'{ this is not json '
    with open(cfg, 'wb') as f:
        f.write(bad)
    alive, wins, lines = run_once(work)
    after = open(cfg, 'rb').read()
    parseerr = find_line(lines, 'JSON 解析失败')
    print('  %s' % parseerr)
    print('  用户文件未被改写: %s' % (after == bad))
    okF = alive and parseerr and after == bad
    results.append(('F 坏 JSON 回默认但不覆盖', bool(okF)))

    print('\n================ 结果 ================')
    allok = True
    for name, ok in results:
        print('  %s  %s' % ('PASS' if ok else 'FAIL', name))
        allok = allok and ok
    try:
        shutil.rmtree(work, ignore_errors=True)
    except Exception:
        pass
    print('======================================')
    return 0 if allok else 1


if __name__ == '__main__':
    py = sys.executable
    print('python: %s' % py)
    sys.exit(main())
