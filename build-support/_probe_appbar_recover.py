import ctypes, json, os, shutil, subprocess, sys, tempfile, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
SPI_GETWORKAREA = 0x0030
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

def wa():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')

d = tempfile.mkdtemp(prefix='zdock_appbar_')
shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
cfg = {"iconSize":48,"iconGap":12,"hoverScale":1.7,"animMs":150,"bottomMargin":6,
       "bgColor":"#1A1A1ACC","cornerRadius":12,"reserveWorkArea":True,
       "items":[{"path":r"C:\Windows\explorer.exe"}]}
with open(os.path.join(d,'config.json'),'w',encoding='utf-8') as f:
    json.dump(cfg,f)

base = wa()
print('原始工作区底 =', base[3])
p = subprocess.Popen([os.path.join(d,'ZDock.exe')], cwd=d)
time.sleep(1.8)
print('注册后工作区底 =', wa()[3])
print('强杀...')
subprocess.run(['taskkill','/F','/IM','ZDock.exe'], capture_output=True)
for i in range(1, 16):
    time.sleep(1.0)
    cur = wa()
    print('  %2ds 后 工作区底 = %d %s' % (i, cur[3], '(已恢复)' if cur[3]==base[3] else ''))
    if cur[3] == base[3]:
        break
shutil.rmtree(d, ignore_errors=True)
