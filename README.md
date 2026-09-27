# ZDock

Windows x64 桌面 Dock 栏。贴屏幕边缘的半透明面板 + 应用图标，支持悬停鱼眼放大、点击启动。

当前进度：**阶段六（位置配置 / 设置窗口 / 开机自启 / 多显示器锚定）已完成（v0.1.7）**，
阶段七（本地化 + 崩溃恢复 + 诊断导出）未开始。
（v0.1.4 ~ v0.1.7 均已发布 GitHub Release；依赖 **Ling v1.3.1**。
阶段一 gate 24/24，阶段二 12 项、阶段三 13 项、阶段四 48 项、显示环境 26 项、
拖放 14 项、预览 13 项、位置配置 18 项、设置/自启/显示器 17 项探针全 PASS；
过程中挖出的 bug 已根因级修复，见 `CHANGELOG.md`）。

## 现状

已实现：

- 贴主屏工作区底边居中的半透明圆角面板，背景色 / 圆角 / 底边留白**均可配置**
- 图标列表来自 **`config.json`**（首次启动自动写出默认配置），项数、图标大小、间距、悬停缩放、动画时长全部可调
- 真实应用图标，源图取 shell 的 **256px jumbo** 图标缓存，用 WIC 的 Fant 缩放预处理到峰值像素
- 悬停鱼眼放大：峰值倍率可配，相邻图标按距离衰减；锚点在图标底边中点（自下而上长大）
- 动画全部走 Composition keyframe，**不 relayout、不 repaint**
- halo 透明区鼠标穿透（窗口 region 方案，见下文）
- **事件驱动窗口跟踪**（`Src/WindowTracker.*`）：`RegisterShellHookWindow` 为主通道 +
  3 个 `SetWinEventHook` 补充，**完全不轮询**
- **运行指示器**：图标下缘 4px 圆点（强调色 `#4CC2FF`），前台应用满色、后台运行半透明
- **临时图标**：在跑但没被固定到 Dock 的应用自动补图标（`Opacity 0.82` 与固定项区分），
  窗口全关后自动回收
- **点击切换**：已运行 → 切到那个窗口；多窗口给列表菜单（含「关闭全部窗口」）；未运行 → 启动
- 按下反馈（缩到 0.92）与注意请求弹跳（`HSHELL_FLASH` → 图标上弹一次）
- **图标级右键菜单**：固定项给 打开 / 以管理员身份打开 / 从 Dock 移除；
  临时项给 固定到 Dock / 关闭全部窗口
- **空白处右键菜单**：添加程序… / 重新载入配置 / 退出 ZDock（手改配置不必重启进程）
- **自动隐藏**（`Src/EdgeHotZone.*`）：鼠标离开 + 无悬停/拖放/菜单会话 → 约 **500ms** 后
  滑出屏幕，**200ms 滑入 / 300ms ease-out 滑出**。触发展开靠一条贴屏幕底边、**3px 厚**、
  宽 `max(dock 宽, 屏宽/2)` 的**全透明细窗**（`WS_EX_NOACTIVATE | TOOLWINDOW | TOPMOST | LAYERED`）
  —— **不用鼠标钩子、不轮询**（红线 4/5）。
- **全屏让位**：有全屏应用在前台时 dock 保持隐藏（可配 `hideOnFullscreen: false` 关掉）。
  全屏期间碰热区也不会拱出来。
- **工作区预留（AppBar，默认关）**：`SHAppBarMessage` 的
  `ABM_NEW / ABM_QUERYPOS / ABM_SETPOS / ABM_REMOVE` + `ABN_*` 通知，
  申请后普通窗口最大化会自动避开 dock 那 95px。**绝不碰桌面窗口 / 任务栏状态**。
- **强杀自愈**（红线 7）：AppBar 的注销是应用自己的责任 —— 被 `taskkill /F` 之后
  shell 会一直保留那条记录（实测等 15 秒也不恢复）。ZDock 用**状态文件**
  （`zdock-appbar.state`，exe 同目录）记下抢占前的工作区，下次启动发现它还在就
  ① `SPI_SETWORKAREA` 写回干净值 ② 跑一次 `NEW/SETPOS(空)/REMOVE` 把 shell 里的
  死记录扫掉（**不扫掉的话再注册会叠加，工作区被占两道**），然后正常注册。详见下文。
- **explorer 重启自愈**：监听 `RegisterWindowMessageW(L"TaskbarCreated")` 广播，
  重新注册 AppBar 与 shell hook（任务栏重建后协调关系会失效）。
- **显示环境变化自适应**：换显示器 / 改分辨率 / 改缩放比之后自动重排（尺寸 + 位置 +
  热区 + AppBar 一起走）。两条来源：`WM_DISPLAYCHANGE` / `WM_SETTINGCHANGE`
  （借跟踪器的隐藏顶层窗口收广播）+ Ling 的 `onDpiChanged`。
- **悬停预览**（`Src/PreviewWin.*`）：鼠标停在图标上约 **300ms** 弹出预览气泡，
  显示该应用窗口的**实时画面**（DWM 缩略图，零截图成本、不用定时刷新）；移开即收起。
- **拖文件打开**：拖文件到程序图标上松手 → 用该程序打开；到文件夹图标 → 复制进去。
  实现靠**子类化 dock 窗口**拿 `WM_DROPFILES`（Ling 不暴露消息口）。
- **设置窗口**（`Src/SettingsWin.*`）：右键空白处 →「设置…」，**自绘**（Ling 的
  Button / Slider / Label），四组 13 项，**改动即时生效**、关窗时写回 config.json。
- **开机自启**（`Src/AutoStart.*`）：HKCU Run 项，不需要管理员权限。
- **多显示器锚定**（`Src/MonitorUtil.*`）：`monitorIndex` 可选锚到哪台显示器，
  越界自动回主屏。定位基准一律 `rcMonitor`。
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

阶段三实测（`_probe_track_api.py` 5/5 + `_probe_stage3.py` **13/13**）：

| 覆盖 | 结果 |
|---|---|
| 事件机制可用 | `SetWinEventHook` / `RegisterShellHookWindow` 都能收到跨进程事件，能从事件解析出 exe 路径 |
| 事件量 | 3 秒内 win event 260 CREATE / 249 SHOW / 252 NAMECHANGE；shell hook 个位数 → 主用后者 |
| 跟踪器启动 | 日志有 `[track] 已启动 shellHookMsg=0xC028 hooks=true/true/true` 与全量重建 |
| 增量补临时图标 | 启动 `charmap`（不在 config）→ 日志 `运行 5 组，共 9 项（其中临时 4）` |
| 自动回收 | 关掉它 → `运行 4 组，共 8 项（其中临时 3）` |
| 点击不崩 | `PostMessage` 投递左键到图标位置后进程存活 |

阶段四实测（`_probe_stage4.py`，**48/48 通过**）：

| 覆盖 | 结果 |
|---|---|
| 自动隐藏关闭 | 日志说明未启用，**没有热区窗口**（不留看不见却吃点击的隐形窗） |
| 热区几何 | 厚 **3px**、贴**屏幕**底边、宽 ≥ 屏宽/2、`NOACTIVATE` / `TOOLWINDOW` / `TOPMOST` / `LAYERED` 全对 |
| 热区触发 | `PostMessage(WM_MOUSEMOVE)` → 日志 `[hotzone] 鼠标进入，触发滑入`（**不移动真实光标**） |
| AppBar 关闭 | 不注册，工作区不动（1440 → 1440） |
| AppBar 开启 | 注册成功，工作区底 **1440 → 1345**，正好抬升 **95px**（= 面板高） |
| 全屏让位 | 注入全屏 → dock 滑到屏幕外（顶=1440）、日志有 `滑出开始`；退出 → 滑回；鼠标不在 dock 上时自动收回 |
| 全屏期间碰热区 | dock **保持隐藏**（不拱出全屏画面） |
| 强杀自愈（红线 7） | 强杀后工作区确实留在 1345（复现系统限制）→ 重启：日志 `强杀自愈：工作区从 (0,38)-(2560,1345) 恢复到 (0,38)-(2560,1440)` + `清残留` 循环完成 → 重新只占 **95px**（不叠加） |
| 正常退出 | 工作区底回到 1440，`zdock-appbar.state` 被删 |
| explorer 重启 | 广播 `TaskbarCreated` → `收到 TaskbarCreated 广播` + `自愈完成`，热区仍在、进程存活 |
| 可重复性 | 探针连跑两轮均 48/48，环境自检 `干净工作区上注册 1px AppBar 只吃掉 0px` |

### 阶段五（v0.1.5，`_probe_drop.py` **14/14** + `_probe_preview.py` **13/13**）

| 覆盖 | 结果 |
|---|---|
| 拖 2 个文件到文件夹图标 | 两个文件**真的出现在**目标文件夹 + 日志 `已复制 2 个文件` |
| 拖到程序图标 | 走到"用该程序打开"分支（用不存在的 exe，零副作用证明分支被走到） |
| 拖到非图标区 | 日志 `落点不在图标上 → 忽略`，且没有误复制 |
| 拖目录进来 | 日志 `跳过目录`，目标文件夹里没出现它 |
| 悬停前 | **没有**预览窗口（按需创建） |
| 悬停 300ms 后 | 弹出预览；日志 `显示预览：目标=0x… 标题=「Python」` |
| 预览位置 | 贴在**图标顶边**上方、水平对齐图标 |
| **预览画面** | 预览窗口里数到 **46128 个品红像素** → DWM 缩略图确实合成进来了 |
| 移开鼠标 | 预览收起 |

### 最贵假设的先验（两个最小 demo）

`_probe_dwm_thumb.cpp` / `_probe_dwm_thumb_ling.cpp` —— DWM 缩略图宿主可行性：

| 宿主形态 | `DwmRegisterThumbnail` | 画面是否合成进来 |
|---|---|---|
| 普通窗口 | S_OK | **是**（品红像素 19200/19200） |
| `WS_EX_NOREDIRECTIONBITMAP` | S_OK | **是** |
| `WS_EX_LAYERED` | S_OK | **是** |
| **真 Ling 窗口** | S_OK | **是** |

`_probe_subclass.cpp` —— 子类化 Ling 窗口：能收消息、`GWLP_USERDATA` 不被破坏、
转发链正常、可还原。`_probe_ling_appid.sh` —— 运行期读 Ling 的 appID 验证升级。

截图见 `build/_review/`：`zdock_idle.png` / `zdock_hover.png`（阶段一）、
`stage2_default.png` / `stage2_hover.png` / `stage2_menu.png` / `stage2_menu_global.png`（阶段二）、
`stage3_default.png` / `stage3_temp_icon.png` / `stage3_hover.png` / `stage3_menu_temp.png`（阶段三）、
`stage4_1_展开态.png` / `stage4_2_隐藏态滑出.png` / `stage4_3_工作区预留.png`（阶段四）。

### 显示环境变化（v0.1.4 新增，`_probe_display_change.py` **26/26**）

| 覆盖 | 结果 |
|---|---|
| `WM_DISPLAYCHANGE` 广播 | 收到 + 触发重排 + 重排后仍贴屏幕底、水平居中、尺寸未变 |
| 跨进程**野指针**消息 | 投带野指针的 `WM_SETTINGCHANGE` / `WM_DPICHANGED` → **不崩**、被防护挡住、位置未被破坏 |
| 隐藏态遇重排 | 全屏让位滑出状态下重排 → dock **不会**从屏幕底边冒出来 |
| 让位与自动隐藏解耦 | `autoHide=false` + `hideOnFullscreen=true` → 让位仍生效、退出后常驻、且没有多余热区窗口 |

### 依赖升级验证（`_probe_ling_appid.sh`）

把同一份探针 exe 放两个不同目录，运行期读 `Ling::App::appID`：

| Ling 包 | 目录 A | 目录 B | 结论 |
|---|---|---|---|
| v1.2.0 | `Ling_Tf9sqM` | `Ling_Tf9sqM` | 相同 → 旧行为（编译期常量） |
| **v1.3.1** | `Ling_2534E9F3804D` | `Ling_9B5D4D1B7594` | **不同 → 按 exe 路径哈希** |

## 配置

exe 同目录的 `config.json`，首次启动自动生成。全部字段与默认值：

```jsonc
{
  "iconSize": 48,          // 图标基准边长（逻辑像素），范围 [16, 256]
  "iconGap": 12,           // 图标间距，[0, 128]
  "hoverScale": 1.7,       // 悬停峰值缩放，[1, 4]
  "animMs": 150,           // 放大/缩回动画时长（毫秒），[0, 2000]
  "bottomMargin": 6,       // 面板距屏幕底边（逻辑像素），[0, 400]
                           //   任务栏设成自动隐藏时调大些可少抢底部热区
  "bgColor": "#1A1A1ACC",  // 面板背景色：#RRGGBB 或 #RRGGBBAA
  "cornerRadius": 12,      // 面板圆角，[0, 128]
  "autoHide": false,       // 自动隐藏：鼠标离开约 500ms 后滑出屏幕
  "autoHideDelayMs": 500,  // 离开到滑出的延迟（毫秒），[0, 10000]
  "slideInMs": 200,        // 滑入动画时长，[0, 3000]
  "slideOutMs": 300,       // 滑出动画时长（ease-out），[0, 3000]
  "hideOnFullscreen": true,// 有全屏应用在前台时保持隐藏
  "reserveWorkArea": false,// 向系统申请工作区（AppBar）—— 最大化窗口会避开 dock

  // ---- 阶段六：位置 / 外观 ----
  "dockEdge": "bottom",    // 停靠边：bottom(默认) / top / left / right
  "dockAlign": "center",   // 沿停靠边的对齐：start / center(默认) / end
  "dockOffset": 0,         // 沿停靠边的偏移（逻辑像素）；正数朝"末端"方向
  "opacity": 0.8,          // 面板不透明度，[0.5, 0.95]（bgColor 只贡献 RGB）
  "showIndicator": true,   // 是否显示运行指示器
  "autoStart": false,      // 开机自启（写 HKCU\...\Run 的 ZDock 值）
  "monitorIndex": -1,      // 显示器锚定：-1 = 跟随窗口所在显示器；>=0 = 锚第 N 台
  "items": [               // 图标列表；顺序即显示顺序
    { "path": "C:\\Windows\\explorer.exe" },
    { "path": "C:\\Windows\\System32\\notepad.exe", "name": "记事本" }
  ]
}
```

- `autoHide` / `reserveWorkArea` **默认都是关的** —— 两个都会明显改变桌面行为
  （一个让 dock 时隐时现、一个改工作区），要用户自己决定。
- 开 `reserveWorkArea` 时屏幕上会多一个 exe 同目录的 `zdock-appbar.state`
  （记抢占前的工作区，正常退出会删）。**别手删**：它是强杀后自愈的依据。

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
- Ling 静态库来源按优先级：`$LING_ROOT` → `../Ling/dist/ling-v1.3.1-x64`（发布包）→ `../Ling`（源码树）。
  当前锁 **v1.3.1**（修了 `App::appID` 的编译期常量问题）。`LING_FROM_SOURCE=1` 强制用源码树。
- 产物：`build/bin/ZDock.exe`（约 830 KB）。

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
<python> build-support/_probe_track_api.py           # 阶段三最贵假设：跟踪 API 是否可用（5 项）
<python> build-support/_probe_stage3.py              # 阶段三：跟踪/指示器/临时图标（13 项）
<python> build-support/_probe_stage4.py              # 阶段四：自动隐藏/全屏让位/AppBar/自愈（48 项）
<python> build-support/_probe_display_change.py       # 显示器/DPI 变化自适应 + 野指针健壮性（26 项）
<python> build-support/_probe_drop.py                 # 拖文件到图标（14 项）
<python> build-support/_probe_preview.py              # 悬停预览 / DWM 缩略图（13 项）
<python> build-support/_probe_layout.py               # 位置配置：4 停靠边 / 对齐 / 偏移（18 项）
<python> build-support/_probe_settings.py             # 设置窗口 / 自启 / 显示器锚定（17 项）
bash build-support/_shot_settings.py                 # 阶段六截图 → build/_review/
<python> build-support/_probe_ling_appid.sh           # 编出读 Ling::App::appID 的小工具（升级验证）
<python> build-support/_probe_ling_build.sh <名>      # 编一个"Ling 窗口"探针（如 _probe_subclass）
bash build-support/_probe_dwm_thumb.sh                # DWM 缩略图宿主可行性（纯 Win32）
bash build-support/_probe_dwm_thumb_ling.sh           # 同上，但宿主是真 Ling 窗口
<python> build-support/release.py --tag vX.Y.Z ...    # 打 GitHub Release 并上传产物
<python> build-support/_probe_config.py              # config.json 生成/读值/兜底/不覆盖（6 项）
<python> build-support/_probe_remove_item.py         # 删除与边界（含空 Dock）（5 项）
<python> build-support/_probe_ui_remove.py           # 真实 UI 走一遍"从 Dock 移除"
<python> build-support/_probe_ui_add.py              # 真实 UI 走一遍"添加程序…"
<python> build-support/_shot_stage2.py               # 阶段二验收截图
<python> build-support/_shot_stage3.py               # 阶段三验收截图
<python> build-support/_shot_stage4.py               # 阶段四验收截图
<python> build-support/_probe_hover_jitter.py        # 悬停抖动量化（阶段一 bug 2 的回归）
<python> build-support/_probe_cross_instance.py      # ZPin/ZDock 共存（阶段一 bug 1 的回归）
```

⚠ **跑阶段四探针前注意工作区可能被上次的残留占着。** 探针自己会在开跑时做环境自检
（`_probe_stage4.py` 的 `probe_shell_stale()`）并打印
`环境自检：干净工作区上注册 1px AppBar 只吃掉 Npx`。**N 明显大于 1 就说明有残留**，
这时"抬升多少像素"的判据会偏大（实测踩过：脏起点下量出 183px，看着像叠加了两份）。
探针会在 `case_appbar()` 里先扫一遍残留，正常情况下会自愈到 0~1px。

⚠ **验证原则：不抢用户的输入焦点。** 优先走两条路 ——

1. **日志取证**：关键状态变化（分组集合、指示器坐标、滑入滑出、AppBar 注册/自愈）都写日志；
2. **`PostMessage` 注入**：需要驱动鼠标事件时把 `WM_LBUTTONDOWN/UP` 投递到目标窗口，
   不移动真实光标。

只有"悬停放大"这类必须真实光标位置的场景才用 `SetCursorPos`，且用完立刻恢复。
（阶段二曾用 `SetCursorPos + mouse_event` 去点模态菜单，把用户的 WorkBuddy 焦点抢走、
对话任务被取消 —— 所以阶段三改成上面这套。）

**全屏让位**这条链路的真值来自 `GetForegroundWindow()`，要造一个真全屏前台窗口就得
`SetForegroundWindow` —— 那必然抢焦点。所以阶段四在热区窗口上留了一条**注入通道**
（`EdgeHotZone::kMsgTestInject = WM_APP + 100`）：探针 `PostMessage` 直接把"全屏状态"
喂进 `DockWin::onFullscreenChanged()`。正常运行时没有任何代码会发这条消息。

涉及真实 UI 的两个脚本（`_probe_ui_*` / `_shot_stage2.py`）仍**会模拟鼠标点击与光标移动**，
跑的时候不要去动键盘鼠标。它们全程在**隔离临时目录**里跑（复制一份 exe 过去），
不会碰你手上的 `config.json`。

诊断日志开关：`ZDOCK_VERBOSE_HIT=1`（每次命中判定，仅在结果变化或位移 >20px 时记一行）、
`ZDOCK_VERBOSE_HOVER=1`（每次 hover 变更）、`ZDOCK_VERBOSE_IND=1`（指示器定位坐标）。

## 目录

```
Src/
  main.cpp            入口：DPI 感知 → 单实例 mutex → Ling::init → 建 DockWin → 消息循环
  DockWin.h/.cpp      主窗口：布局、图标行、悬停、命中区域（region）、菜单、重建、跟踪接线、
                      自动隐藏状态机、全屏让位、AppBar 同步、显示环境变化重排、
                      拖放（子类化拿 WM_DROPFILES）、悬停预览接线
  WindowTracker.h/.cpp  事件驱动窗口跟踪：shell hook + win event，分组 / 运行状态 / 全屏判定
                       + TaskbarCreated 广播（explorer 重启自愈）
                       + WM_DISPLAYCHANGE / WM_SETTINGCHANGE 广播（显示环境变化）
                       + 测试注入通道（WM_APP+100）
  EdgeHotZone.h/.cpp  自动隐藏热区：贴屏幕底边的 3px 全透明细窗
  AppBarReserve.h/.cpp  工作区预留（AppBar）+ 强杀自愈 + 死记录清扫
  PreviewWin.h/.cpp   悬停预览气泡：DWM 缩略图做画面（独立顶层窗口）
  SettingsWin.h/.cpp  设置窗口：自绘（Button / Slider / Label），改动即时生效
  AutoStart.h/.cpp    开机自启：HKCU\...\Run 的 ZDock 值
  MonitorUtil.h/.cpp  显示器枚举：给多显示器锚定提供 rcMonitor
  Config.h/.cpp       config.json 读写（默认值 / 兜底 / 原子替换）
  SingleInstance.h/.cpp  命名 mutex 单实例
  IconNode.h/.cpp     自绘图标节点：surface 绘制 + Composition 缩放动画 + 按下 / 弹跳 / 临时态
  IndicatorNode.h/.cpp  运行指示器：独立节点画 4px 圆点（不随图标缩放）
  IconLoader.h/.cpp   图标提取：SHGetImageList(jumbo) → HICON → WIC → D2D 位图
  Log.h/.cpp          轻量日志（exe 同目录 ZDock.log）
  Res/Resource.rc     VERSIONINFO（版本号唯一来源）
build-support/
  _msvc_env.sh        cl/link/rc 的最小环境
  build.sh            构建脚本（含残留进程守卫；Ling 版本锁定在这里）
  zdock_stage1_test.py, _probe_*.py, _shot_stage4.py   验证 / 诊断 / 截图
  _probe_ling_appid.sh / .cpp    读 Ling::App::appID 的小工具（升级验证用）
_review/              **交付产物**（本地，被 gitignore）—— 带版本号的 exe
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

### 窗口跟踪：**主用 shell hook，win event 只补三个**（绝不轮询）

两条路都实测过（`build-support/_probe_track_api.py`）。同一个 3 秒窗口内起一个应用：

| 事件源 | 事件量 |
|---|---|
| `SetWinEventHook`（CREATE / SHOW / NAMECHANGE 全订） | 260 / 249 / 252 次 |
| `RegisterShellHookWindow` | **个位数** |

所以主通道是 shell hook；`SetWinEventHook` 只订 `EVENT_SYSTEM_MINIMIZESTART/END`
与 `EVENT_OBJECT_NAMECHANGE` 这三个 shell hook 拿不到的。**订阅面窄是关键**，
全订等于给自己造了个高频回调（变相轮询，违反红线 4）。

两个容易记反/搞错的点：

- shell hook 的参数是 **`wParam` = 事件码（`HSHELL_*`）、`lParam` = HWND**，不是反过来；
- 消息号来自 `RegisterWindowMessageW("SHELLHOOK")`（本机实测 `0xC028`），
  **不是**固定的 `WM_*`。按"除 WM_CREATE 外全部"来收会把无关消息混进来。

跟踪器自己建一个 0x0 的隐藏窗口收消息 —— Ling 的 `WinBase::winProc` 是静态私有的、
不暴露消息口，借不了 dock 主窗口。

过滤规则（任务书 §9.4）：可见顶层窗口 + 无 owner + 非 `WS_EX_TOOLWINDOW`
+ 非 DWM cloaked（UWP 切到别的虚拟桌面时 `IsWindowVisible` 仍为 TRUE，只能靠
`DwmGetWindowAttribute(DWMWA_CLOAKED)` 排除）。分组键**双轨**：能拿到 AUMID 就用
AUMID，否则用小写 exe 路径 —— UWP 没有传统 exe 路径，只靠路径会把它们全并成一个。
`ApplicationFrameWindow` **不要无脑排除**（某 Dock 因此完全不可用），要下钻取子窗口。

### 结构增删**就地做，绝不借道 `rebuild()`**

第一版 `syncWithTracker()` 发现"临时图标要增删"时直接调 `rebuild()`，
而 `rebuild()` 结尾又调回 `syncWithTracker()` —— **无限循环**（日志里刷了 23 次
"重建完成"）。而且 `rebuild()` 内部会 `items.clear()` 再只恢复固定项，
所以第二次进来临时项"又不见了"，`needRebuild` 永远为真。

现在：`row->makeChild<IconNode>()` / `removeChild()` 就地增删 →
`relayoutForItemCount()` 重算面板宽度与窗口尺寸（**不销毁节点**）。

**这类互调很难在写的时候看出来**，因为两处改动可能隔了几百行。
通用规则：一个"重建"函数如果在结尾调用某个"同步"函数，那"同步"函数里就绝不能再调"重建"。

### 指示器定位：**坐标系 + 单位，同一段代码两个坑**

`IndicatorNode` 必须独立（不能画在 `IconNode` 的 surface 上 —— 那张 surface 会随
悬停放大动画一起缩放，而指示器要恒定 4px）。摆位置时连撞两个坑，**都不报错、不崩溃**：

1. **坐标系**：Ling 的 `Node::x/y` 是**绝对坐标**（相对窗口客户区），不是相对父节点。
   实测 `row->x == row 里第一个 IconNode->x == 77`。叠加父节点偏移会整体偏出去。
2. **单位**：`node->x/y/w/h` 读出来是**物理像素**（yoga 输出已乘 dpi），
   而 `setPosition` / `setSize` 内部**会再乘一次 dpi**。
   第一版把物理值直接喂进去，位置被放大 1.24 倍跑到窗口外 —— 透明合成窗口里
   完全没有视觉反馈，看着就像"没画出来"。

```cpp
const float d = (dpi > 0.f) ? dpi : 1.f;
const float cxLog = (item.node->x + item.node->w * 0.5f) / d;   // 物理 → 逻辑
const float yLog  = (item.node->y + item.node->h) / d + kIndicatorGap;
item.indicator->setPosition(Ling::Edge::Left, cxLog - kIndicatorDia * 0.5f);
item.indicator->setPosition(Ling::Edge::Top,  yLog);
```

Ling 的坐标/单位约定（实测，文档没写全）：

| 接口 | 期望单位 |
|---|---|
| `Node::x/y/w/h`（读） | **物理像素** |
| `Node::setPosition` / `setSize` | **逻辑像素**（内部 ×dpi） |
| `WinBase::setSize`（窗口级） | **逻辑像素**（内部 ×dpi） |
| `WinBase::setPosition`（窗口级） | **物理像素**（不乘） |

⚠ 后两行 2026-09-27 实测更正：`WinBase::setSize` 的源码是 `this->w = w * dpi`（**乘**），
`setPosition` 直接 `SetWindowPos`（**不乘**）。早先这张表把两者写成一样，是错的 ——
按错的那版算窗口尺寸会差一个 dpi 倍。

排查手段：`ZDOCK_VERBOSE_IND=1` 让 `placeIndicators()` 把**算出来的坐标**和**面板坐标**
一起打日志，一眼核对"指示器 y 是否 < 面板底边"。**算得对但画不对 → 怀疑单位/坐标系；
算出来就超界 → 算法问题**，这一步区分能省掉大量瞎猜。

### 阶段四的两个坑：**定位基准自引用** + **shell 的死 AppBar 记录**

两个坑症状一样（工作区被占两道，95px 变 183px），根因完全不同，必须分开看。

**坑 1：dock 定位不能用工作区当基准（自引用反馈回路）**

`dockRectShown()` 最初写的是"底边 = `SPI_GETWORKAREA` 的底边 − bottomMargin"。
而 AppBar 预留**改的就是工作区** —— 于是变成自引用：工作区被抬一次，
下次算出来的 dock 位置就跟着上爬一次。日志里能看到批准矩形一路往上漂。

改成锚**监视器的 `rcMonitor`** 之后位置恒定，AppBar 只负责把工作区让出 95px。
**`hotZoneRect()`（热区位置）和滑出目标也必须一起改**，否则热区会跟着往上爬、
最后鼠标够不到。

**坑 2：强杀留下的死 AppBar 记录会叠加**

AppBar 的注销（`ABM_REMOVE`）是应用自己的责任，shell **不检查**宿主窗口是否还活着。
`taskkill /F` 之后工作区就一直保持缩进（实测等 15 秒也不恢复）。
而且这条死记录会**叠加** —— 下次启动读到的"抢占前的值"本身就已经被占着了：

```
已记录抢占前工作区 (0,38)-(2560,1440)  → 已注册 → 工作区底 1345   ← 正常，占一份
[强杀]
已记录抢占前工作区 (0,38)-(2560,1345)  → 已注册 → 工作区底 1257   ← 叠加，占两份
```

**它在哪里**：`Shell_TrayWnd` 进程的内存态，注册表里没有 AppBar 记录表，
所以外部没法直接改。

**怎么清**（实测）：下一次 **`ABM_NEW → ABM_SETPOS(空矩形) → ABM_REMOVE` 完整循环**
会让 shell 重扫一遍、把死记录丢掉。而只做 `NEW + SETPOS(真位置)`（= 正常注册路径）
**清不掉**。所以 `AppBarReserve::purgeStaleRecord()` 专门跑一遍完整循环。

**自愈的完整顺序**（`recoverStaleWorkArea`，两步都不能省）：

1. 用状态文件里备份的值 `SPI_SETWORKAREA` 写回干净工作区；
2. `purgeStaleRecord()` 把 shell 里的死记录扫掉；
3. 之后才正常注册 —— 功能不降级，工作区只占一份。

### 显示环境变化：**Ling 的回调顺序**决定必须延后一拍

`WM_DPICHANGED` 的处理链在 Ling 里是：

```cpp
// Ling/src/WinBase.cpp
dpi = newDPI / 96.f;
body->applyDpiChange();
onDpiChanged();                       // ← 我们的回调在这里
RECT* prcNewWindow = (RECT*)lParam;
SetWindowPos(hwnd, nullptr, prcNewWindow->left, prcNewWindow->top, ...);  // ← 之后立刻覆盖
```

那个"系统建议矩形"是按"窗口原物理尺寸等比缩放"给的，对**内容自适应**的 dock 是错的
（位置会被推到屏幕中间）。所以 ZDock 在 `onDpiChanged` 里**不直接重排**，
只挂一个 1ms 的一次性定时器（`kTimerRelayout`）——让重排落在那条 `SetWindowPos` 之后。
（一次性定时器不是轮询，红线 4 允许。）

另外三点：

1. **必须重算尺寸**，不能只改位置 —— `px(logical) = logical * dpi`，dpi 变了窗口物理尺寸
   也要跟着变，否则 200% 缩放下图标只占一半大小。所以重排直接复用 `relayoutForItemCount()`。
2. **隐藏态要保住** —— 重排内部是 `applyDockPlacement(0)`（展开态），
   如果此刻 dock 正因自动隐藏/全屏让位滑出屏幕，必须再推回屏幕外，
   否则它会在全屏应用底下突然冒出来。
3. **`WM_SETTINGCHANGE` 必须按 `lParam` 过滤** —— 它是"任何设置变化都会广播"的高频消息，
   不过滤等于给自己造了个高频回调。只认 `WorkArea` / `WindowMetrics` / `Display`。
   同时它和 `WM_DPICHANGED` 的 `lParam` **都是指针**，而这是广播消息、谁都能往我们窗口投 ——
   跨进程投进来的指针指向对方地址空间，解引用就是读野指针，所以先 `IsBadStringPtrW` 再读。

### 悬停预览：**DWM 缩略图**做画面，不截屏

任务书 §9.7 要求"首选 `DwmRegisterThumbnail`（DWM 直接合成目标窗口的实时画面，
零截图成本）… 刷新由事件驱动，**不要定时截屏**"。所以画面来源就用它，
`PreviewWin` 只负责给它一块地方。

⚠ 有三个坑：

1. **宿主形态必须先实测，而且判据不能只看返回值。**
   `DwmRegisterThumbnail` 对普通窗口 / `WS_EX_NOREDIRECTIONBITMAP` / `WS_EX_LAYERED`
   **都返回 S_OK** —— 但返回值不代表画面真的合成上来了。可靠判据是
   **拿 `PrintWindow` 拍宿主、数源窗口的颜色像素**（`_probe_dwm_thumb*.cpp` 就是这么做的）。
   好消息：真 Ling 窗口（DirectComposition + `WS_EX_NOREDIRECTIONBITMAP`）实测
   **可以做宿主**，任务书担心的限制不成立。
2. **DWM 会把画面拉伸到 `rcDestination`，不管纵横比。** 所以目标矩形要自己按
   源窗口比例算好再填（否则 16:9 的视频会被压成方的）。
   同时用 `DWM_TNP_SOURCECLIENTAREAONLY` 只取客户区，别把标题栏和边框也缩进来。
3. **最小化窗口拿不到画面。** 挑目标窗口时优先"可见且未最小化"的那个，
   挑不出来才退回第一个（那时画面是空的，但位置和标题仍然对）。

另外 `WinBase::~WinBase()` **不是虚函数** → `PreviewWin` 只能值语义持有
（`DockWin` 的成员），绝不能通过 `WinBase*` 删除，否则析构不跑、缩略图句柄泄漏。

### 拖放：**子类化** Ling 窗口拿 `WM_DROPFILES`

Ling 的 `WinBase` 没有通用消息钩子，而拖放是"消息进窗口过程"才拿得到的东西。
三条路里选了**子类化**（另两条：改 Ling 加钩子要动别人的库；用覆盖窗口收拖放会
挡住 dock 自己的 hover 命中）：

```cpp
s_self = this;   // ⚠ 用自己的静态指针取 this
origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&DockWin::subclassProc);
DragAcceptFiles(hwnd, TRUE);

LRESULT CALLBACK DockWin::subclassProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DROPFILES) { s_self->onDropFiles((HDROP)wp); return 0; }
    return CallWindowProcW(s_self->origWndProc, h, m, wp, lp);   // 其余一律转回 Ling
}
```

⚠ **取 this 不要读 `GWLP_USERDATA`** —— 那里是 Ling 自己的 `WinBase*`。
`_probe_subclass.cpp` 实测确认过它没被我们改坏，但拿它向下转型属于依赖 Ling 内部布局。

⚠ 子类化是"**包一层**"不是"换一个"：其余消息必须 `CallWindowProcW` 转回原过程，
否则 dock 会整个失去输入与绘制处理。

⚠ 测试这类功能不必真拖鼠标：`WM_DROPFILES` 的 `HDROP` 本质上就是一块
`DROPFILES` 结构 + 双 `\0` 结尾的文件名列表，自己 `GlobalAlloc` 造一个再
`PostMessage` 就行（`_probe_drop.py` 就是这么做到全自动的）。

### ⚠ 验收截图曾经**颜色全是反的**（工具的错，不是程序的）

`write_png()` 拿到的是 Windows DIB 的 **BGRA**，却直接按 PNG 的 **RGBA** 写
（IHDR 里声明的颜色类型就是 6 = RGBA）——**R 和 B 从头到尾在对方的槽里**。
这个 bug 从阶段一的截图脚本就在，一路带到阶段五。

症状极具误导性：设置窗口里 `#1F5C7A`（深青）的按钮在截图里是**金褐色** `#7A5C1F`，
第一反应必然是"Ling 把颜色通道搞反了"。而直接读窗口像素是 `R=31 G=92 B=122` ——
**完全正确**。绕了一圈才发现是截图在骗人。

**教训**：验收用的工具本身也要能自证。判断"颜色对不对"这种问题，
**直接读像素**比看截图可靠 —— 截图多过一道编码，就多一个出错的地方
（和"验证要看最终效果、别只看中间信号"是同一条，只不过这次是反过来：
**中间信号（截图）错了，最终效果（像素）才是对的**）。

### 设置窗口：改动即时生效走的是**轻量重排**

`applyLiveConfig()` —— 只重排、**不重建图标节点**。别拿 `reloadConfig()` 干这事：
它会 `rebuild()` 把每个图标节点删掉重建，拖滑块时每动一下就重建一次，既卡又闪。

窗口可拖动靠 `onHitTest` 在标题条区域返回 `HTCAPTION`（自绘窗口拖着走的标准做法），
但**关闭按钮那一小块要排除在外**，否则它永远是标题栏、点不到。

⚠ `choiceRow` 里踩过一个坑：把 `initializer_list` 参数**按值捕获**进
延迟执行的刷新回调里 = 抓了一把悬空指针（`initializer_list` 只在当前语句的临时数组活着）。
要先把选项抄进 `std::vector` 再捕获。

### 开机自启：两个容易静默失败的点

1. **值必须带引号**。路径带空格时不加引号，Windows 会把空格前的部分当程序名，
   开机**静默启动失败** —— 用户只会觉得"自启没生效"，不会看到任何报错。
   统一加引号（不带空格时也无害）。
2. **`GetModuleFileNameW` 的 MAX_PATH 截断**。长路径下它会截断并返回 MAX_PATH，
   拿到的路径末尾缺一截、写进注册表就再也启动不起来。要"返回长度顶到缓冲区上限就翻倍重试"。

设置窗口里那一项的 getter 读**注册表真值**（`autostart::isEnabled()`）而不是配置字段 ——
用户可能在任务管理器里关过自启，那才是事实；setter 两个都写，保证配置那份不漂。

## 已知限制 / 下一步

- **滚轮调大小没做**（任务书 §2 #24）—— 用户明确说不需要这一项。
- **多显示器锚定只在一台显示器上验过**：本机只有一台屏，所以探针验的是
  "`monitorIndex=0` 与 `-1` 等价"和"越界退回主屏"。真正插两块屏、改混合 DPI 的效果
  还需要实机确认（`MonitorUtil` 用的是 `rcMonitor`，理论上混合 DPI 下位置不会偏，
  但**没实测过**）。设置窗口里显示器选项在多屏时才列出来。
- **多窗口预览还是"取第一个窗口"**，没做成任务书 §2 #17 / #16 说的**列表**
  （每窗口一张缩略图 + 标题）。单窗口预览、画面来源、位置都通了，列表是排列与
  多缩略图管理的事，下一步补。
- 悬停**名称气泡**（纯文字标签）未做 —— 预览气泡已经覆盖了"看这是哪个应用"的需求，
  但任务书 §3 的标签规格还没实现。
- 配置**改动后需要手动"重新载入配置"**（或重启）才生效；文件变更监听（`ReadDirectoryChangesW`）未做。
- **拖放不支持目录**（拖文件夹进来会被跳过并记日志）：语义不明确 —— 是复制整个目录树，
  还是只当路径参数？这要按 §1 原则裁量后再做。
- 拖放用的是 `WM_DROPFILES` 而不是 OLE `IDropTarget`，所以**拿不到拖放过程事件**
  （DragEnter/DragOver/DragLeave），也就没法"按图标区分光标形状"
  （拖到程序图标 vs 文件夹图标，现在都是同一个光标）。
- 菜单用的是系统默认 UI 字体，与面板自绘风格不完全统一。
  自绘菜单需要 owner-draw，而 `WM_DRAWITEM` 要发给 owner 窗口 ——
  Ling 的 `WinBase` 不暴露消息口，得先有个能收消息的中间窗口（跟踪器的接收窗口可以，
  但要把它变成菜单 owner），属"观感增强"而非缺陷，暂缓。
- **UWP / 打包应用**：跟踪与分组已支持（走 AUMID），但**启动**还不行 ——
  临时图标没有传统 exe 路径，"固定到 Dock"、点击启动都会拒绝并记一行日志，
  需要走 `shell:AppsFolder\<AUMID>` 才行（属后续阶段）。
- **AppBar 残留的系统限制**（实测，非本程序 bug）：AppBar 的注销是应用自己的责任，
  进程被 `taskkill /F` 后 shell 会一直保留那条记录。ZDock 的自愈能把它扫掉，
  但**如果 ZDock 从此再没启动过**，工作区就会一直缩进着。
  这时手动跑一次 ZDock 或用 `SPI_SETWORKAREA` 复位即可（重启 explorer 也能清）。
- AppBar **会与系统任务栏抢边缘空间**：任务栏在底部时两者会互相挤压，
  所以默认关闭；开着的时候建议把任务栏固定在别的边。
- **DPI 变化的自动化验证覆盖不到真实触发**：`WM_DPICHANGED` 的 `lParam` 是指针，
  跨进程投递无效，而自动化改系统缩放比不现实。当前靠
  "订阅日志存在 + `WM_DISPLAYCHANGE` 走同一条重排路径"间接覆盖。
- 忽略键盘与无障碍。

> 已修（v0.1.4）：~~显示器变化（`WM_DISPLAYCHANGE` / DPI）没有处理~~ →
> 已有 `onDisplayChanged` + `onDpiChanged` 双来源重排；
> ~~全屏让位只在自动隐藏开启时生效~~ → 两者已解耦。
> 已完成（v0.1.5）：~~拖放排序 / 拖放添加未做~~ → 拖文件打开已做。
