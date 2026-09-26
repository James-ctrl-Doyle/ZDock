# ZDock

Windows x64 桌面 Dock 栏。贴屏幕边缘的半透明面板 + 应用图标，支持悬停鱼眼放大、点击启动。

当前进度：**阶段二（配置持久化）已完成**（阶段一 gate 24/24，两个 bug 已根因级修复，见 `CHANGELOG.md`）。

## 现状

已实现：

- 贴主屏工作区底边居中的半透明圆角面板，背景色 / 圆角 / 底边留白**均可配置**
- 图标列表来自 **`config.json`**（首次启动自动写出默认配置），项数、图标大小、间距、悬停缩放、动画时长全部可调
- 真实应用图标，源图取 shell 的 **256px jumbo** 图标缓存，用 WIC 的 Fant 缩放预处理到峰值像素
- 悬停鱼眼放大：峰值倍率可配，相邻图标按距离衰减；锚点在图标底边中点（自下而上长大）
- 动画全部走 Composition keyframe，**不 relayout、不 repaint**
- halo 透明区鼠标穿透（窗口 region 方案，见下文）
- 左键启动图标（`ShellExecuteW`）
- **图标级右键菜单**：打开 / 以管理员身份打开 / 从 Dock 移除（移除即落盘 `config.json`）
- **空白处右键菜单**：添加程序… / 重新载入配置 / 退出 ZDock（手改配置不必重启进程）
- 独立顶层窗口：`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，不进 Alt+Tab、点击不抢焦点
- 启动日志写 exe 同目录 `ZDock.log`（UTF-8，1MB 轮转）
- 单实例：**自己命名的 mutex**（`Src/SingleInstance.*`）——
  ⚠ 不能用 `Ling::App::refuseSecondInstance()`，原因见「关键设计说明」最后一节

阶段一 gate 实测（`build-support/zdock_stage1_test.py`，**24/24 通过**）：

| gate | 结果 |
|---|---|
| 红线自检 | 窗口 `GA_PARENT` = 桌面、无 owner；启动前后 explorer 的 `WorkerW` 计数不变（14 → 14） |
| 鼠标穿透 | halo 点已不属于 dock（`WindowFromPoint` 返回下层窗口）；真实点击落到下层窗口 1 次；面板实体的点击被 dock 吃掉（下层 0 次） |
| 性能 | 空闲 10s CPU 增量 **0.000s**；悬停扫动 3s 期间 **≈3% 单核**；移开后回落 |
| 位置 | 面板底边 = 工作区底边 − 6 逻辑像素（实测 1433 = 1433）；水平居中（误差 0px） |
| 渲染 | 空闲/悬停两张 PrintWindow 截图差 15354 像素（证明放大动画确实在画） |

阶段二实测（`_probe_config.py` / `_probe_remove_item.py` / `_probe_ui_remove.py` / `_probe_ui_add.py`，**12 项全 PASS**）：

| 覆盖 | 结果 |
|---|---|
| 首次启动 | 自动写出带默认值的 `config.json`；Dock 正常显示 6 项 |
| 改值生效 | `iconSize=64` / `hoverScale=2.2` / `bottomMargin=90` 重启后日志与行为一致 |
| 坏值兜底 | `iconSize=9999` 回默认 48、非法 `bgColor` 回默认色，**且用户文件字节未变** |
| 坏 JSON | 整份语法坏掉 → 回默认值，**用户文件字节未变** |
| 空列表 | `items: []` 保持 0 项（不被塞回默认）；空 Dock 不崩 |
| 右键移除 | 真实菜单点击 → 落盘 → 界面从 4 项重建为 3 项 |
| 添加程序 | 空白右键 → 文件选择框 → 落盘 → 界面从 2 项重建为 3 项 |

截图见 `build/_review/`：`zdock_idle.png` / `zdock_hover.png`（阶段一）、
`stage2_default.png` / `stage2_hover.png` / `stage2_menu.png` / `stage2_menu_global.png`（阶段二）。

## 配置

exe 同目录的 `config.json`，首次启动自动生成。全部字段与默认值：

```jsonc
{
  "iconSize": 48,          // 图标基准边长（逻辑像素），范围 [16, 256]
  "iconGap": 12,           // 图标间距，[0, 128]
  "hoverScale": 1.7,       // 悬停峰值缩放，[1, 4]
  "animMs": 150,           // 放大/缩回动画时长（毫秒），[0, 2000]
  "bottomMargin": 6,       // 面板距工作区底边（逻辑像素），[0, 400]
                           //   任务栏设成自动隐藏时调大些可少抢底部热区
  "bgColor": "#1A1A1ACC",  // 面板背景色：#RRGGBB 或 #RRGGBBAA
  "cornerRadius": 12,      // 面板圆角，[0, 128]
  "items": [               // 图标列表；顺序即显示顺序
    { "path": "C:\\Windows\\explorer.exe" },
    { "path": "C:\\Windows\\System32\\notepad.exe", "name": "记事本" }
  ]
}
```

- `name` 可省略，省略时从文件名推。`items` 为空数组 = 故意清空 Dock，不会被塞回默认。
- **数值超范围回默认值**（不是钳到边界）—— 填 9999 明显是笔误，钳成 256 会得到一个
  你没想要又不易察觉的结果。
- **配置坏掉时不会覆盖你的文件**：JSON 语法错、字段类型错都只是回默认值，
  磁盘上的文件原样不动，日志里有一行说明哪个字段被忽略了。
- 改完**不必重启进程**：空白处右键 → 「重新载入配置」。
- 增删图标也可以完全走 UI：图标上右键 → 「从 Dock 移除」；空白处右键 → 「添加程序…」。

## 构建

```bash
bash build-support/build.sh
```

- 直接调 `cl.exe / link.exe / rc.exe`，**不走 MSBuild**（本机没有 .NET SDK，也不需要）。
- Ling 静态库来源按优先级：`$LING_ROOT` → `../Ling/dist/ling-v1.3.0-x64`（发布包）→ `../Ling`（源码树）。
  `LING_FROM_SOURCE=1` 强制用源码树。
- 产物：`build/bin/ZDock.exe`（约 340 KB）。

依赖的硬约定（踩过的坑，改构建脚本前先看 `build-support/build.sh` 里的注释）：

1. **不要把 Ling 的 `include/` 目录加进 `-I`** —— Windows 大小写不敏感，SDK 的 `<winbase.h>`
   会被 Ling 的 `include/WinBase.h` 抢走，`windows.h` 的基础类型整片消失，报错却指向
   `rpcasync.h` 的 `OVERLAPPED` 未定义。Ling 的头一律写成 `<include/xxx.h>`。
2. **运行时库必须 `/MT`**（与 Ling / ZPin 一致），否则 LNK2038。
3. **手工调 `link.exe` 要自己列出系统默认库**（user32 / shell32 / ole32 / comdlg32 …），
   MSBuild 才会替你加。
4. `wWinMain` 的第三参数是 `LPWSTR`（SDK 声明如此），写成 `LPTSTR` 在非 UNICODE 构建下撞声明报 C2731。
5. **用 WinRT JSON 必须同时引 `winrt/Windows.Foundation.Collections.h`**：
   `JsonObject::HasKey` / `JsonArray::Size/GetAt/Append` 都是 `IVector`/`IMap` 上的
   **auto 返回**函数，只引 `Windows.Data.Json.h` 会 C3779。

## 验证

```bash
<python> build-support/zdock_stage1_test.py          # 阶段一 gate（约 30s）
<python> build-support/_probe_config.py              # config.json 生成/读值/兜底/不覆盖（6 项）
<python> build-support/_probe_remove_item.py         # 删除与边界（含空 Dock）（5 项）
<python> build-support/_probe_ui_remove.py           # 真实 UI 走一遍"从 Dock 移除"
<python> build-support/_probe_ui_add.py              # 真实 UI 走一遍"添加程序…"
<python> build-support/_shot_stage2.py               # 阶段二验收截图
<python> build-support/_probe_hover_jitter.py        # 悬停抖动量化（阶段一 bug 2 的回归）
<python> build-support/_probe_cross_instance.py      # ZPin/ZDock 共存（阶段一 bug 1 的回归）
```

⚠ 涉及真实 UI 的三个脚本（`_probe_ui_*` / `_shot_stage2.py`）会**模拟鼠标点击与光标移动**，
跑的时候不要去动键盘鼠标 —— 模拟输入可能把焦点抢到你当前的活动窗口上。
它们全程在**隔离临时目录**里跑（复制一份 exe 过去），不会碰你手上的 `config.json`。

`ZDOCK_VERBOSE_HIT=1` 时 dock 会把每次命中判定写进日志（仅在结果变化或位移 >20px 时记一行）；
`ZDOCK_VERBOSE_HOVER=1` 记录每次 hover 变更。

## 目录

```
Src/
  main.cpp            入口：DPI 感知 → 单实例 mutex → Ling::init → 建 DockWin → 消息循环
  DockWin.h/.cpp      主窗口：布局、图标行、悬停、命中区域（region）、菜单、重建
  Config.h/.cpp       config.json 读写（默认值 / 兜底 / 原子替换）
  SingleInstance.h/.cpp  命名 mutex 单实例
  IconNode.h/.cpp     自绘图标节点：surface 绘制 + Composition 缩放动画
  IconLoader.h/.cpp   图标提取：SHGetImageList(jumbo) → HICON → WIC → D2D 位图
  Log.h/.cpp          轻量日志（exe 同目录 ZDock.log）
  Res/Resource.rc     VERSIONINFO（版本号唯一来源）
build-support/
  _msvc_env.sh        cl/link/rc 的最小环境
  build.sh            构建脚本
  zdock_stage1_test.py, _probe_*.py, _shot_stage2.py   验证 / 诊断 / 截图
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

### 菜单 / 对话框：**owner 必须是真实窗口，且要先摘掉 `WS_EX_NOACTIVATE`**

`Ling::App::popupMenu()` 把 owner 传成 `msgHwnd` —— 那是个 `HWND_MESSAGE` 的**消息专用窗口**。
消息专用窗口没有真实窗口层级，`SetForegroundWindow` 必然失败，`TrackPopupMenuEx`
找不到可归属的 owner，于是**菜单根本不显示、直接返回 0**。
（托盘场景恰好能用，因为托盘菜单走 shell 那条路；窗口内右键就不行。）

ZDock 改用 `DockWin::popupMenuHere()`：owner 用自己的真实 `hwnd`，并在弹出前
**临时摘掉 `WS_EX_NOACTIVATE`** 以便拿到前台权（弹完还原）。
`GetOpenFileNameW`（"添加程序…"）同理 —— 不摘这个标志的话，对话框可能被压在别的窗口后面，
用户以为"点了没反应"。不摘回来则会让"点图标不抢焦点"这条承诺失效。

### 配置读写：坏数据不能毁掉用户的文件

四条约定（见 `Src/Config.h` 的注释）：文件不存在就写默认值；**解析失败绝不覆盖**；
每个字段独立取值 + 独立兜底；保存走临时文件 + `MoveFileExW` 原子替换。
数值超范围**回默认值而非 clamp** —— 填 9999 明显是笔误，钳到边界会得到一个
用户没想要又不易察觉的结果。

`items` 是**空数组**时保持空（用户故意清空 Dock）；只有整个 `items` 键缺失才回默认表。

## 已知限制 / 下一步

- 悬停标签（名称气泡）未做。
- 显示器变化（`WM_DISPLAYCHANGE` / DPI 变化）没有处理 —— Ling 的窗口过程不暴露消息口，
  需要子类化或给 Ling 加钩子。
- 配置**改动后需要手动"重新载入配置"**（或重启）才生效；文件变更监听（`ReadDirectoryChangesW`）未做。
- 拖放排序 / 拖放添加未做。
- 菜单用的是系统默认 UI 字体，与面板自绘风格不完全统一。
- 未做窗口列表跟踪 / 运行指示 / 自动隐藏 / 多显示器 —— 属阶段三及以后。
- 忽略键盘与无障碍。
