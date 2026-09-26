"""实测：强杀留下的死 AppBar 记录，到底靠什么、多久才被清掉？

这决定了红线 7 的"自愈"该怎么写。三种候选：
  A) 自行过期（等一会儿就好）
  B) 下一次完整 NEW/SETPOS(空)/REMOVE 时被 shell 顺带清掉
  C) 只有重启 explorer 才清

做法：强杀一个开着预留的 ZDock → 立刻测 eaten → 等 10s 再测 → 走一次探测循环再测。
"""
import ctypes, json, os, shutil, subprocess, sys, time
from ctypes import wintypes as wt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _probe_stage4 as P

ROOT = r"C:/Users/zqw35/WorkBuddy/projects/ZDock"
BIN = os.path.join(ROOT, 'build', 'bin')
d = os.path.join(ROOT, '_tmp_stale')

def make():
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    shutil.copy2(os.path.join(BIN, 'ZDock.exe'), os.path.join(d, 'ZDock.exe'))
    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg['autoHide'] = False; cfg['reserveWorkArea'] = True
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

P.clean_work_area()
print('1) 干净基线 eaten =', P.probe_shell_stale(), 'px')

make()
p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
time.sleep(2.5)
print('2) ZDock 起来后工作区底 =', P.work_area()[3])
print('   state 文件存在 =', os.path.exists(os.path.join(d, 'zdock-appbar.state')))

# 强杀（不给 ABM_REMOVE 机会）
subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'],
               capture_output=True, shell=False)
time.sleep(1.0)
print('3) 强杀后工作区底 =', P.work_area()[3], '（= 残留仍在）')

# 清工作区数值，但**不动 shell 记录**，然后立刻测 eaten
P.clean_work_area()
print('4) 只 clean 工作区数值后再测 eaten =', P.probe_shell_stale(), 'px  <-- 关键判据')

shutil.rmtree(d, ignore_errors=True)
P.clean_work_area()
print('结束工作区 =', P.work_area())
