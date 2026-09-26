"""隔离验证：故意制造一条死记录，再看正常注册会占多少。
用来区分"残留叠加"和"自引用回路"这两个曾经混在一起的症状。
"""
import json, os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _probe_stage4 as P

ROOT = r"C:/Users/zqw35/WorkBuddy/projects/ZDock"
BIN = os.path.join(ROOT, 'build', 'bin')

def mk(dirname, cfg_extra):
    d = os.path.join(ROOT, dirname)
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    shutil.copy2(os.path.join(BIN, 'ZDock.exe'), os.path.join(d, 'ZDock.exe'))
    cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
    cfg.update(cfg_extra)
    json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)
    return d

P.clean_work_area()
print('基线 eaten =', P.probe_shell_stale(), 'px')

# 制造死记录：起一个开预留的实例 → 强杀（删掉 state 文件，模拟"自愈后仍注册"）
d = mk('_tmp_r1', {'autoHide': False, 'reserveWorkArea': True})
p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
time.sleep(2.5)
print('第一次启动后工作区底 =', P.work_area()[3])
subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True, shell=False)
time.sleep(1.0)
# 关键：**删掉 state 文件**，这样下次启动不会走"跳过注册"分支，
# 而是老老实实注册一次 —— 看会不会叠加。
st = os.path.join(d, 'zdock-appbar.state')
if os.path.exists(st): os.remove(st)
print('删掉 state 文件（模拟不跳过）; 工作区底 =', P.work_area()[3])

p2 = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
time.sleep(2.5)
print('第二次启动（有残留、不跳过）后工作区底 =', P.work_area()[3], '  <-- 若=1345 说明不叠加')
log = os.path.join(d, 'zdock.log')
for line in open(log, encoding='utf-8', errors='replace').read().splitlines():
    if 'appbar' in line:
        print('  LOG:', line.split(']')[-1].strip()[:130])
subprocess.run(['taskkill', '/F', '/IM', 'ZDock.exe'], capture_output=True, shell=False)
time.sleep(0.5)
shutil.rmtree(d, ignore_errors=True)
P.clean_work_area()
print('结束工作区 =', P.work_area())
