# CHANGELOG

## 0.1.3 · 阶段三（窗口跟踪 / 运行指示 / 临时图标）— 2026-09-26

给 Dock 装上"任务栏语义"：跟踪运行中的顶层窗口，固定图标亮起运行指示器，
没被固定但在跑的应用自动补临时图标，点图标切换窗口。

### 新增：`WindowTracker`（`Src/WindowTracker.h/.cpp`）

事件驱动的窗口跟踪器，**完全不轮询**（红线 4）。

- 主通道 `RegisterShellHookWindow`（`HSHELL_WINDOWCREATED/DESTROYED/ACTIVATED/
  RUDEAPPACTIVATED/REDRAW/FLASH`），自建 0x0 隐藏窗口收消息
  （Ling 的 `winProc` 是静态私有的，借不了 dock 主窗口）
- `SetWinEventHook` 只补三个 shell hook 拿不到的：`EVENT_SYSTEM_MINIMIZESTART/END`、
  `EVENT_OBJECT_NAMECHANGE`
- 过滤规则：可见顶层 + 无 owner + 非 `WS_EX_TOOLWINDOW` + 非 DWM cloaked
- 分组键**双轨**：AUMID 优先，否则小写 exe 路径；`ApplicationFrameWindow` 下钻取
  `Windows.UI.Core.CoreWindow`（**不无脑排除**，某 Dock 因此完全不可用）
- 显示名取 shell 的 `FileDescription`（"记事本" 而不是 "notepad"）
- `activateWindow()`：`AttachThreadInput` → `SetForegroundWindow` → detach
- 全屏判定：前台窗口 rect == 显示器 `rcMonitor`（**不是 `rcWork`**）+ 排除最小化

**为什么主用 shell hook**（实测，`_probe_track_api.py`）：同一个 3 秒窗口内，
`SetWinEventHook` 全订时能来 260 次 CREATE / 249 次 SHOW / 252 次 NAMECHANGE，
而 shell hook 只有个位数。订阅面窄是关键。

### 新增：运行指示器（`Src/IndicatorNode.h/.cpp`）

图标下缘 4px 圆点，强调色 `#4CC2FF`（任务书 §3）。前台应用满色、后台运行 `0xDD` 半透明。

⚠ 必须是**独立节点**，不能画在 `IconNode` 的 surface 上 —— 那张 surface 会随悬停放大
动画一起缩放，而指示器要恒定大小。

### 新增：临时图标 + 点击切换 + 分组列表

- `syncWithTracker()` 把图标列表与分组表对齐：固定项挂指示器；在跑但未固定的应用
  补临时图标（`Opacity 0.82` 与固定项区分）；窗口全关后自动回收
- 点图标：已运行 → 切窗口；多窗口 → 弹列表菜单（标题 + 「关闭全部窗口」）；
  未运行 → 启动
- 临时图标右键：固定到 Dock（写 `config.json`）/ 关闭全部窗口
- 按下反馈（缩到 0.92，100ms）、`HSHELL_FLASH` → 图标弹跳（`Offset.Y`，600ms）

### 修：三个 bug（都写进了 README「关键设计说明」）

1. **`sync ↔ rebuild` 无限循环** —— 第一版 `syncWithTracker()` 发现要增删临时图标时
   调 `rebuild()`，而 `rebuild()` 结尾又调回 `syncWithTracker()`。且 `rebuild()` 内部
   `items.clear()` 后只恢复固定项，导致 `needRebuild` 永远为真。日志里刷了 **23 次**
   "重建完成"。改成就地 `row->makeChild/removeChild` + `relayoutForItemCount()`。
2. **指示器坐标系错** —— Ling 的 `Node::x/y` 是**绝对坐标**（相对窗口客户区），
   不是相对父节点（实测 `row->x == row 里第一个 IconNode->x`）。
3. **指示器单位错** —— `node->x/y/w/h` 是**物理像素**，而 `setPosition` 内部
   **会再乘一次 dpi**。第一版把物理值直接喂进去，位置放大 1.24 倍跑到窗口外，
   透明合成窗口里毫无视觉反馈，看着就像"没画出来"。

### 改：验证方式（不再抢用户焦点）

阶段二用 `SetCursorPos + mouse_event` 点模态菜单，把用户的 WorkBuddy 焦点抢走、
对话任务被取消。阶段三的验证改成两条不抢焦点的路：**日志取证** + **`PostMessage` 注入**。
只有"悬停放大"这类必须真实光标位置的场景才用 `SetCursorPos`，且用完立刻恢复。

### 验证

- `_probe_track_api.py` **5/5 PASS**（两个事件机制可用、能从事件解析 exe 路径）
- `_probe_stage3.py` **13/13 PASS**（跟踪器启动、增量补临时图标、自动回收、点击不崩）
- 截图：`build/_review/stage3_{default,temp_icon,hover,menu_temp}.png`

产物体积：**669 KB → 734 KB**（增量来自窗口跟踪与属性查询，`propkey`/`dwmapi`/`shell32` 那批）。

---

## 0.1.2 · 阶段二（配置持久化）— 2026-09-26

把写死在代码里的那套参数和图标表挪进 `config.json`，并补上图标级菜单（增 / 删 / 打开）。

### 新增：`config.json`（`Src/Config.h/.cpp`）

与 **exe 同目录**的 `config.json`，首次启动自动写出带默认值的文件（用户能看到有哪些可调项）。

| 字段 | 默认 | 含义 |
|---|---|---|
| `iconSize` | `48` | 图标基准边长（逻辑像素），闸门 `[16, 256]` |
| `iconGap` | `12` | 图标间距，闸门 `[0, 128]` |
| `hoverScale` | `1.7` | 悬停峰值缩放，闸门 `[1, 4]` |
| `animMs` | `150` | 放大/缩回动画时长（毫秒），闸门 `[0, 2000]` |
| `bottomMargin` | `6` | 面板距工作区底边（逻辑像素），闸门 `[0, 400]` |
| `bgColor` | `"#1A1A1ACC"` | 面板背景色，`#RRGGBB` 或 `#RRGGBBAA` |
| `cornerRadius` | `12` | 面板圆角，闸门 `[0, 128]` |
| `items` | 系统自带 6 项 | 图标列表，每项 `{path, name?}` |

四条设计约定（与 ZPin 一致，便于两边共享心智）：

1. **文件不存在** → 用默认值并**把默认值写出去**（用户能看到可调项长什么样）
2. **文件存在但解析失败** → 用默认值，**绝不覆盖**用户文件
   （手写错一个字符就把整份配置抹掉，代价太大）
3. **每个字段独立取值 + 独立兜底**：缺字段用默认，坏字段不拖垮整份配置
4. **保存走"写临时文件 + `MoveFileExW` 原子替换"**，避免写一半断电留下半截 JSON

数值超范围**回默认值而不是 clamp** —— 用户填 9999 明显是笔误，钳到 256 会得到一个
他没想要又不易察觉的结果。`items` 是**空数组**时保持空（用户故意清空 Dock），
只有**整个 `items` 键缺失**才回默认表。

`bottomMargin` 就是上一轮拍板的"底部留白改成可配置项"：任务栏设成自动隐藏时调大它，
可以少抢底部热区。

### 新增：图标级右键菜单

右键**落在图标上** → 该项专属菜单：打开 / 以管理员身份打开 / 从 Dock 移除。
右键**落在空白处** → 全局菜单：添加程序… / 重新载入配置 / 退出 ZDock。

- 「从 Dock 移除」：先改内存 + 写盘（**写失败整体回退**，不让界面和磁盘不一致），再重建界面
- 「添加程序…」：`GetOpenFileNameW` 选 exe/lnk，**已在列表里的不重复加**
- 「重新载入配置」：手改完 `config.json` 不必重启进程
- 重建走 `body->removeAllChildren()` + 重跑构造那套，项数变化时窗口尺寸/位置一并重算

### 修：Ling 的 `popupMenu` 在窗口内右键**菜单根本不显示**

`App::popupMenu()` 把 owner 传成 `msgHwnd` —— 那是个 `HWND_MESSAGE` 的**消息专用窗口**。
消息专用窗口没有真实窗口层级，`SetForegroundWindow` 必然失败、`TrackPopupMenuEx`
找不到可归属的 owner，于是菜单不显示、直接返回 0。
（托盘场景恰好能用，因为托盘菜单走 shell 那条路。）

ZDock 改用 `DockWin::popupMenuHere()`：owner 用自己的真实 `hwnd`，
并在弹菜单前**临时摘掉 `WS_EX_NOACTIVATE`** 以便拿到前台权（弹完还原）——
同一个原因，`GetOpenFileNameW` 也要这么处理，否则对话框可能被压在别的窗口后面，
用户以为"点了没反应"。不摘这个标志是因为它带 `WS_EX_NOACTIVATE`，前台权拿不到。

### 其他

- `src/Config.cpp` 必须**同时** `#include <winrt/Windows.Foundation.Collections.h>`：
  `JsonObject::HasKey` / `JsonArray::Size/GetAt/Append` 都是 `IVector`/`IMap` 上的
  **auto 返回**函数，只引 `Windows.Data.Json.h` 会 C3779「要使用将会返回 auto 的函数，
  必须首先定义此函数」。
- 新增探针：`_probe_config.py`（生成/读值/兜底/**不覆盖**，6 项）、
  `_probe_remove_item.py`（删除/边界/空 Dock，5 项）、
  `_probe_ui_remove.py`（真实 UI 走一遍右键移除）、`_probe_ui_add.py`（真实 UI 走一遍添加）、
  `_shot_stage2.py`（验收截图）。**12 项全 PASS**，阶段一 gate 重跑 **24/24**，无回归。
- 验收图：`build/_review/stage2_default.png`（默认面板）、`stage2_hover.png`（悬停放大）、
  `stage2_menu.png`（图标菜单）、`stage2_menu_global.png`（全局菜单）。

### 本轮踩到并解决的坑

1. **`Config::load()` 忘了接线**：`Config` 写完了、`DockWin` 也接了 `applyConfig()`，
   但没人调 `load()` → 启动后 `config.json` 根本不生成。已在 `DockWin::create()` 最前面补上
   （`applyConfig` / `collectItemsFromConfig` 读的都是 Config 的内存态，载入必须最先）。
2. **探针不清日志会"假失败"**：`find_line` 取第一条匹配，而日志是追加的 →
   命中的是**上一轮**留下的旧行。`_probe_config.py` 第一版 6 项里错了 3 项，
   全是这个原因。修法：每轮开跑前删日志 + `find_line` 取**最后一条**匹配。
3. **模拟右键不移动光标 → 菜单弹错位置**：菜单弹出点来自 `GetCursorPos()`，
   只 `PostMessage(WM_RBUTTONUP)` 而不真的 `SetCursorPos`，菜单会弹在光标残留处
   （实测跑到 `(506,902)`）。`_probe_ui_add.py` 踩到。
4. **`PostMessage` 的 lParam 必须是 client 坐标**：Ling 的 `winProc` 用
   `GET_X_LPARAM(lParam)` 直接当客户区坐标，传屏幕坐标会算出窗口外的点 → 命中不到图标。
5. **模态菜单/对话框只能靠物理鼠标点**：`TrackPopupMenuEx` 有自己的模态消息循环，
   `keybd_event` 在 `WS_EX_NOACTIVATE` 窗口线程上不可靠（焦点拿不到）。
   改成 `SetCursorPos` + `mouse_event` 点菜单项坐标，或者直接 `FindWindowExW` 找控件 +
   `SendMessage(WM_SETTEXT / BM_CLICK)`。
   ⚠ **代价**：物理输入会波及用户当前的活动窗口（会把 WorkBuddy 的对话抢掉焦点）。
   取证优先用 `PostMessage` / `SendMessage`，**非必要不模拟物理输入**。
6. **菜单截图不能用 `PrintWindow`**（系统 `#32768` 窗口拍不出来），
   只能 `BitBlt` **菜单自身那一小块矩形**（实测约 214×108 像素）——
   绝不整屏 BitBlt，那会把用户的浏览器内容一起拍进去。

## 0.1.1 — 2026-09-26

修两个实测复现的 bug。都是**根因级**修复，不是绕过现象。

### Bug 1：ZDock 与 ZPin 互相把对方当成"第二实例"

**现象**：ZPin 在跑时启动 ZDock → ZDock 闪一下就没了（日志停在 `ling init done`，
连 `first instance` 都写不出来）；反过来 ZDock 在跑时启动 ZPin → ZPin 起不来。

**根因**（查证过程有两步，第一步的推断是错的，记录下来避免后人重走）：
- 起因是 `Ling::App::refuseSecondInstance()` 用 `FindWindow(L"STATIC", appID)`
  做单实例判定，而 appID 来自 `App::App() : appID{ COMPILE_TIME_RAND_STR(6)}`。
- 那个宏的种子是 `__TIME__` / `__COUNTER__` / `__LINE__`，**全部来自编译
  `App.cpp` 的时刻**；而 `App()` 是编进**预编译静态库 `Ling.lib`** 的。
  一旦打库，取值就永久固化 —— 跟链接它的程序是哪个、什么时候编的**毫无关系**。
- 实测：旧版 ZDock 日志打出 `appID=Ling_Tf9K8M` 后立刻判为 second instance 退出；
  而链接同一个 `ling-v1.3.0` 发布包的 ZPin 持有的就是**同一个** appID。
  于是两个程序争抢同一个 `类名 STATIC + 标题 Ling_Tf9K8M` 的 message-only 窗口槽位，
  后启动者必然 `FindWindow` 命中先启动者 → 给自己 `PostMessage(WM_APP+1)` →
  `App::exit(0)` 就是 `ExitProcess`，硬终止，日志都来不及刷。
- 附带伤害：`WM_APP+1` 恰好 = ZPin 托盘菜单"关闭所有快捷键"的 id（165），
  所以这个误投的消息还会顺手切掉 ZPin 那个开关。

**修复（两层）**：
- ZDock 侧：不再调用 Ling 的判定，改用自己命名的 mutex
  （`Src/SingleInstance.h/.cpp`，`Local\ZDock.SingleInstance.{GUID}`），
  在 `Ling::init()` **之前**占位。名字带程序前缀，天然与别的程序隔离。
- Ling 侧（根因）：`App::appID` 改为按 **exe 自身完整路径**做 FNV-1a 哈希
  （`Ling/src/App.cpp` 的 `makeAppID()`），同一个程序稳定、不同程序必然不同。
  并在 `Util.h` 给 `COMPILE_TIME_RAND_STR` 加了警告注释，说明它不能用于跨程序身份标识。

**验证**：ZPin→ZDock、ZDock→ZPin 两个方向都正常共存；
同一份二进制放两个不同目录跑出 `Ling_18D4E0F9919F` / `Ling_FFBD9BE82D79`（修复前必然相同）。

### Bug 2：悬停时图标来回变大变小，不能稳定保持放大

**现象**：鼠标停在某个图标上不动，图标持续地放大→缩回→放大，看着像在"呼吸"。

**根因**：`DockWin` 用「上次 `WM_NCHITTEST` 的时间戳 + 150ms 超时」判断鼠标还在不在。
但 **`WM_NCHITTEST` 只在鼠标移动时才来**，光标停住就断了，于是形成死循环：

1. 光标移入 → `WM_NCHITTEST` → hover=0，图标放大，时间戳刷新
2. 光标不动 → 系统不再发 `WM_NCHITTEST` → 时间戳冻结
3. 150ms 后定时器判定"鼠标走了" → hover=-1，图标缩回
4. 缩回改变命中/渲染状态 → 系统补发一次 `WM_NCHITTEST` → 时间戳刷新、图标又放大
5. 回到 2，无限循环

实测 `[hover]` 日志是每 250ms 一个完整的 `-1 → 0` 循环，与 150ms 超时 + 100ms 定时器周期吻合；
抓窗口像素统计"亮像素数"，波动 32.6%（10538 ↔ 15641）。

**修复**：判定依据改成**光标的真实位置**，而不是"上次收到消息的时间"
（`DockWin::refreshHoverFromCursor()`）：`GetCursorPos` + `WindowFromPoint` 直接问系统
"这个点还算不算我的窗口"。定时器降级为低频兜底。这是**事件驱动 + 单次查询**，
不是轮询（红线 4），且只查自身 UI 状态、不碰任何外部进程。

**验证**：三次采样波动 0.0%（恒为 15641，即放大后的稳定高位）；悬停扫动 CPU 1.02%（修复前 2.05%）。

### 其他

- 新增诊断探针：`_probe_cross_instance.py`（跨程序单实例）、
  `_probe_cross_instance_rev.py`（反向）、`_probe_hover_jitter.py`（悬停抖动量化）、
  `_probe_msgwin_real.py`（枚举 message-only 窗口）。
- `DockWin` 新增 `ZDOCK_VERBOSE_HOVER=1` 诊断开关，记录每次 hover 变更
  （原有的 `[hit]` 有去重逻辑，看不出"反复 applyHover"，排查抖动时必须靠它）。
- 阶段一 gate 重跑 **24/24 通过**，无回归。

## 0.1.0 · 阶段一（垂直切片 / gate）— 2026-09-26

第一条可用代码：贴边的半透明面板 + 6 个真实图标 + 悬停鱼眼放大 + halo 穿透。

- 新增工程骨架：`Src/`（main / DockWin / IconNode / IconLoader / Log / Res）、
  `build-support/`（`_msvc_env.sh` / `build.sh` / 验证脚本）。直接调 `cl.exe`+`link.exe`，
  不依赖 MSBuild 与 .NET SDK。
- 新增 `IconLoader`：shell 256px jumbo 图标 → HICON → WIC（格式转换 + Fant 缩放）→ D2D 位图。
- 新增 `IconNode`：自绘 surface（按峰值像素）+ Composition 缩放动画（锚点底边中点）。
- 新增 `DockWin`：贴主屏工作区底边居中、独立顶层窗口、鱼眼悬停、窗口 region 命中区域、
  左键启动、右键退出菜单。
- 新增 `Log`：exe 同目录 `ZDock.log`，UTF-8 + CRLF，1MB 轮转。
- 阶段一 gate 验证脚本 `zdock_stage1_test.py`：红线自检 / 穿透 A-B / 空闲与动效 CPU /
  位置 / 渲染截图，**24/24 通过**。

### 本轮踩到并解决的坑（都有实测依据）

1. **`HTTRANSPARENT` 跨进程无效**，改用 `SetWindowRgn`（窗口 region）挖掉 halo。
   这是对既有评估结论的修正 —— 详见 README「关键设计说明」。
2. **Ling 的 `include/` 不能进 `-I`**：会劫持 SDK 的 `<winbase.h>`（大小写不敏感），
   症状是 `rpcasync.h` 报 `OVERLAPPED` 未定义。Ling 头统一 `<include/xxx.h>`。
3. **`CreateBitmapFromHICON` 的结果不能直接喂 `CreateBitmapFromWicBitmap`**（`0x88982F80`），
   必须先过 `IWICFormatConverter` 转 32bppPBGRA。
4. 手工 `link.exe` 要显式列系统库；运行时库要 `/MT`；`wWinMain` 第三参数是 `LPWSTR`。
5. ctypes 测试窗**必须有消息泵**才会收到 `WM_LBUTTONDOWN`（排队消息）；
   且点击会激活窗口把它抬到顶层带最前，要 `WS_EX_NOACTIVATE` + 每次点击前重新钉回 dock 下方。
