"""针对性验证：无路径无 AUMID 的窗口被过滤，正常应用照常收录。

只验这一条改动路径（WindowTracker::addWindow 的新收录门槛）：
  1) 日志出现 "[track] 跳过无路径无 AUMID 的窗口 ... ParsecMinFrameRate5"
  2) 日志**不再**出现 "key=hwnd:" 这种退化分组
  3) explorer / Chrome / WorkBuddy 等正常应用照常成组

跑法： <python> build-support/_verify_orphan_filter.py
会启动一次 build/bin/ZDock.exe，约 5 秒后自动关掉（不预留工作区、不抢焦点）。
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _probe_common as pc  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\zqw35\WorkBuddy\projects\ZDock\build\bin'
EXE = os.path.join(BIN, 'ZDock.exe')
LOG = os.path.join(BIN, 'ZDock.log')

pc.kill_existing_zdock()
if os.path.exists(LOG):
    os.remove(LOG)

proc = subprocess.Popen([EXE], cwd=BIN)
time.sleep(5)
pc.kill_existing_zdock()

with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
    txt = f.read()

lines = txt.splitlines()
print('=' * 74)
print('日志行数：', len(lines))
print('=' * 74)

skipped = [l for l in lines if '跳过无路径无 AUMID' in l]
groups = [l for l in lines if '分组 key=' in l]
hwnd_groups = [l for l in groups if 'key=hwnd:' in l]
dockline = [l for l in lines if '分组变化' in l or '配置里' in l]

print('【被门槛拦下的窗口】')
for l in skipped:
    print('   ', l)
print('【实际成组】')
for l in groups:
    print('   ', l)
print('【退化分组 key=hwnd: / 名称=应用】', len(hwnd_groups))
print('【dock 项数】')
for l in dockline[:4]:
    print('   ', l)

ok1 = any('ParsecMinFrameRate5' in l for l in skipped)
ok2 = not hwnd_groups and not any('名称=应用' in l for l in groups)
# 正常应用必须照常成组：至少有 1 个 path:/aumid: 形式的分组。
# ⚠ 不要断言"必须有 explorer"—— 那是**配置相关**的：用户此刻没开资源管理器窗口，
#   或跑的是另一份 config，都会让这条断言假失败（本探针就踩过这个坑）。
ok3 = any('key=path:' in l or 'key=aumid:' in l for l in groups)

print('=' * 74)
print(f'  1) Parsec 窗口被过滤          : {"PASS" if ok1 else "FAIL"}')
print(f'  2) 无 hwnd: / "应用" 退化分组  : {"PASS" if ok2 else "FAIL"}')
print(f'  3) 正常应用照常成组            : {"PASS" if ok3 else "FAIL"}')
print('=' * 74)
print('RESULT:', 'PASS' if (ok1 and ok2 and ok3) else 'FAIL')
