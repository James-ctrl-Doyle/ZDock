import ctypes, json, os, shutil, subprocess, sys, tempfile, time
from ctypes import wintypes as wt

u32 = ctypes.WinDLL('user32', use_last_error=True)
SPI_GETWORKAREA = 0x0030
u32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND

def wa():
    r = wt.RECT()
    u32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(r), 0)
    return (r.left, r.top, r.right, r.bottom)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'ZDock.exe')

d = tempfile.mkdtemp(prefix='zdock_appbar2_')
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

# 正常退出：给 dock 主窗口发 WM_CLOSE
hwnd = u32.FindWindowW('ZDock', None)
print('dock hwnd =', hwnd)
u32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
time.sleep(1.5)
print('正常退出后进程存活 =', p.poll() is None)
print('正常退出后工作区底 =', wa()[3], '(原始 %d)' % base[3])
subprocess.run(['taskkill','/F','/IM','ZDock.exe'], capture_output=True)
shutil.rmtree(d, ignore_errors=True)
