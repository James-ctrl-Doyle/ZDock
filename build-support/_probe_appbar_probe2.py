"""验证：残留的 AppBar 记录能否被'重新 ABM_NEW + ABM_REMOVE'清掉，以及 ABN_POSCHANGED 的效果。"""
import ctypes, json, os, shutil, subprocess, tempfile, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
sh32 = ctypes.WinDLL('shell32', use_last_error=True)
SPI_GETWORKAREA = 0x0030
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]

def wa():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')

def run_case(name, cfg_extra, kill_after=True):
    d = tempfile.mkdtemp(prefix='zdock_ab_')
    shutil.copy2(EXE, os.path.join(d, 'ZDock.exe'))
    cfg = {"iconSize":48,"iconGap":12,"hoverScale":1.7,"animMs":150,"bottomMargin":6,
           "bgColor":"#1A1A1ACC","cornerRadius":12,
           "items":[{"path":r"C:\Windows\explorer.exe"}]}
    cfg.update(cfg_extra)
    with open(os.path.join(d,'config.json'),'w',encoding='utf-8') as f:
        json.dump(cfg,f)
    return d

print('--- 干净的起点 ---')
print('工作区:', wa())
time.sleep(0.3)

# 步骤 1：确认 Shell_TrayWnd 存在
tray = u32.FindWindowW('Shell_TrayWnd', None)
print('Shell_TrayWnd =', tray)
print()
