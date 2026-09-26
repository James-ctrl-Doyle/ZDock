# CHANGELOG

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
