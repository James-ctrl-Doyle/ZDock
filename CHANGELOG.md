# CHANGELOG

## 0.1.8 · 七条体验修正 — 2026-09-28

一轮集中的体验修正，七条需求（「滚轮调大小」按用户要求不做）。

### 1. 修：自动隐藏开着时"唤不出来"

用户反馈：开了自动隐藏后鼠标碰屏幕边缘叫不出 dock。

根因是**判定口径不一致**：热区是**另一个窗口**（贴边的 3px 细窗），从热区唤起时
光标还压在热区上、并不在 dock 窗口上 —— 而 `cursorOverDock()` 只看
`WindowFromPoint(...) == hwnd`。于是 `tickSlide()` 结尾那次"该不该收"的判定
认为"鼠标不在 dock 上"，dock **刚滑进来又缩回去**。

修法：`cursorOverDock()` 把"光标停在停靠边的热区里"也算成"贴着 dock"。

### 2. exe 属性里的「文件说明」改为 `ZDock`

原来是一长串 `ZDock - Windows desktop dock`。

### 3. 右键图标时同时给出「程序」和「dock」两级菜单

用户反馈：dock 级命令（设置 / 添加程序 / 重新载入）原来只能右键**图标之间的缝隙**
才出得来，那个可点范围只有几像素、很难点中。

现在右键任意图标都会出**一整张菜单**，上下两段用分隔符隔开：
上半段是这个程序的（打开 / 管理员打开 / 移除），下半段是 dock 的
（设置… / 添加程序… / 重新载入配置）。

⚠ **"退出 ZDock"没有放进去** —— 混在"移除 / 关闭全部窗口"旁边太容易误点，
而误点的代价是关掉整个 dock。它仍然只在空白处菜单里。

### 4. 删掉悬停预览

`PreviewWin.*` 整个删除（连探针 `_probe_preview.py`）。悬停放大 / 标签不受影响。

### 5. 默认配置只留两枚图标：资源管理器（最左）+ 回收站（最右）

原来默认塞了 6 个系统程序（记事本 / 画图 / cmd / PowerShell / 截图 / 计算器），
用户嫌多。现在默认就两项，**中间那段留给正在运行的应用**。

配套改了两件事：

**① 图标行拆成三段容器**：左固定项 | 临时项 | 右固定项。

⚠ 为什么要拆：Ling 的 `Node::setChild()` **只能 append**
（`YGNodeInsertChild(..., YGNodeGetChildCount)`），而 `children` 是 protected 的 ——
外面没法把"后面才出现的临时图标"插到"右侧固定项"前面去。
三个容器各自按沿边方向定位，就能表达"资源管理器 | 运行中的应用 | 回收站"。

配置里加 `items[].pinRight`（默认 false）：`true` 的项排到临时图标右边。
段内"最后一个不加间距"要按**本段**判断（`isLastInSegment`），段间距由
`applyNodeLayout()` 在三段之间补。

**② 回收站要用 shell 虚拟对象那一套**。它没有 exe 路径，配置里写的是
**CLSID**：`"::{645FF040-5081-101B-9F08-00AA002F954E}"`。

⚠ 实测 `SHGetFileInfoW(L"::{...}", ...)` **三种 flags 全返回 0** —— 它不认裸 CLSID 字符串。
必须先 `SHParseDisplayName` 解析出 **PIDL**，再带 `SHGFI_PIDL` 调。
打开同理：走 `ShellExecuteExW` + `SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST`，
不能拿字符串直接 `ShellExecuteW`。菜单里也**不给虚拟对象**"以管理员身份打开"。

### 6. 修：「全屏时让位」打开后没效果

用户反馈这条没看出效果。根因：**把当前窗口全屏化**（F11 / 双击标题栏 /
播放器全屏按钮）**并不会换前台窗口**，shell 也就不发 `HSHELL_WINDOWACTIVATED`——
而全屏判定只挂在那条事件上，于是状态一直不更新、dock 不让位。

修法：加 `EVENT_OBJECT_LOCATIONCHANGE` 钩子 —— 前台窗口一变形就重判一次全屏。

⚠ 这是**事件不是轮询**（红线 4 允许 `SetWinEventHook`），但它很频繁：加了之后
**空闲 CPU 从 0.000% 涨到 1.094%**，正好顶穿阶段一那条 1% 红线。
所以回调里**节流 100ms**（"全屏了没有"是个人类尺度的事），加完回到 **0.156%**。

### 7. 设置窗口里的数值改成可键盘输入

原来数值是只读标签，拖滑块只能拖个大概。现在每个数值位都是一个 `TextBox`，
和滑块**双向联动**：

- 拖滑块 → 输入框跟着变
- 输完**失焦**（点别处或按回车）→ 解析、**夹紧到范围**、应用、规范化回写
- 精度跟着步长走：`step >= 1` 的项（图标大小 / 间距 / 圆角 / 偏移）**只收整数**，
  小数点直接丢掉；`step < 1` 的项（悬停放大 / 不透明度）才收小数

回车确认靠 `WinBase::onKeyDown` 把当前聚焦的输入框 `blur()` 一下
（TextBox 自己会把回车当换行插进去，但解析时忽略非数字字符，没有副作用）。

### 测试

- 新增 `build-support/_shot_default.py`（默认配置截图 + 回收站图标提取验证）
- **修探针**：阶段一原来在拍"空闲参考图"**之前没有移开光标** ——
  如果上一轮（或别的脚本）把光标留在了 dock 上，那张"空闲图"其实已经带着放大，
  跟悬停图一比就是 0 差异像素、误报成"放大动画没在画"。现在先移开再拍。
- 删除 `_probe_preview.py`（预览功能已去掉）

回归全绿：阶段一 **24/24**、阶段三 13/13、阶段四 **48/48**、显示环境 **26/26**、
拖放 **14/14**、位置配置 **18/18**、设置/自启 **17/17**。

版本号 0.1.7.0 → 0.1.8.0。

## 0.1.7 · 阶段六（二/三）：设置窗口 + 开机自启 + 显示器锚定 — 2026-09-28

阶段六剩下的三块交付了：**自绘设置窗口**、**开机自启**、**多显示器锚定**
（滚轮调大小按用户要求不做）。加上 0.1.6 的位置配置，阶段六完成。

### 新增：设置窗口（任务书 §2 #25）

右键 dock 空白处 →「设置…」。**自绘**（Ling 的 Button / Slider / Label 拼出来），
不用系统控件 —— 这个程序整体是自绘风格，塞一列 Win32 原生控件观感会裂开。

四个分组、13 个可调项：

| 分组 | 项 |
|---|---|
| 外观 | 图标大小 / 图标间距 / 悬停放大 / 面板不透明度 / 圆角 / 运行指示器 |
| 位置 | 停靠边（下/上/左/右）· 对齐（起始/居中/末端）· 沿边偏移 · 显示器 |
| 行为 | 自动隐藏 / 全屏时让位 / 预留工作区 |
| 启动 | 开机自启 |

- **改动即时生效**：拖滑块当场看到 dock 变，不用点保存。
- 关闭窗口时写回 `config.json`。
- 有「恢复默认外观」按钮（回不到"取消"，但能一键回默认）。
- 窗口可拖动：`onHitTest` 在标题条区域返回 `HTCAPTION`（自绘窗口拖着走的正解），
  关闭按钮那小块排除在外，否则点不到。

⚠ 走的是**新加的轻量入口 `applyLiveConfig()`**，不是 `reloadConfig()` ——
后者会 `rebuild()` 把图标节点全删了重建，拖滑块时每动一下就重建一次，既卡又闪。

### 新增：开机自启（任务书 §2 #26）

`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `ZDock` 值。

- 不需要管理员权限；关掉自启就是删一个值，不用去启动文件夹里找残留 .lnk。
- ⚠ 值必须是 **exe 完整路径**，且**带引号** —— 不带的话 Windows 会把空格前的部分
  当程序名，开机静默启动失败（用户只会觉得"自启没生效"）。
- 启动时按配置 `sync()` 一次（用户可能手改过 config.json，或在任务管理器里关过）。
- 设置窗口里那一项的 getter 读的是**注册表真值**（`autostart::isEnabled()`）而不是
  配置字段 —— 用户在别处关过的话，那才是事实。

### 新增：多显示器锚定（任务书 §2 #31）

新增 `Src/MonitorUtil.*`（`EnumDisplayMonitors` + `MONITORINFOEXW`）与 `monitorIndex` 配置：
`-1`（默认）= 跟随窗口当前所在显示器（= 加这个配置项之前的老行为），
`>=0` = 锚到第 N 台（**越界自动回主屏**，比如拔掉了一块屏）。

⚠ 定位基准一律用 `rcMonitor`，不是 `rcWork` —— 后者会被 AppBar 预留改掉，
拿它当基准就是自引用（dock 改工作区、工作区又反过来影响 dock 位置）。

### 修：验收截图的颜色一直是 R/B 互换的

`write_png()` 把 Windows DIB 的 **BGRA** 数据直接当成 PNG 的 **RGBA** 写
（IHDR 声明的颜色类型就是 6 = RGBA），所以**所有验收截图**里 R 和 B 都在对方的槽里。

症状很有误导性：设置窗口里 `#1F5C7A`（深青）的按钮**在截图里显示成金褐色** `#7A5C1F`，
看着特别像"Ling 把颜色通道搞反了"。实际直接读窗口像素是 `R=31 G=92 B=122` —— 完全正确，
**是截图骗人**。这个 bug 从阶段一的截图脚本就在了。

顺带做的两处小修（都是设置窗口第一版截图暴露的）：
数值统一用 `{:.0f}` 会把"悬停放大 1.70"显示成"2"、"不透明度 0.80"显示成"1"
（改成精度跟着步长走）；窗口高度 700 不够，最后一行"开机自启"被底部按钮压住（→ 790）。

### 测试

新增 `build-support/_probe_settings.py`（**17/17**）：

| 覆盖 | 结果 |
|---|---|
| 自启 | `autoStart=true` → 注册表出现 `ZDock`，值 = **带引号的 exe 完整路径**；改回 false → 值被删 |
| 设置窗口 | 注入 `WM_APP+101` 后出现 `ZDockSettings` 窗口、尺寸符合、**渲染出 5 万+ 非背景像素** |
| 多显示器 | `monitorIndex=0` 与 `-1` 位置一致；`=5`（越界）退回主屏、不崩 |

⚠ 自启那条会**真的读写用户的注册表 Run 项**，所以探针开头备份、`finally` 里恢复
（实测跑完恢复成"（删掉）"，没动用户的开机自启）。

⚠ 设置窗口是用**消息注入**打开的（`DockWin::kMsgOpenSettings = WM_APP+101`）——
探针点不到菜单，那要先 `SetCursorPos` 把光标挪到 dock 上再模拟点击、会抢用户的输入焦点。
和全屏注入（`WM_APP+100`）是同一套路。

新增 `build-support/_shot_settings.py` 截图（**复用** `_shot_stage4` 的截图实现，
同一种截图只留一份）：`stage6_1_设置窗口.png` / `stage6_2_停靠底部.png` /
`stage6_3_停靠左边.png` / `stage6_4_停靠上边.png`。

回归全绿：阶段一 **24/24**、阶段三 13/13、阶段四 **48/48**、显示环境 **26/26**、
拖放 **14/14**、预览 **13/13**、位置配置 **18/18**、设置 17/17。

版本号 0.1.6.0 → 0.1.7.0。

## 0.1.6 · 阶段六（一）：位置配置 + 两处修复 — 2026-09-27

Dock 不再只会贴在屏幕底边：**四个停靠边 + 三种对齐 + 沿边偏移**都可以在
`config.json` 里配，改完「重新载入配置」即时生效。

> ⚠ 阶段六是三件事（位置 / 大小配置、设置窗口、多显示器锚定），本版交付**第一件**。
> 设置窗口、滚轮调大小、开机自启、多显示器锚定尚未做，见 README「已知限制」。

### 新增：停靠边 / 对齐 / 沿边偏移（任务书 §2 #22）

新增三个配置项 + 两个外观项：

```jsonc
"dockEdge": "bottom",   // bottom(默认) / top / left / right
"dockAlign": "center",  // start / center(默认) / end
"dockOffset": 0,        // 沿停靠边的偏移（逻辑像素），正数朝"末端"
"opacity": 0.8,         // 面板不透明度 0.5~0.95（任务书 §3）
"showIndicator": true   // 是否显示运行指示器
```

四个默认值都对应"和 0.1.5 完全一样的行为"，所以老配置升级后观感不变。

**核心是"换轴"**。原来所有几何都写死了"贴底边 + 水平居中"，
现在统一收敛成两条轴：

- **沿边方向**（bottom/top 是 X，left/right 是 Y）——决定对齐与偏移；
- **厚度方向** ——决定面板厚度、halo 留在哪一侧、指示器画在哪。

配套改动（都收口成函数，不再各处硬编码）：

| 改动 | 说明 |
|---|---|
| `panelAlong()` / `panelAcross()` | 面板的长与厚；`panelW()/panelH()` 按停靠边**自动换轴** |
| `panelOrigin()` | 面板在窗口内的偏移 —— halo 留在"远离屏幕边"那一侧 |
| `applyNodeLayout()` | 面板/图标行的**位置、尺寸、排列方向**（横向边 `Row`、纵向边 `Column`）收口到一处 |
| `slideDir()` / `slideDistance()` / `curSlideOffset()` | 滑动从"只动 Y"改成"沿滑出方向的位移"，四个边共用一套补间 |
| `hotZoneRect()` | 热区贴对应的屏幕边；左右停靠时变成**竖条**；沿边范围跟着**面板**中心走（原来固定屏幕中心，换对齐后就对不上了） |
| `placeIndicators()` | 指示器统一画在"面板厚度方向的末端"，四个边共用一套留白逻辑 |
| 预览锚点 | 预览浮在"朝屏幕中心"那一侧（bottom→上方 / top→下方 / left→右侧 / right→左侧） |

`PreviewWin::showFor()` 的接口也跟着改了：不再收"锚点"、改收**目标左上角** ——
"该浮在哪一侧"是停靠边的知识，不该塞进预览窗口。

### 修：两个 bug

**① `#RRGGBB` 六位色解析错位**（`Config::parseColorImpl`）

```cpp
if (s.size() == 7) return v | 0xFF000000u;   // ✗ alpha 被塞进了 R 的位置
```

`Ling::Color(uint32)` 的字节序是 **0xRRGGBBAA**（alpha 在低字节），
所以 `#1A1A1A` 会被解析成"红色 + 几乎全透明"，而不是"不透明的深灰"。
（九位色 `#RRGGBBAA` 走另一条分支，所以一直没暴露。）
顺带把 `Config.h` 里写反的注释改对 —— 它一直写着"0xAARRGGBB"。

**② 悬停扫视时预览反复弹出/切换**

预览的 300ms 延迟定时器原本**只在"首次进入图标"时启动**，于是扫视时
（hover 一直在某个图标上）定时器不会重置 → 每 300ms 到期一次 →
预览反复弹出、还反复换目标（每次换目标都要 DWM 注册/注销缩略图）。

实测代价：阶段一的"悬停扫动 CPU"从 **1.0% 涨到 5.7%**（超了它的 5% 红线）。
改成**每次悬停变化都重置延迟**之后回到 **2.1%**，语义也更对 ——
**停住不动 300ms 才弹**。

### 测试

新增 `build-support/_probe_layout.py`（**18/18**）：

| 覆盖 | 结果 |
|---|---|
| 四个停靠边各一条 | 面板贴对屏幕边；左右停靠时**换轴**（面板变竖的，88x317） |
| 对齐 start / center / end | 分别在 bottom 边上量面板的边界 |
| `dockOffset: 120` | 面板沿边精确推移 120 逻辑像素 |
| `opacity` | 写进 config 后被正确载入（日志可查） |
| 热区换轴 | left 停靠时热区变成 **3x720 的竖条**、贴屏幕左边 |

⚠ 探针第一版**按 config 里的项数算面板宽**，结果"居中/端对齐"全判成假失败：
这台机器上 WorkBuddy / Chrome / Steam / 游戏一堆在跑，dock 会被补出**临时图标**，
项数从 4 涨到八九项。改成**从日志读实际项数**（`Dock 共 N 项`）之后才对上。

回归全绿：阶段一 **24/24**（CPU 那两条一度掉到 22/24，就是上面第 ② 个 bug）、
阶段三 13/13、阶段四 **48/48**、显示环境 **26/26**、拖放 **14/14**、预览 **13/13**。

版本号 0.1.5.0 → 0.1.6.0。

## 0.1.5 · 阶段五（悬停预览 / 拖文件打开）— 2026-09-27

让 Dock 能"看见"窗口、也能"接住"文件：鼠标停在图标上弹出该应用的**实时画面**，
把文件拖到图标上用对应程序打开（拖到文件夹图标则复制进去）。

### 先验的两个最贵假设（任务书 §14.2 要求"先最小 demo 实测再集成"）

**① Ling 窗口能不能当 DWM 缩略图宿主？** —— 能。

任务书 §9.7 提醒"宿主窗口形态可能限制缩略图可用性（例如 layered 窗口做不了宿主）"，
而 ZDock 主窗口是 DirectComposition 栈、带 `WS_EX_NOREDIRECTIONBITMAP`
（字面意思就是"没有重定向表面"），所以这条**必须实测**。
两个 demo 一起给出了答案（`build-support/_probe_dwm_thumb.cpp` + `_probe_dwm_thumb_ling.cpp`）：

| 宿主形态 | `DwmRegisterThumbnail` | 画面是否合成进来 |
|---|---|---|
| 普通窗口 | S_OK | **是**（品红像素 19200/19200） |
| `WS_EX_NOREDIRECTIONBITMAP` | S_OK | **是** |
| `WS_EX_LAYERED` | S_OK | **是** |
| **真 Ling 窗口**（NOREDIRECTIONBITMAP、非 layered） | S_OK | **是** |

结论：三种形态都行，**任务书担心的限制在 Ling 窗口上不成立**，DWM 缩略图路线可用。
（判据不能只看 `DwmRegisterThumbnail` 的返回值 —— 它对三种都返回 S_OK；
必须**数宿主截图里的源窗口像素**才知道画面有没有真的上来。）

**② 能不能从外部"子类化" Ling 窗口拿消息？** —— 能。

Ling 的 `WinBase` 只暴露 `onCreated` / `onHitTest` / `onMinMaxInfo` / `setCursor` / `layout`
和一组 winrt 事件，**没有通用消息钩子**，也不处理 `WM_DROPFILES` ——
而拖放恰恰是"消息进窗口过程"才拿得到的东西。三条路里：

- 改 Ling 加钩子 → 要动别人的库、发新版本
- 用覆盖窗口收拖放 → 会挡住 dock 自己的 hover 命中，功能打架
- **子类化 Ling 窗口** → 纯 ZDock 侧、零 Ling 改动 ← 选它

`_probe_subclass.cpp` 实测：能收到投给该窗口的消息、
**Ling 存在 `GWLP_USERDATA` 里的 self 指针不受影响**、原 wndProc 转发链正常、可还原。

### 新增：悬停预览（`Src/PreviewWin.h/.cpp`）

- 鼠标在图标上停留 **300ms** → 弹出预览气泡，显示该应用窗口的**实时画面**；
  鼠标离开图标即收起。延迟用一次性定时器实现（红线 4 的例外条款允许"自身 UI 状态"短定时器）。
- 画面来源 = **DWM 缩略图**，DWM 直接合成目标窗口 —— **零截图成本**，
  也不用定时刷新（任务书 §9.7："刷新由事件驱动，不要定时截屏"）。
- 预览是**独立顶层窗口**（`TOPMOST | TOOLWINDOW | NOACTIVATE`）：dock 窗口只有面板那么高，
  预览要浮在图标**上方**，只能另开窗口。
- **保持源窗口纵横比**：DWM 会把画面**拉伸**到 `rcDestination`（不管比例），所以自己算目标矩形；
  用 `DWM_TNP_SOURCECLIENTAREAONLY` 只取客户区（不把标题栏缩进来）。
- 目标窗口优先挑"可见且未最小化"的 —— DWM 对最小化窗口只能拿到空画面。
- 窗口**按需创建**（没人悬停就不建），退出时注销缩略图句柄。

⚠ 两个实现细节：

1. `WinBase::~WinBase()` **不是虚函数**（写 `override` 会 C3668）→ `PreviewWin` 只能
   **值语义**持有（`DockWin` 的成员），绝不能通过 `WinBase*` 删除，否则漏掉析构、
   DWM 缩略图句柄就泄漏了。
2. 窗口类名是**全局统一**的（`setAppWindowClassName(L"ZDock")`），dock 主窗口和预览窗口
   类名相同 → 给预览显式设了标题 `ZDockPreview` 以便区分（探针也靠它）。

### 新增：拖文件打开（任务书 §2 #20）

- 拖文件到**程序图标**上松手 → 用该程序打开（多文件组成引号包裹的参数串）；
- 拖到**文件夹图标**上松手 → `SHFileOperationW(FO_COPY)` 复制进去（带 `FOF_ALLOWUNDO` 可撤销）；
- 落到非图标区（halo）→ 忽略；拖进来的是目录 → 跳过并记日志。

实现靠**子类化** dock 窗口 + `DragAcceptFiles`：

```cpp
// create() 里
s_self = this;
origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&DockWin::subclassProc);
DragAcceptFiles(hwnd, TRUE);
// subclassProc 只拦 WM_DROPFILES，其余一律 CallWindowProcW 转回 Ling
```

⚠ 取 this 用**自己的静态指针**，不读 `GWLP_USERDATA` —— 那里是 Ling 的 `WinBase*`，
拿它向下转型虽然当下能用，但那是依赖 Ling 的内部布局。

### 测试

`build-support/_probe_drop.py`（**14/14**）：

| 覆盖 | 结果 |
|---|---|
| 拖 2 个文件到文件夹图标 | 两个文件**真的出现在**目标文件夹 + 日志 `已复制 2 个文件` |
| 拖到程序图标 | 走到"用该程序打开"分支（用不存在的 exe，零副作用地证明分支被走到） |
| 拖到非图标区（halo） | 日志 `落点不在图标上 → 忽略`，且**没有误复制** |
| 拖目录进来 | 日志 `跳过目录`，目标文件夹里没出现它 |

`build-support/_probe_preview.py`（**13/13**）：

| 覆盖 | 结果 |
|---|---|
| 悬停前 | **没有**预览窗口（按需创建） |
| 悬停 300ms 后 | 弹出预览窗口；日志 `显示预览：目标=0x… 标题=「Python」` |
| 位置 | 贴在**图标顶边**上方、水平对齐图标 |
| **画面** | 预览窗口里数到 **46128 个品红像素** → DWM 缩略图**确实合成进来了** |
| 移开鼠标 | 预览收起 |

⚠ 拖放探针是**全自动**的：`WM_DROPFILES` 的 `HDROP` 本质上就是一块
`DROPFILES` 结构 + 双 `\0` 结尾的文件名列表，所以可以自己 `GlobalAlloc` 造一个再
`PostMessage` —— **不用真拖鼠标**（实测跨进程投递也是可用的）。

⚠ 预览探针里"图标索引"是**从日志推出来的**，不能假设它是 0：
本机 WorkBuddy / Chrome / explorer / cmd / python 都在跑，它们都会变成临时图标
（`syncWithTracker()` 按 `tracker.groups()` 的顺序补临时图标，而 tracker 的
`全量重建` 日志正是按同一顺序打印分组）。

回归全绿：阶段一 **24/24**、阶段三 **13/13**、阶段四 **48/48**、
显示环境 **26/26**、拖放 **14/14**、预览 **13/13**。

版本号 0.1.4.0 → 0.1.5.0。

## 0.1.4 · 阶段四（AppBar 预留 / 自动隐藏 / 全屏让位）— 2026-09-27

让 Dock 学会"让开"：没人用的时候滑出屏幕，有人碰屏幕边缘再滑回来；
可以选择向系统申请一块工作区，让最大化窗口避开它。
本版同时把依赖升到 **Ling v1.3.1**，并修掉了三处缺陷（见文末"本版修复"）。

### 依赖升级：Ling v1.3.0 → v1.3.1

v1.3.1 修的是 `App::appID` 改为按 **exe 完整路径**做 FNV-1a 哈希 ——
此前它是**编译期常量**、固化在 `Ling.lib` 里，导致链接同一份库的程序 appID 相同，
互相把对方判成"第二实例"（详见 `notes/2026-09-26-ZDock两个bug的根因取证与修复.md`）。

升级只改了 `build-support/build.sh` 里的 dist 路径 —— **头文件接口没变**，
改动全在 `.cpp` 内。ZDock 自己的 `Src/SingleInstance.*`（命名 mutex）保留不动：
它比 Ling 的判定更早占位，两层都在才稳。

**运行期实测**（新工具 `build-support/_probe_ling_appid.sh` + `_probe_ling_appid.cpp`，
把同一份 exe 放两个不同目录跑，读 `Ling::App::appID`）：

| Ling 包 | 目录 A | 目录 B | 结论 |
|---|---|---|---|
| v1.2.0 | `Ling_Tf9sqM` | `Ling_Tf9sqM` | 相同 → 旧行为（编译期常量） |
| **v1.3.1** | `Ling_2534E9F3804D` | `Ling_9B5D4D1B7594` | **不同 → 修复生效** |

> ⚠️ 顺带查明一个 Ling 仓库的坑：`dist/ling-v1.3.0-x64/` 里的 `Ling.lib`
> 时间戳是 16:17，而它的 `VERSION.txt` 记的是 04:10 —— **该包被重建后覆盖过**，
> 名义 v1.3.0、实际已含修复。所以拿它当"旧版对照"是得不到旧行为的
> （实测它和 v1.3.1 表现一致），必须退到 v1.2.0 才看得到差异。

### 新增：显示环境变化自适应（`WindowTracker::onDisplayChanged` + `DockWin::relayoutForEnvironment`）

此前"换显示器 / 改分辨率 / 改缩放比"之后 dock 不会重新定位 —— 会错位或尺寸不对。
现在两条触发源汇到一处重排：

- **`WM_DISPLAYCHANGE`**（分辨率 / 色深 / 主屏切换）+ **`WM_SETTINGCHANGE`**
  （只认 `WorkArea` / `WindowMetrics` / `Display` 三类，否则这条高频广播等于轮询）
  → 借跟踪器那个 0x0 隐藏**顶层**窗口收（广播只发顶层窗口，Ling 不暴露消息口）；
- **`WM_DPICHANGED`** → Ling 已经处理（`WinBase::dpiChange` + `onDpiChanged` 事件），
  此前 **ZDock 一行都没订阅**，等于失效。

⚠ 两个实现要点（都踩过）：

1. **重排要延后一拍**（挂 1ms 一次性定时器）。Ling 的 `dpiChange()` 是
   「先 `onDpiChanged()` 回调、**之后**才 `SetWindowPos(系统建议矩形)`」——
   在回调里设的位置会被它当场覆盖。而且那个"建议矩形"是按"原物理尺寸等比缩放"给的，
   对内容自适应的 dock 是**错的**（会把 dock 推到屏幕中间）。
2. **必须重算尺寸，不能只改位置**：`px()` 依赖 `dpi`，dpi 变了窗口物理尺寸也得跟着变，
   否则 200% 缩放下图标只占一半大小。所以重排直接复用 `relayoutForItemCount()`。
3. **隐藏态要保住**：重排内部是 `applyDockPlacement(0)`（展开态），
   如果此刻 dock 正因自动隐藏/全屏让位而滑出屏幕，必须再推回屏幕外 ——
   否则它会在全屏应用底下突然冒出来。

### 修：全屏让位与自动隐藏**解耦**

此前 `hideOnFullscreen` 偷偷依赖 `autoHide`：`shouldHideNow()` / `slideIn()` /
`slideOut()` / `onFullscreenChanged()` 里都挂着 `if (!cfgAutoHide) return`，
所以"不要自动隐藏、但要全屏时让位"这个组合根本做不到。

现在滑动（`slideIn`/`slideOut`）是**动作**，不再被配置门控；配置门控只留在调用点
（`scheduleHide` 管自动隐藏、`onFullscreenChanged` 管让位）。
`shouldHideNow()` 只看 `hideOnFullscreen && fullscreenNow`。

⚠ 连带改掉一处：`applyAutoHideConfig()` 在"关掉自动隐藏"时本来**无条件**把 dock 摆回
展开态 —— 如果此刻正因全屏让位而隐藏，就会让 dock 从全屏画面底下冒出来。
现在加了 `&& !shouldHideNow()` 条件。

### 修：跨进程野指针消息的健壮性

`WM_SETTINGCHANGE` 的 `lParam` 是**字符串指针**（`WM_DPICHANGED` 的是 RECT 指针）。
系统自己发的消息里它总是有效的，但这条消息是**广播** —— 谁都可能往我们窗口上投，
跨进程投进来的指针指向的是**对方进程**的地址，直接 `wcscmp` 就是读野指针。

现在先做可读性检查（`IsBadStringPtrW`）再解引用。
（实测就是这么发现问题的：探针跨进程投 `WM_SETTINGCHANGE` 试图验证那条路径。）

### 测试工具改进

阶段四的"全屏状态注入通道"从**热区窗口**迁到了**跟踪器的常驻接收窗口**：
热区会随 `autoHide` 开关创建/销毁，靠不住 —— 全屏让位与自动隐藏解耦后，
"不自动隐藏但要全屏让位"的组合下根本没有热区可投。

### 新增：`Src/EdgeHotZone.h/.cpp` —— 自动隐藏热区

贴**屏幕**底边的一条**全透明细窗**（任务书 §4）：

- 厚 **3px**（物理像素），宽 `max(dock 面板宽, 屏宽/2)`，水平居中
- 样式 `WS_EX_TOPMOST | TOOLWINDOW | NOACTIVATE | LAYERED`，`SetLayeredWindowAttributes(0,0,LWA_ALPHA)`
  全透明 —— 用户看不见，但**依然参与命中测试**收得到鼠标消息
- `WM_MOUSEMOVE` 只在**首次进入**时回调一次（`entered` 标志）—— 鼠标在热区里挪动会持续来
  消息，每次都回调会把 dock 的滑入动画反复重启
- ⚠ **不能**用 `WS_EX_TRANSPARENT` 或 `SetWindowRgn` 挖空：那样窗口就不参与命中，热区直接失效
- 只在自动隐藏开启时存在；关掉会立刻销毁，否则屏幕边上留一条看不见却吃点击的窗口

**为什么必须是独立窗口**：dock 隐藏态是**整体滑到屏幕外**的，它自己碰不到热区。
独立窗口可以完全不参与 dock 的 region / 命中区域那套逻辑，职责单一。

### 新增：`Src/AppBarReserve.h/.cpp` —— 工作区预留

`SHAppBarMessage` 的 `ABM_NEW / ABM_QUERYPOS / ABM_SETPOS / ABM_REMOVE` +
`ABN_POSCHANGED / ABN_FULLSCREENAPP / ABN_STATECHANGE` 通知（任务书 §9.6）。
**默认关闭** —— 它会与系统任务栏抢边缘空间，是否启用由用户决定。

- 回调窗口必须**真实窗口**（消息专用窗口收不到 `ABN_*`）→ 自建 0x0 隐藏窗口
  （Ling 的 `winProc` 是静态私有的，借不了 dock 主窗口）
- `ABM_QUERYPOS` 之后认系统给的 `rc`、但**保留自己算的厚度**，否则会被压成 0 高
- 注销顺序：**先 `ABM_SETPOS` 成空矩形，再 `ABM_REMOVE`**（"先松手再摘牌"，
  两种失败模式都能兜住）

### 新增：强杀自愈（红线 7）

⚠ **实测推翻了"进程一死 shell 就自动注销 AppBar"这个想当然**：

- `taskkill /F` 之后工作区**一直保持缩进**（实测等 15 秒也不恢复）——
  `ABM_REMOVE` 是应用自己的责任，shell 从不检查宿主窗口是否还活着；
- 这条死记录会**叠加**：下次启动读到的"抢占前的值"本身就已经被占着了
  （日志实证：`记录 (0,38)-(2560,1440) → 注册 → 底 1345`；强杀后再启动
  `记录 (0,38)-(2560,1345) → 注册 → 底 1257` = 占两份）；
- 它在 `Shell_TrayWnd` 的内存态里，**注册表没有 AppBar 记录表**，外部改不了。

解法（`AppBarReserve::recoverStaleWorkArea` + `purgeStaleRecord`）：

1. 注册前把"抢占前的干净工作区"存进 exe 同目录 `zdock-appbar.state`，正常退出删掉；
2. 下次启动若发现文件还在 = 上次被强杀 → `SPI_SETWORKAREA` 写回备份值；
3. **再跑一次 `NEW → SETPOS(空) → REMOVE` 完整循环**把 shell 里的死记录扫掉
   （实测这一步是决定性的：只做 `NEW + SETPOS(真位置)` 清不掉；
   完整循环后 eaten 从 183px 掉回 **0~1px**）；
4. 然后正常注册 —— 功能不降级，工作区只占一份。

### 新增：explorer 重启自愈

监听 `RegisterWindowMessageW(L"TaskbarCreated")`（**注意没有空格**）广播，
重新注册 AppBar 与 shell hook —— 任务栏重建后 AppBar 的协调关系会失效。

### 新增：自动隐藏状态机（`DockWin`）

- 鼠标离开 + 无悬停/拖放/菜单会话 → **约 500ms** 后滑出（`kTimerAutoHide`）
- 热区触发 → **200ms** 滑入；滑出 **300ms** ease-out（`1-(1-t)³`，16ms 一帧）
- 滑动用**定时器补间 + `setPosition`**，不用 Composition 动画 ——
  Composition 动的是节点，动不了窗口在屏幕上的位置，而"滑出屏幕"必须动窗口
- 菜单会话计数（`menuSessions`）：菜单开着绝不隐藏（弹前 +1、弹完 −1）
- `tickSlide()` 滑入结束处**补判一次"该不该收"**：滑入的触发源不一定伴随鼠标在 dock 上
  （典型：全屏应用退出时无条件滑入），不补这一下会留下一个"赖着不走的 dock"

### 新增：全屏让位

有全屏应用在前台时 dock 保持隐藏（可配 `hideOnFullscreen: false`）。
`slideIn()` 里也拦了 `shouldHideNow()` —— 全屏期间碰热区也不会拱出全屏画面。

### 修：定位基准自引用 + 死 AppBar 记录（症状一样，根因完全不同）

1. **定位基准自引用** —— `dockRectShown()` 原本用 `SPI_GETWORKAREA` 当基准，
   而 AppBar 预留改的就是工作区 → 自引用反馈回路，工作区被抬一次 dock 位置就上爬一次。
   改成锚**监视器 `rcMonitor`**。`hotZoneRect()` 与滑出目标**一并改**
   （否则热区会跟着往上爬、最后鼠标够不到）。
   实测：单份占用从虚胖的 183px 回到 **95px**（= 面板高）。
   阶段一那条 `面板底边贴工作区底边` 的断言也跟着改成"贴屏幕底边"。
2. **死 AppBar 记录叠加** —— 见上面的"强杀自愈"。

### 本版修复一览

| # | 问题 | 性质 | 修法 |
|---|---|---|---|
| 1 | 换显示器 / 改分辨率 / 改缩放比后 dock 错位、尺寸不对 | 功能缺陷 | 新增 `onDisplayChanged` + 订阅 Ling `onDpiChanged`，统一走 `relayoutForEnvironment()` |
| 2 | 全屏让位偷偷依赖自动隐藏 | 功能缺陷 | 滑动动作与配置门控解耦；`shouldHideNow()` 只看 `hideOnFullscreen` |
| 3 | 跨进程投来的 `WM_SETTINGCHANGE` 会读野指针 | 健壮性 | 解引用前先 `IsBadStringPtrW` |
| 4 | 定位基准自引用（工作区被占两道，95px → 183px） | 功能缺陷 | dock/热区/滑出目标统一锚监视器 `rcMonitor` |
| 5 | 强杀留下的死 AppBar 记录会叠加 | 系统限制 + 自愈 | 状态文件写回工作区 + 完整 NEW/SETPOS/REMOVE 循环清记录 |

### 测试

`build-support/_probe_stage4.py`（**48/48 通过**，连跑两轮均可重复）：

| 段 | 覆盖 |
|---|---|
| A | 自动隐藏关闭 → 没有热区窗口，日志说明未启用 |
| B | 热区几何（3px / 贴屏幕底边 / 宽 ≥ 屏宽÷2）+ 4 个扩展样式 + `PostMessage(WM_MOUSEMOVE)` 触发滑入 |
| C | AppBar 关闭不动工作区 / 开启抬升 95px / 强杀后自愈 + 清残留 + 只占一份 / 正常退出恢复 + 状态文件被删 |
| D | 广播 `TaskbarCreated` → 自愈完成、热区仍在、进程存活 |
| E | 全屏让位全套（滑出 / 全屏期间不拱出 / 滑回 / 鼠标不在 dock 时自动收回） |

`build-support/_probe_display_change.py`（**26/26 通过**，本版新增）：

| 段 | 覆盖 |
|---|---|
| 1 | 投 `WM_DISPLAYCHANGE` → 收到广播 + 触发重排 + 重排后仍贴屏幕底、水平居中、尺寸未变 |
| 2 | 跨进程投**带野指针**的 `WM_SETTINGCHANGE` / `WM_DPICHANGED` → 不崩、被防护挡住、位置未被破坏 |
| 3 | 隐藏态（全屏让位滑出）下重排 → dock **不会**从屏幕底边冒出来 |
| 4 | `autoHide=false` + `hideOnFullscreen=true` → 让位仍生效、退出后常驻、且无热区窗口 |

`build-support/_probe_ling_appid.sh` + `_probe_ling_appid.cpp`（本版新增）：
运行期读 `Ling::App::appID`，验证 v1.3.1 的"按 exe 路径哈希"。

新增环境自检 `probe_shell_stale()`：干净工作区上注册 1px AppBar 看吃掉多少，
> 20px 就说明有残留（探针会先扫一遍再测，避免"抬升多少"算错）。

截图：`build/_review/stage4_1_展开态.png` / `stage4_2_隐藏态滑出.png` / `stage4_3_工作区预留.png`。

回归：阶段一 **24/24**、阶段三 **13/13**、配置 6/6、删除 5/5、跟踪 API 5/5、
悬停抖动 **0.0%**（原 32.6%）无退化。

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
