# ZDock

Windows x64 桌面 Dock 栏。贴屏幕边缘的半透明面板 + 应用图标，支持悬停鱼眼放大、点击启动。

当前进度：**阶段一（垂直切片 / gate）已完成并通过**（含两处 bug 修复，见 `CHANGELOG.md` 0.1.1）。

## 现状

已实现（阶段一范围）：

- 贴主屏工作区底边居中的半透明圆角面板（`#1A1A1A` @ 80% 不透明度，圆角 12 逻辑像素）
- 6 个真实应用图标，源图取 shell 的 **256px jumbo** 图标缓存，用 WIC 的 Fant 缩放预处理到峰值像素
- 悬停鱼眼放大：峰值 1.7×，相邻图标按距离衰减；锚点在图标底边中点（自下而上长大）
- 动画全部走 Composition keyframe，**不 relayout、不 repaint**
- halo 透明区鼠标穿透（窗口 region 方案，见下文）
- 左键启动图标（`ShellExecuteW`）、右键菜单（目前只有"退出 ZDock"）
- 独立顶层窗口：`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，不进 Alt+Tab、点击不抢焦点
- 启动日志写 exe 同目录 `ZDock.log`（UTF-8，1MB 轮转）
- 单实例：**自己命名的 mutex**（`Src/SingleInstance.*`）——
  ⚠ 不能用 `Ling::App::refuseSecondInstance()`，原因见「关键设计说明」最后一节

阶段一 gate 实测（`build-support/zdock_stage1_test.py`，**24/24 通过**）：

| gate | 结果 |
|---|---|
| 红线自检 | 窗口 `GA_PARENT` = 桌面、无 owner；启动前后 explorer 的 `WorkerW` 计数不变（14 → 14） |
| 鼠标穿透 | halo 点已不属于 dock（`WindowFromPoint` 返回下层窗口）；真实点击落到下层窗口 1 次；面板实体的点击被 dock 吃掉（下层 0 次） |
| 性能 | 空闲 10s CPU 增量 **0.000s**；悬停扫动 3s 期间 **≈2% 单核**；移开后回落 |
| 位置 | 面板底边 = 工作区底边 − 6 逻辑像素（实测 1433 = 1433）；水平居中（误差 0px） |
| 渲染 | 空闲/悬停两张 PrintWindow 截图差 9678 像素（证明放大动画确实在画） |

截图见 `build/_review/zdock_idle.png`（空闲）与 `zdock_hover.png`（悬停，中间图标放大）。

## 构建

```bash
bash build-support/build.sh
```

- 直接调 `cl.exe / link.exe / rc.exe`，**不走 MSBuild**（本机没有 .NET SDK，也不需要）。
- Ling 静态库来源按优先级：`$LING_ROOT` → `../Ling/dist/ling-v1.3.0-x64`（发布包）→ `../Ling`（源码树）。
  `LING_FROM_SOURCE=1` 强制用源码树。
- 产物：`build/bin/ZDock.exe`（约 330 KB）。

依赖的硬约定（踩过的坑，改构建脚本前先看 `build-support/build.sh` 里的注释）：

1. **不要把 Ling 的 `include/` 目录加进 `-I`** —— Windows 大小写不敏感，SDK 的 `<winbase.h>`
   会被 Ling 的 `include/WinBase.h` 抢走，`windows.h` 的基础类型整片消失，报错却指向
   `rpcasync.h` 的 `OVERLAPPED` 未定义。Ling 的头一律写成 `<include/xxx.h>`。
2. **运行时库必须 `/MT`**（与 Ling / ZPin 一致），否则 LNK2038。
3. **手工调 `link.exe` 要自己列出系统默认库**（user32 / shell32 / ole32 …），MSBuild 才会替你加。
4. `wWinMain` 的第三参数是 `LPWSTR`（SDK 声明如此），写成 `LPTSTR` 在非 UNICODE 构建下撞声明报 C2731。

## 验证

```bash
<python> build-support/zdock_stage1_test.py          # 完整 gate（约 30s，会真实移动鼠标，结束自动还原）
<python> build-support/_probe_passthrough.py         # 只查穿透：看命中归属与 [hit] 日志（需 ZDOCK_VERBOSE_HIT=1）
<python> build-support/_probe_click.py               # 只查"真实鼠标注入在这台机器上通不通"
```

`ZDOCK_VERBOSE_HIT=1` 时 dock 会把每次命中判定写进日志（仅在结果变化或位移 >20px 时记一行）。

## 目录

```
Src/
  main.cpp          入口：DPI 感知 → Ling::init → 建 DockWin → 消息循环
  DockWin.h/.cpp    主窗口：布局常量、图标行、悬停、命中区域（region）、菜单
  IconNode.h/.cpp   自绘图标节点：surface 绘制 + Composition 缩放动画
  IconLoader.h/.cpp 图标提取：SHGetImageList(jumbo) → HICON → WIC → D2D 位图
  Log.h/.cpp        轻量日志（exe 同目录 ZDock.log）
  Res/Resource.rc   VERSIONINFO（版本号唯一来源）
build-support/
  _msvc_env.sh      cl/link/rc 的最小环境
  build.sh          构建脚本
  zdock_stage1_test.py, _probe_*.py   验证与诊断脚本
```

## 关键设计说明

### 透明与鼠标穿透：**必须用窗口 region，`HTTRANSPARENT` 不通**

悬停放大要求图标溢出面板，所以窗口矩形必须比面板大（上方留 halo）。这圈透明像素如果参与
命中测试，就会挡住桌面/下层窗口的点击。

最初按"Composition 栈下用 `WM_NCHITTEST` 返回 `HTTRANSPARENT`"实现，**实测被证伪**：
dock 确实返回了 `HTTRANSPARENT`，但系统仍把该点判给 dock，下层窗口收不到任何点击
（`WindowFromPoint` 也返回 dock）。原因是 MSDN 对 `HTTRANSPARENT` 的说明限定在
**同一线程的窗口之间**继续下探，跨进程无效。

现方案：`SetWindowRgn` 把 halo 从窗口形状里挖掉 —— 区域外的像素连窗口都不算，
点击直接落到下面的窗口（跨进程有效）。命中区域 = **面板矩形 ∪ 当前放大中图标的可见框**，
只在 hover 变化时重算（动画期间图标的逻辑缩放已落到目标值，一次算准）。

实测证据见 `notes/2026-09-26-ZDock阶段一-穿透机制实测与region方案.md`。

### 图标清晰度

源图取 shell 的 256px jumbo 缓存（`SHGetImageList`，shell32 按**序号 727** 导出；
其 `IImageList` 是 COM 接口，**vtable 里 28 个方法一个都不能少、顺序不能错**，否则 `GetIcon` 落到错误槽位直接崩）。
提取链：`SHGetFileInfo(SYSICONINDEX)` → `IImageList::GetIcon` → `IWICImageFactory::CreateBitmapFromHICON`
→ **`IWICFormatConverter`（转 32bppPBGRA，漏掉这步 `CreateBitmapFromD2DBitmap` 会报 `0x88982F80`）**
→ `IWICBitmapScaler`（Fant 缩放）→ `ID2D1Bitmap`。

`IconNode` 的 drawing surface 按**放大后的峰值像素**绘制，而 `visual.Size` 是基准尺寸
（brush 拉伸填框）—— 放大到峰值时正好 1:1 采样，不会糊。

### 动画性能

放大动画只改 `visual.Scale`（Composition keyframe），不碰 yoga、不触发 `refresh()`。
`CenterPoint` 设在图标底边中点。UI 线程在动效期间几乎零占用（实测 1% 单核/整个进程）。

### 悬停判定：**看光标的真实位置，别看"上次收到消息的时间"**

原实现用「上次 `WM_NCHITTEST` 的时间戳 + 150ms 超时」判断鼠标还在不在图标上，
结果是图标**来回缩放**。因为 `WM_NCHITTEST` **只在鼠标移动时才来**，光标停住就断了：

1. 光标移入 → `WM_NCHITTEST` → 图标放大，时间戳刷新
2. 光标不动 → 系统不再发 `WM_NCHITTEST` → 时间戳冻结
3. 150ms 后定时器判定"鼠标走了" → 图标缩回
4. 缩回改变命中/渲染状态 → 系统补发一次 `WM_NCHITTEST` → 图标又放大
5. 回到 2，无限循环

现在统一走 `DockWin::refreshHoverFromCursor()`：`GetCursorPos` + `WindowFromPoint`
直接问系统"这个点还算不算我的窗口"（`WindowFromPoint` 会忽略被 region 挖掉的 halo 像素，
正好符合"不算在 dock 上"的定义）。这是事件驱动 + 单次查询，不是轮询，
也只查自身 UI 状态、不碰任何外部进程。

### 单实例：**不能用 `Ling::App::refuseSecondInstance()`**

Ling 那个方法靠 `FindWindow(L"STATIC", appID)` 判定，而 appID 来自
`App::App() : appID{ COMPILE_TIME_RAND_STR(6)}` —— 那个构造函数是编进
**预编译静态库 `Ling.lib`** 的，种子（`__TIME__` / `__COUNTER__`）取自**库自己的编译时刻**，
打库时就固化了。任何链接**同一个 Ling.lib** 的程序拿到的 appID 完全相同，
于是共用同一个 message-only 窗口槽位，后启动者必被先启动者误判成"第二实例"
并 `ExitProcess`（ZPin 与 ZDock 互相冲突的根因，实测两者 appID 都是 `Ling_Tf9K8M`）。

ZDock 改用自己命名的 mutex（`Local\ZDock.SingleInstance.{GUID}`，在 `Ling::init()` 之前占位）。
Ling 上游也已修：`App::appID` 改为按 exe 完整路径做哈希，不同程序必然不同。

## 已知限制 / 下一步

- 图标列表**暂时写死在代码里**（`DockWin::collectDefaultItems`），阶段二接 `config.json`。
- 右键菜单只有"退出"；图标级菜单（打开 / 管理员运行 / 移除）、设置界面未做。
- 悬停标签（名称气泡）未做。
- 显示器变化（`WM_DISPLAYCHANGE` / DPI 变化）没有处理 —— Ling 的窗口过程不暴露消息口，
  需要子类化或给 Ling 加钩子。
- 未做窗口列表跟踪 / 运行指示 / 自动隐藏 / 拖放 / 多显示器 —— 属阶段三及以后。
- 忽略键盘与无障碍（阶段一不涉及）。
