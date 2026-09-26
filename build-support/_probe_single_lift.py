"""单份占用实测：干净工作区上开一个 ZDock（reserveWorkArea=true），量它占了多少 px。
不抢焦点（不模拟输入），只在临时目录跑。
"""
import json, os, shutil, subprocess, time, ctypes, sys
from ctypes import wintypes as wt
u32 = ctypes.WinDLL('user32', use_last_error=True)
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
SPI_GETWORKAREA = 0x0030
def wa():
    r = wt.RECT(); u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

ROOT = r"C:/Users/zqw35/WorkBuddy/projects/ZDock"
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = os.path.join(BIN, 'ZDock.exe')
d = r"C:/Users/zqw35/WorkBuddy/projects/ZDock/_tmp_single"
shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
cfg = json.load(open(os.path.join(BIN, 'config.json'), encoding='utf-8'))
cfg['autoHide'] = False
cfg['reserveWorkArea'] = True
json.dump(cfg, open(os.path.join(d, 'config.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=2)

print('启动前工作区 =', wa())
p = subprocess.Popen([os.path.join(d, 'ZDock.exe')], cwd=d)
time.sleep(2.5)
after = wa()
print('启动后工作区 =', after)
print('单份占用 =', wa()[3])
# 读日志
log = os.path.join(d, 'zdock.log')
if os.path.exists(log):
    for line in open(log, encoding='utf-8', errors='replace').read().splitlines():
        if 'appbar' in line.lower() or '工作区' in line:
            print('  LOG:', line.strip()[:160])
# 正常退出
hw = u32.FindWindowW('ZDock', None)
if hw: u32.PostMessageW(hw, 0x0010, 0, 0)
time.sleep(1.5)
print('退出后工作区 =', wa())
try: p.kill()
except Exception: pass
time.sleep(0.5)
shutil.rmtree(d, ignore_errors=True)
