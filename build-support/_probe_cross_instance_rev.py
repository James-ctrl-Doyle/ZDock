"""端到端复现 bug 1 的反向：ZDock 先跑，ZPin 能否正常起来。

用户体感是"ZDock 运行时开启 ZPin 则没有反应但 ZPin 也不会正常开启"。
修好 ZDock 侧之后，这条路也该通 —— 因为病根在 Ling 的共享 appID，
但现在 ZDock 不再建那个共享消息窗、也不再调 Ling 的单实例判定，
ZPin 侧就该恢复正常。

判据：ZPin 进程活下来，且 ZDock 不被 ZPin 的启动带崩。

跑法： <python> build-support/_probe_cross_instance_rev.py
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _probe_cross_instance import (  # noqa: E402
    ZDOCK, ZPIN, ZDOCK_LOG, proc_map, dock_hwnds, log_tail)

import ctypes
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)


def zpin_windows():
    """ZPin 起来后会有托盘图标，但没有常规顶层窗口（它是托盘常驻）。
    判据用进程存活 + 托盘图标注册。这里只查进程，托盘由 pid 覆盖到 explorer 图标上，
    脚本层面不额外验证（避免读用户的通知区域配置）。"""
    pm = proc_map()
    return [p for p in pm.values() if p.lower().startswith('zpin')]


def main():
    for p in (ZDOCK_LOG, ZDOCK_LOG + '.1'):
        if os.path.exists(p):
            os.remove(p)
    if not ZPIN or not os.path.isfile(ZDOCK):
        print('缺 ZDock.exe 或 ZPin 产物，跳过')
        return 2

    print('--- 1) 起 ZDock ---')
    zdock = subprocess.Popen([ZDOCK], cwd=os.path.dirname(ZDOCK))
    time.sleep(3.0)
    print('  ZDock 存活=%s 窗口数=%d' % (zdock.poll() is None, len(dock_hwnds())))
    if zdock.poll() is not None:
        print('  !! ZDock 自己就没起来，先查它')
        return 1

    print('--- 2) 起 ZPin（ZDock 已在跑）---')
    zpin = subprocess.Popen([ZPIN], cwd=os.path.dirname(ZPIN))
    time.sleep(6.0)
    alive_after = zdock.poll() is None
    print('  ZPin 进程: %s' % zpin_windows())
    print('  ZDock 仍存活=%s 窗口数=%d' % (alive_after, len(dock_hwnds())))
    print('  ZDock 日志尾部:')
    for ln in log_tail(ZDOCK_LOG, 6):
        print('    ' + ln)

    ok = bool(zpin_windows()) and alive_after
    print('  => %s' % ('双向共存正常' if ok else '仍有问题'))

    print('--- 3) 收尾 ---')
    for p in (zdock, zpin):
        try:
            p.terminate()
        except Exception:
            pass
    time.sleep(1.5)
    for p in (zdock, zpin):
        if p.poll() is None:
            try:
                p.kill()
            except Exception:
                pass
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
