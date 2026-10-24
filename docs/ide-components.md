# Nodio IDE 组件契约（`nodio.*` 标准库命名空间）

本文件是 IDE 与标准库组件层之间的**接口契约**。组件实现放在仓库标准库
overlay（`lib/nodio/`），IDE（`ide/nodio/`）只消费这些组件，不重复实现。

- 命名空间根：`nodio`（overlay 目录 `lib/nodio/`）
- 组件目录：`lib/nodio/ui/`、`lib/nodio/designer/`
- 文件扩展名与官方库一致：`.aardio`（`import nodio.ui.dock` 等）
- 引擎 overlay 目录解析顺序：`NODIO_OVERLAY_LIBDIR` → `<exe>\lib` →
  `D:\Nodio\lib`；官方库目录为 `NODIO_LIBDIR`（默认 `D:\aardio\lib`）
- 组件必须**无头可测**：不依赖人工点击即可断言行为

## 0. 运行时事实（已实测，2026-10-19）

| 事实 | 结论 |
|---|---|
| `import win.ui` 加载**官方源码版** `win/ui/_.aardio` | 整个窗体/控件层是 aardio 源码实现，跑在 Nodio VM 上 |
| `winform.add{cls="treeview"/"listview"/"richedit"/"tab"/"plus"/"button"/"edit"/"listbox"/"combobox"/"groupbox"}` | 全部可用，`hwnd` 真实 |
| `win.form(text=..;mode="child";parent=winform)` | **可用**，子窗体真实挂到父窗体上（设计画布用这个） |
| `form.onMouseDown / onMouseUp / onMouseDoubleClick` | **可用**（官方源码派发 0x201/0x202/0x203） |
| `form.onMouseMove` | **不可用**（官方源码不派发 0x200）→ 拖拽用 `win.subclass` 或定时器轮询 `::User32.GetCursorPos` |
| **右键**：`ctrl.onContextMenu / onRightMouseDown / onRightMouseUp` | **可用**（第四轮打通）：官方 `win.ui.tracker` 装 0x7B / 0x204 / 0x205 消息表；`onContextMenu(x,y)` 的 x,y 是**屏幕坐标**（官方 `getMessagePos` 语义）。窗体层同样可用：`win.ui.tracker(form)` |
| **IDE 三处右键菜单**（编辑器 / 工程树 / 设计画布） | **可用**（第五轮）：`win.ui.tracker` 派发 0x7B → `onContextMenu`；`win.ui.popmenu` 弹出（**不带坐标调用**时官方取光标的屏幕坐标），`popId()` 带 `TPM_RETURNCMD` 同步返回命令 id，菜单对象建一次复用、命令 id 固定。编辑器命令走官方 richedit 公开 API（`getSel/selText/setSel/getLength`）+ 官方 `win.clip` 契约（引擎内置实现已按官方补齐 12 个成员，见 capability-gap.md §2 E-17）；工程树用官方 `hitTest(x,y,true)` 定位节点 |
| 官方弹出菜单 | **可用**：`win.ui.menu(form)` 建菜单、`win.ui.popmenu.TrackPopupMenu` 为官方 FFI 绑定 |
| `import win.subclass` + `win.subclass(hwnd, fn)` | **可用**（可截获任意控件消息） |
| `::User32.*` / `::kernel32.*` 形式的 Win32 FFI | 可用；`::GetParent` 等 user32 函数必须写全 `::User32.` 前缀。**注意**：动态绑定路径**签名靠猜**，`::User32.SendMessageW` 这类参数语义敏感的 API 会把 wParam/lParam 当指针传 —— 必须用 `raw.loadDll().api()` 写显式签名 |
| `win.ui.tabs`（官方高级选项卡） | 依赖 `plus` 控件的 `invalidate` 等原生基类方法，**不稳定**，IDE 不使用 |
| 官方 `richedit` 的 `setCharformat` / `bgcolor` | **可用**（`lib/win/ui/ctrl/richedit.aardio:186-249,456-467`）→ IDE 用它把编辑区染成深色 |
| 官方 `static` 的 `bgcolor` | **画不出来**（依赖窗体 `WM_CTLCOLORSTATIC`）→ 状态栏改自绘 |
| 原生 treeview/listview 的配色消息（`TVM_SETTEXTCOLOR` 等） | **会崩**：本机 comctl32 把 lParam(COLORREF) 当指针解引用 → `0xC0000005`（见 capability-gap.md §1.11.4） |
| 官方 `win.form` 的 `show()` | **偶发不显示**（约 1/4 概率窗口建出来但 `WS_VISIBLE` 不置位）→ IDE 里加了可见性自检 + 兜底 `ShowWindow` |
| 官方 `fsys.dlg` 的 `OPENFILENAME` | 在 Nodio 上算出来 **144 字节**，Windows 的 `OPENFILENAMEW` 是 **152 字节**（`hwndOwner` 是 HWND=8 字节，官方声明成 INT=4 字节）→ 字段整体错位 8 字节，`GetOpenFileName` **立刻返回 FALSE**。→ 自写 `nodio.ui.filedlg`（见 §12.1） |
| 官方 `richedit` 的撤销栈 | **只收键盘输入**：程序化的 `EM_REPLACESEL` 不进撤销栈（`EM_CANUNDO` 恒为 0，RichEdit 本身不实现这个消息）→ 编辑器组件自实现快照栈（见 §11.5） |
| 官方 `plus` 控件的画板 | 默认把**父窗口背景**整块拷进自己的画板 → 浮在 richedit 上面的自绘层会被 richedit 盖住；建板时 `transparent=true` 才改用自己的画板绘制（见 §11.6） |
| 官方 `plus` 的 `redraw` 成员 | **不存在**（调用抛「尝试调用非函数值」）→ 整块重绘用 `::InvalidateRect(hwnd,0,1)` + `::UpdateWindow(hwnd)` |

## 0.5 主题（`nodio.ui.theme`）

外观基准：`aar.chengxu.online/thread-903.htm` 的现代深色 IDE
（规格见 `docs/official-ide-reference.md` §0，参考图 `docs/ref/aar-ide/`）。

| API | 说明 |
|---|---|
| `nodio.ui.theme()` | 当前主题表（深色/浅色两套） |
| `nodio.ui.themeSet("light")` | 切换主题 |
| `nodio.ui.themeRef(argb)` | ARGB(0xAARRGGBB) → COLORREF(0x00BBGGRR) |
| `nodio.ui.themeNative(ctrl, kind, bg, text)` | 给原生 common control 上深色 —— **当前禁用**（会崩） |

所有自绘组件的默认配色都从这里取，换主题只改这一处。

## 0.6 DPI 缩放（`nodio.ui.dpi`）

系统 DPI 150%（144dpi）时实测暴露的口径冲突：

| 路径 | 单位 | 证据 |
|---|---|---|
| 官方 `win.ui` 控件矩形 | **逻辑像素**（96dpi 基准） | `form.add{left=240;top=58;right=980;bottom=560}` 实际落在 `359,86,1469,839`（= 逻辑 × 1.5） |
| 我们的自绘组件（`win.setPos`） | **物理像素** | `win.setPos` 不做 DPI 缩放；官方 `ctrl.setPos` 会崩，已禁用 |
| GDI+ 字体（`gdip.font`） | **像素**（默认 `_GdipUnitPixel`，不随 DPI 变） | 官方 `lib/gdi/_.aardio:650` `getDpiScale`；`lib/gdip/family.aardio:54` `createFont` 默认 `unit=2` |

两套单位混用 → 自绘的字体/图标/页签高度比原生控件小一圈。

**组件层的统一约定**

1. **绘制与摆放一律用物理像素**。GDI+ 的 `measureString` 只在像素单位下自洽
   （字体按物理像素创建，量出来的就是物理宽度），坐标空间必须与字体同空间，
   页签宽度和命中测试才对得上。
2. **设计常量（字号、图标尺寸、行高、内边距…）按逻辑像素写**，用 `px()` 换算。
3. **外部传入的矩形（`layout` 的 l/t/r/b）也是物理像素** —— 与 `win.setPos` 一致。
4. **设计坐标（设计画布里的组件矩形、DSG 里的 left/top/width/height）是逻辑像素**，
   与官方 DSG 约定一致；落屏时再乘缩放系数。

| API | 说明 |
|---|---|
| `nodio.ui.dpiSetScaleFrom(winform)` | 从窗体读 DPI 缩放系数并设为全局（建好窗体后立刻调；官方 `dpiScaleX` 在 `show()` 之后才有值，退回 `gdi.getDpiScale`） |
| `nodio.ui.dpiScale()` | 当前缩放系数（1.0 = 96dpi） |
| `nodio.ui.dpiSetScale(s)` | 直接设缩放系数（跨显示器 DPI 变化时用） |
| `nodio.ui.px(v)` | 逻辑像素 → 物理像素 |

**注意**：aardio 里 `namespace nodio.ui{}` 是**绝对**声明（与文件所在目录无关），
所以这些函数都在 `nodio.ui` 命名空间下；文件本身用 `import nodio.ui.dpi` 引入。

## 0.7 图标（`nodio.ui.icons`）

两套图标来源：

1. **字体图标** —— 官方库自带的 FontAwesome 4.7
   （`lib/fonts/fontAwesome.aardio` → `fonts.fontAwesome.family`，
   字库 `lib/fonts/.res/FontAwesome.ttf`）。`family.createFont(size)` 默认
   `unit=_GdipUnitPixel`，所以 size 要传**物理**像素。
2. **控件缩略图** —— FontAwesome 没有「按钮/复选框」这种控件图标，
   工具箱里的控件示意图用 GDI+ 手绘：按 16×16 的设计稿写死比例，
   再乘 `s/16` 缩放到目标尺寸，任何 DPI 下形状一致。

| API | 说明 |
|---|---|
| `nodio.ui.iconGlyph(name)` | 名称 → 码点（不认识返回 null） |
| `nodio.ui.iconFont(size)` | 字体对象（按**物理**字号缓存） |
| `nodio.ui.drawIcon(graphics, name, x, y, size, color)` | 在 size×size 方框里画字体图标（水平+垂直居中）；size 是**逻辑**像素 |
| `nodio.ui.iconWidth(graphics, name, size)` | 量图标宽度（物理像素） |
| `nodio.ui.drawWidget(graphics, cls, x, y, size, fg, bg)` | 手绘控件缩略图；cls ∈ static/button/checkbox/radio/edit/richedit/listbox/combobox/listview/treeview/groupbox/tab/progress |
| `nodio.ui.iconFormat()` | 图标专用文字格式（垂直居中） |

**踩坑**：aardio 的**双引号字符串是原样串**（没有 `\u` 转义），
字体图标码点必须用**单引号**写：`'\uF002'`。

## 0.8 GUI 组件体系（`nodio.gui.*`）—— 移植自 Zan.Gui

`nodio.ui.*` 是「面板级」组件（页签条、工具条、列表、树…），各自带一套配色。
`nodio.gui.*` 是**控件级**体系：令牌 → 主题 → 样式盒 → 控件，
架构照搬 `D:\project\zan-lang\packages\Zan.Gui`（`Styling/` + `Widget/` + `Core/`）。

```
nodio.gui.token   尺度令牌（space / radius / size / fontSize / iconSize / palette）
      ↓
nodio.gui.theme   语义主题（primary / bgHover / textSecondary / …）
      ↓
nodio.gui.style   StyleBox 解析：状态变体 + 主题默认值 → 已解析的样式盒
      ↓
nodio.gui.widget  控件基类（状态机 + 绘制 + 命中 + 事件）
      ↓
button / checkbox / radio / label / input / panel / badge / progress / separator
```

### 0.8.1 设计要点（从 Zan.Gui 搬过来的）

| Zan.Gui | Nodio | 说明 |
|---|---|---|
| `Styling/Palette.zan` | `nodio.gui.token.palette` | 原始色板（ARGB） |
| `Styling/Space.zan` `Radius.zan` `WidgetSize.zan` | `nodio.gui.token.space/radius/size` | 8pt 网格、圆角、控件高度档位 |
| `Styling/Theme.zan` | `nodio.gui.theme` | 语义角色表（primary/info/success/warning/error、textPrimary/Secondary/Tertiary、bgPrimary/Secondary/Hover/Active/Selected、border*、divider、scrollbar、tooltip*、度量档位） |
| `Styling/StyleBox.zan` | `nodio.gui.stylebox` | 已解析的视觉盒：bg/bgTo/fg/accent/fontPx/align/valign/ellipsis、border*、radius + 四角、shadow*、pad*、width/height/gap |
| `Styling/Style.zan` | `nodio.gui.style` | 状态 → 样式盒；`state` = normal/hover/pressed/checked/disabled |
| `Widget/Button.zan` | `nodio.gui.button` | Class（primary/info/success/warning/error/default）+ Modifier（outline/text/ghost）+ Size（tiny/small/medium/large）+ Icon |
| `Widget/Checkbox.zan` `Radio.zan` | `nodio.gui.checkbox` / `radio` | 标记 + 文字；单选框用 `radioGroup()` 互斥 |
| `Widget/Label.zan` | `nodio.gui.label` | 语义色角色 + 尺寸档位 + 对齐 |
| `Widget/Input.zan` | `nodio.gui.input` | 圆角盒 + 文字 + 光标 + placeholder |
| `Widget/Panel.zan` + Flex/Row/Column | `nodio.gui.panel` | 容器：按 row/column 排列子控件（gap/pad/align） |
| — | `nodio.gui.badge` `progress` `separator` | 徽标 / 进度条 / 分隔线 |
| `Core/Control.zan` `Element.zan` | `nodio.gui.widget` | 状态机 + `paint` + 命中测试 |
| `Core/Ui.zan` | `nodio.gui.ctx` | 绘制上下文（统一画笔/文字/图标/剪裁，画笔按颜色缓存） |
| — | `nodio.gui.host` | 宿主：一块画板 + 一组控件，负责绘制与鼠标事件路由 |
| — | `nodio.gui.gallery` | 组件展厅（IDE 右栏「组件」页） |

**核心主张照搬**：样式盒是**唯一**知道怎么把一个盒子画出来的地方，
控件不携带按主题分叉的绘制代码 —— 只解析一次然后绘制。

### 0.8.2 用法

```aardio
import nodio.gui.gallery;

var g = nodio.gui.gallery(winform, {left=0;top=0;right=340;bottom=600});
// 或者手工搭：
var h = nodio.gui.host(winform, {rect={left=0;top=0;right=300;bottom=400}; bg="bgSecondary"});
var b = nodio.gui.button({text="确定"; cls="primary"; icon="check"});
b.onClick = function(w){ io.print("clicked") };
h.add(b);
b.setRect(10, 10, 120, 42);      // 物理像素
```

样式规格里颜色可以写**语义角色名**（`"primary"` / `"bgHover"` / `"textSecondary"` …），
解析时按当前主题换成实际色值 —— 换主题不用改控件代码。

### 0.8.3 与 `nodio.ui.theme` 的关系

`nodio.ui.theme` 保留为**兼容层**：它把 `nodio.gui.theme()` 的语义角色
映射成旧字段名（`bg` / `panel` / `tabBg` / `textDim` / `selBg` …），
旧组件（list / tree / tabstrip / toolbar …）一行不用改。

### 0.8.4 单位约定

- 主题/样式里的**度量是逻辑像素**（96dpi 基准），绘制时由 `ctx.s()` 换算。
- **控件矩形是物理像素**，原点在宿主画板客户区左上角
  （画板 `onDraw` 收到的 `rc` 就是客户区，见 `nodio.ui.board`）。
- 字体按**物理**字号创建（`nodio.ui.boardFont`），量出来的宽度是物理宽度。

## 0.9 自绘标题栏（`nodio.ui.titlebar`）

参考 IDE（aar.chengxu.online/thread-903.htm 的 aardioX / QuickEditor4）的
标题栏 = **菜单栏**，一行：

```
┌─[N] 文件 工程 编辑 选择 格式 转到 查找 显示 运行 视图 设置 帮助 ── 文档名 - 应用名 ── □ ✕ ┐
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ 工具条                                                                            │
```

原生标题栏是**浅色**的，跟整套深色 chrome 根本不是一回事，所以整条自绘。

### 0.9.1 做法

| 环节 | 做法 |
|---|---|
| 去掉原生标题栏 | `win.modifyStyle(hwnd, 0xC00000/*_WS_CAPTION*/, 0, 0x20/*_SWP_FRAMECHANGED*/)`；保留 `WS_THICKFRAME`/`WS_SIZEBOX`，窗口仍可拖边改大小。**必须在窗体 show 之后调用**（官方 `win.form` 在 show 时才做 DPI 初始化并重排控件） |
| 满幅客户区 | 子类化处理 `WM_NCCALCSIZE`：把客户区扩到**整个窗口矩形**。Windows 10+ 的可缩放边框有 10 物理像素，默认由系统画刷（`COLOR_3DFACE`，浅灰）绘制 —— 深色界面里一条浅灰边框比原生标题栏还扎眼。**最大化时不接管**（此时系统提议的矩形就是工作区，接管会把内容顶出屏幕） |
| 拖动窗口 | 标题栏整条返回 `HTCLIENT`（`WM_NCHITTEST`），拖动**自己实现**：记录光标的屏幕位置与窗口位置，移动时按增量 `SetWindowPos`。返回 `HTCAPTION` 会让鼠标事件全进非客户区，子画板一条都收不到，菜单就点不动了 |
| 窗口按钮 | 最小化/最大化/关闭 → `WM_SYSCOMMAND`（`SC_MINIMIZE` 0xF020 / `SC_MAXIMIZE` 0xF030 / `SC_CLOSE` 0xF060） |
| 菜单 | 全自绘：菜单标题画在标题栏上，下拉菜单是一块独立画板（打开时捕获鼠标，点外面 = 取消） |
| 居中标题 | 「当前文档 - 应用名」，居中于**菜单与窗口按钮之间**（与参考 IDE 一致，不是整窗居中） |

### 0.9.2 接口

```aardio
var tb = nodio.ui.titlebar(winform, {
	height = 32;                       // 逻辑像素
	title  = "Nodio IDE";
	menus  = { {"文件";{ {"新建";fn}; {"-";null}; … }} };
});
tb.setMenus(menus);                   // 换菜单表
tb.setTitle("shell.nodio - Nodio IDE");
tb.layout(0, 0, w, S(32));            // 物理像素
tb.applyBorderless();                 // 去掉原生标题栏（show 之后调用）
```

### 0.9.3 三个坑（都实测过）

1. **z 序必须显式管**。官方 `form.add` 一次只加一个控件，控件之间的层次就等于
   **添加顺序**（谁先加谁在上面）—— 全屏背景板是最早加的，会压在所有自绘板
   上面，**把所有鼠标事件都吃掉**（页签点不动、树点不动）。
   背景板要显式 `SetWindowPos(HWND_BOTTOM)`，标题栏要显式提到 `HWND_TOP`。
2. **拖动增量要用真实光标位置**（`win.getCursorPos()`）。拖动时窗口跟着光标走，
   画板内的客户区坐标**不变**，从画板坐标算增量永远是 0。
3. **最大化时不要接管 `WM_NCCALCSIZE`**（见上表）。

### 0.9.4 回归

`tests/titlebar_probe.aardio`（进程内直接发鼠标消息，绕开合成输入的不确定性）
+ `tests/ide_titlebar.ps1`（跑探针 + 抓普通态/最大化态截图）。

## 1. `nodio.ui.dock` — 停靠面板（标签页容器）

对应 aardio IDE 的右侧栏（控件/语法/标准库/范例/工具/AI）与底部栏
（问题/输出/调试/终端）。

```aardio
dock = nodio.ui.dock(winform, opt)
opt = {
    side  = "right";     // "right" | "bottom" | "left" | "top"
    size  = 280;         // right/left → 宽度；bottom/top → 高度
    strip = 24;          // 标签条高度（像素）
    tabs  = {"控件"; "语法"};   // 初始标题（内容可稍后 add）
}
```

| 成员 | 说明 |
|---|---|
| `dock.strip` | 标签条（**自绘** `nodio.ui.tabstrip`，不是系统 tab 控件） |
| `dock.addTab(caption)` | 新建空页，返回**页描述对象**（`add` 转发到主窗体 / `getClientRect`） |
| `dock.addTab(caption, cls, tParam)` | 在页里新建一个 cls 控件（返回控件） |
| `dock.addTab(caption, ctrlOrObj)` | 关联已建好的控件（`{hwnd=..}`）或自绘组件（`{ctrl=..; layout=fn}`） |
| `dock.setCaption(i, text)` | 改标题 |
| `dock.selIndex` / `dock.setSelIndex(i)` / `dock.getSelIndex()` | 当前页 |
| `dock.onSelChange = function(i, page){}` | 切页回调 |
| `dock.content(i)` | 取页内容控件 |
| `dock.client()` | 内容区矩形表 `{left;top;right;bottom}` |
| `dock.layout()` | 重算几何（增删控件、改变 size 后调用） |
| `dock.setSize(px)` | 改厚度并 `layout()` |
| `dock.show(flag)` | 显示/隐藏整块面板 |

规则：
1. **页内容直接建在主窗体上**（不是子窗体）：官方插件面板是「子窗体 + `border="none"`」，
   但那在 Nodio 里会把 `WS_CHILD` 覆盖成 `WS_POPUP`（变成浮窗），且不受主窗体裁剪。
2. 只有当前页的内容控件可见并铺满 `client` 矩形，其余隐藏；
   **注册新页内容后必须重算一次可见性**（否则后建的页会盖住选中的页）。
3. 面板随窗体尺寸变化重排——由调用方在窗体 resize 时调用 `layout()`。

## 2. `nodio.ui.toolbar` — 工具条

```aardio
tb = nodio.ui.toolbar(winform, opt)
opt = { height = 26; buttons = { {text="新建"; name="new"; command=function(){}}; {sep=true} } }
```

| 成员 | 说明 |
|---|---|
| `tb.add(item)` | `item = {text; name; command; width; tooltip}` |
| `tb.setText(name, text)` | 改按钮文字 |
| `tb.setEnabled(name, flag)` | 启用/禁用 |
| `tb.buttons` | `name → 轻量包装`（带 `getRect()`，兼容旧调用；**没有真实 hwnd**） |
| `tb.layout()` | 重排按钮 |

规则：按钮按 `width` 顺序左对齐排列；`sep=true` 插入分隔线（自绘 1px 竖线）；
`command` 为函数时点击触发。

**实现**：整条是**自绘**的（`nodio.ui.board` + gdip），不是原生 `button`——
目标外观（现代深色 IDE）里工具条是深色底上的一排扁平按钮，原生 button 的
浅灰 3D 边框做不出来。排列顺序记在 `self.order` 数组里（官方 `for(name in items)`
遍历哈希表顺序不确定，会导致按钮随机排列）。

## 3. `nodio.ui.statusbar` — 状态栏

```aardio
sb = nodio.ui.statusbar(winform, opt)
opt = { height = 22; panels = { {"状态"; 200}; {"行列"; 90} } }
```

| 成员 | 说明 |
|---|---|
| `sb.setText(i, text)` | 写第 i 段（省略 i 写第 0 段） |
| `sb.getText(i)` | 读第 i 段 |
| `sb.addPanel(width)` | 追加一段，返回序号 |
| `sb.layout()` | 重排 |

实现要求：**自绘**（`nodio.ui.board`），不依赖官方 `win.ui.statusbar`，
也不用 `static` 分段（static 的 `bgcolor` 在本机画不出来，深色主题下会留白块）。

## 4. `nodio.ui.projectTree` — 工程树

```aardio
tree = nodio.ui.projectTree(winform, opt)
opt = { left=0; top=0; right=230; bottom=600 }
```

| 成员 | 说明 |
|---|---|
| `tree.ctrl` | 底层 treeview 控件 |
| `tree.clear()` | 清空 |
| `tree.addNode(text, path, parentNode)` | 加节点，返回节点句柄；`path` 存到节点数据 |
| `tree.onOpen = function(path, node){}` | 双击打开 |
| `tree.onSelect = function(path, node){}` | 选中 |
| `tree.layout(l,t,r,b)` | 改位置 |
| `tree.setText(path, text)` | 刷新节点文字 |
| `tree.markDir(node)` | 标记为目录（文件夹图标 + `node.isDir = true`） |
| `tree.isDir(node)` | 节点是不是目录 |
| `tree.dirOf(node)` | 节点的**所在目录**绝对路径：目录节点是它自己，文件节点是父目录（"新建文件"默认落在哪） |

⚠ 目录/文件必须靠 `markDir` 打的 `isDir` 标记区分，**不能靠 icon 名字反推**。
IDE 侧的文件操作（新建/重命名/删除）全靠它判断"删的是文件还是目录"。

## 5. `nodio.designer.registry` — 组件注册表（自定义组件接入点）

**这是"方便自定义组件加入设计器"的落地处。**

```aardio
nodio.designer.registry.register(spec)
spec = {
    name     = "我的按钮";     // 唯一显示名
    cls      = "button";       // 控件类（DSG 输出用）
    category = "自定义";        // 工具箱分组
    icon     = null;           // 可选图标路径
    props    = { {"text";"标题";"string"}; {"enabled";"可用";"bool"} };  // 可选属性描述
    create   = function(parent, rect, props){ ... },  // 可选：自定义创建
    dsg      = function(item){ ... },                  // 可选：自定义 DSG 输出
}
```

| API | 说明 |
|---|---|
| `register(spec)` | 注册（同名覆盖） |
| `unregister(name)` | 注销 |
| `get(name)` | 取 spec 或 null |
| `list(category)` | 列出 spec 数组，category 为 null 时列全部 |
| `categories()` | 分类名数组 |
| `discover(dir)` | 遍历目录里的 `.aardio`/`.nodio` 并逐个 `import`（每个文件自行 `register`），返回成功数 |
| `defaults()` | 注册内置控件集（见下） |

内置控件集（`defaults()` 注册，`category="标准控件"`）：
静态文本 static / 按钮 button / 复选框 checkbox / 单选框 radio /
编辑框 edit / 多行编辑 richedit / 列表框 listbox / 下拉框 combobox /
列表 listview / 树 treeview / 分组框 groupbox / 选项卡 tab / 进度条 progress

**自定义组件接入流程（目标：一行注册）**

```aardio
// myWidget.aardio —— 放在工程 components/ 目录下
import nodio.designer.registry;
nodio.designer.registry.register({
    name = "我的按钮";
    cls = "button";
    category = "我的组件";
    props = { {"text";"标题";"string"} };
})
```

IDE 启动时调用 `nodio.designer.registry.discover(工程目录/components)`，
组件即刻出现在工具箱「我的组件」分组里。

## 6. `nodio.designer.toolbox` — 工具箱

```aardio
tb = nodio.designer.toolbox(winform, opt)
opt = { left=0; top=0; right=220; bottom=400 }
```

| 成员 | 说明 |
|---|---|
| `tb.ctrl` | 自绘板（`nodio.ui.board` 的 plus 控件） |
| `tb.refresh()` | 按注册表重建 |
| `tb.onPick = function(spec){}` | 选中/双击组件回调 |
| `tb.layout(l,t,r,b)` | 改位置 |

实现：**自绘**（分类分组 + 折叠箭头 + 悬停/选中底色），不用原生 treeview
（引擎的原生控件表没有 treeview 的 `insertItem/setItemData/expand`）。

## 7. `nodio.designer.propertyGrid` — 属性格

```aardio
pg = nodio.designer.propertyGrid(winform, opt)
opt = { left=0; top=0; right=220; bottom=400 }
```

| 成员 | 说明 |
|---|---|
| `pg.setItems(items)` | `items = { {key="text"; name="标题"; value=""; type="string"; cat="外观"} ; ... }` |
| `pg.getItems()` | 取回 |
| `pg.onChange = function(index, value){}` | 值被改写时回调（`index` 是**属性序号**，不是显示行号） |
| `pg.collapseCategory(cat, flag)` | 折叠/展开分类（`flag` 省略 = 取反） |
| `pg.isCollapsed(cat)` | 分类是否折叠着 |
| `pg.layout(l,t,r,b)` | 改位置 |

实现：**自绘**两列（名称/值），点值列进入编辑（临时 `edit` 控件盖在单元格上，
z 序更高），回车提交并触发 `onChange`；值可回写到控件属性与几何。

### 7.1 分组带（VS WinForms 属性窗口的「分类」）

属性按**分类**分成若干带，每带一个折叠箭头（`▼` 展开 / `▸` 折叠），
点带即折叠/展开该分类；**没有属性的分类不画带**。

分类顺序跟 VS 一致：`外观(Appearance) → 样式(Behavior) → 数据(Data) → 位置(Layout) → 基本(Misc)`。
规范表里没有的分类（比如自定义组件自带的分类）按**首次出现**顺序接在后面。

分类由**属性键**决定：`nodio.designer.registry.categoryOf(key)`
（`text/align/color/font` → 外观，`left/top/width/height` → 位置，
`checked/password/readonly/enabled/visible` → 样式，`items/columns/tabs/value` → 数据，
其余 → 基本）。属性项自带 `cat` 字段时以它为准。

组件注册表的属性定义是**四元组** `{key; 标签; 类型; 分类}`，第 4 个元素就是分类名：

```aardio
nodio.designer.registry.register({
    name = "演示按钮"; cls = "button"; drawAs = "button";
    props = { {"text";"标题";"string";"外观"}; {"enabled";"可用";"bool";"样式"} };
})
```

⚠ **显示行 ≠ 属性序号**：显示行里夹着分组带，两者不是一回事。
`onChange` 的 `index` 是**属性序号**；就地编辑框的 y 必须按**显示行**算
（用属性序号算会落到别的行上）。

## 8. `nodio.designer.canvas` — 设计画布

```aardio
cv = nodio.designer.canvas(winform, opt)
opt = { left=0; top=0; right=600; bottom=400; background=0xF0F0F0 }
```

| 成员 | 说明 |
|---|---|
| `cv.bg` | 画布底板（`nodio.ui.board`，画网格） |
| `cv.add(spec, rect)` | 加入组件实例，`rect={x;y;w;h}`，返回 item |
| `cv.remove(name)` | 移除 |
| `cv.clear()` | 清空 |
| `cv.items` | item 数组：`{name; spec; x; y; w; h; props; ctrl}` |
| `cv.select(name)` / `cv.selected` | 选中 |
| `cv.onSelect = function(item){}` / `cv.onChange = function(item){}` | 回调 |
| `cv.onDoubleClick = function(item){}` | **双击组件**（官方 IDE 用它生成事件代码；button/edit/static/listbox/combobox 等标准控件类带 `CS_DBLCLKS`，双击消息能到子类化过程；收不到的控件用右键菜单兜底） |
| `cv.form` | **窗体设置** `{text; w; h}`（设计坐标=逻辑像素；DSG 头 `win.form(text=..;right=..;bottom=..)` 的来源。改它走 `applyFormProps`） |
| `cv.selectForm()` / `cv.applyFormProps(props)` | 选中窗体本身（点画布空白处 / 「设计」菜单「窗体设置」）/ 把属性格改动写回 form 状态 |
| `cv.snap` | **对齐网格**（默认 true）：拖动/缩放时坐标吸附到 GRID_SIZE 整数倍；属性格的精确值不吸附 |
| `cv.margins` | 边距参考线 `{left;top;right;bottom}`（设计坐标；null=不画） |
| `cv.onContextMenu = function(item, x, y){}` | 右键（x,y 屏幕坐标） |
| `cv.applyProps(item)` | 把属性/几何回写到控件 |
| `cv.toDSG()` | 生成 `/*DSG{{*/ ... /*}}*/` 文本 |
| `cv.fromDSG(text)` | 解析并载入 |
| `cv.layout(l,t,r,b)` | 重新摆放底板与所有组件 |
| `cv.show(flag)` | 显示/隐藏画布 |

交互要求：
1. 画布是**主窗体上的一块自绘底板**（不是子窗体：`mode="child"` 会带标题栏，
   `border="none"` 会变成弹出窗口）。
2. 组件是**真实控件**（`winform.add` 到主窗体），所见即所得。
3. 选中框与 8 个手柄画在**底板**上（手柄放组件外侧、选中框外扩 1px，否则被组件盖住）。
4. 拖动/缩放用**屏幕坐标**算增量（控件在拖动过程中会跟着鼠标走，lParam 会漂移）。
5. **底板重绘后必须把组件区域再刷一遍**（`__refresh()` 里对每个组件
   `invalidate()` + `UpdateWindow`）—— 底板是 `plus` 自绘板，内容直绘到窗体背景上，
   绘制顺序在子控件之前，底板一重绘就会盖掉组件的像素，而组件不会自动重绘。
   不补这一步的现象是「画布上只看得见网格，看不见控件」。
6. 摆放组件时用 `..win.setPos(..., 0/*_HWND_TOP*/, 0x10/*_SWP_NOACTIVATE*/)`
   提到底板之上 —— 官方 `win.ui` 的 `add` 只在**同一次调用内部**按 `z` 排序，
   跨调用只按创建顺序，层次容易反。
7. 组件的 `text` 属性**建完控件要再写一次**（官方控件建的时候不吃 `tParam.text`），
   否则画布上的按钮是个空壳，属性格里却有标题。

### 8.1 事件代码生成与设计 ↔ 源码同步（app.designer，switchActiveForm 语义）

设计器与源码的关系是**同一份东西的两个视图**（官方 `switchActiveForm` 的语义）：

| 动作 | 行为 |
|---|---|
| 打开设计器（切到设计视图） | 从**当前文档**读 `/*DSG{{*/ … /*}}*/` 块 → 画布；没有块的文档给空画布 |
| 关闭设计器（切回代码视图） | 把**当前设计**写回文档的 DSG 块（**只写编辑器缓冲不落盘**，落盘交给 Ctrl+S） |
| 双击组件 / 右键「查看事件代码」 | 切回代码视图，源码里出现（或跳到）它的事件处理函数 |

两个方向都只在「内容真的不一样」时才动文档；画布空**且**文档本来没有块时
不写 —— 不能往普通脚本里塞空设计；同步是**替换**不是追加（块只有一份）。

事件名按官方控件库的默认事件（对照 `D:\aardio\lib\win\ui\ctrl\*.aardio`
的 `_commandFuncnames` 与事件成员文档）：

| 控件类 | 默认事件 | 桩 |
|---|---|---|
| button / checkbox / radio / groupbox | `oncommand` | `winform.<name>.oncommand = function(id,event){ }` |
| combobox / listbox | `onSelChange` | `winform.<name>.onSelChange = function(id,event){ }` |
| edit / richedit | `onChange` | `winform.<name>.onChange = function(){ }` |
| listview / treeview / tab | `onnotify` | `winform.<name>.onnotify = function(id,code,ptr){ }` |

- 自定义组件在 registry spec 里自带 `event` 字段即可覆盖；没有的类
  双击时提示「没有默认事件」，不生成。
- **已有同名处理函数只跳转、不重复生成**（官方同款行为）；
- 插入点：有 DSG 块插在块结束标记后面，没有就追加到文件末尾；
  光标直接落到函数体那一行。
- 生成入口走 `app.designer.hooks.onEventCode`（设计器不认识编辑器；
  钩子必须收进**表**——模块级裸标量会被帧局部副本遮蔽，见 §1.20.3）。

### 8.2 官方 ide.customCtrl 替身映射（unknown 类怎么画）

`nodio.designer.registry.customCtrl` 照抄官方 `lib/ide/customCtrl.aardio`
（radio/vlistview/close/ipaddress/textbox/custom.button/custom.edit/
custom.checkbox/custom.radio/custom.trackbar）。画布 `__findSpec` 的解析顺序：

1. **注册表精确匹配** —— Nodio 能直接画的类原样画（如 radio，虽然官方
   也把它映射到 radiobutton，但 Nodio 自己就能画）；
2. **映射表命中** → 借替身类的属性表，但 **DSG 仍写真实类名**
   （`spec.cls` 保持原类、`spec.drawAs` = 替身类）——官方工程的
   `textbox` 画成 edit，写回的 DSG 官方 IDE 也认；
3. 都没有 → 兜底原样画（drawAs = cls，属性表为空）。

### 8.3 窗体设置（DSG 头）

- 画布空白处单击 = 选中**窗体本身**（`selectForm`）：属性格给
  标题/宽度/高度三行，改动走 `applyFormProps` 写回 `cv.form`；
- `toDSG()` 把 `cv.form` 生成到 DSG 头 `win.form(text=..;right=..;bottom=..)`，
  `fromDSG()` 解析回头（属性顺序不保证，各自独立匹配）；
- 没有设计块的文档（普通脚本）打开设计器时 form 回默认
  （窗体/759/469）——不带走上一个文档的窗体尺寸；
- 属性格的精确值**不吸附网格**；吸附只发生在拖动/缩放手势上。

回归：`tests/ide_smoke.ps1` 的 `EVENT-STUB / EVENT-STUB-DUP / EVENT-CODE-VIEW /
SYNC-ITEMS / SYNC-WRITEBACK / SYNC-BLOCKS / CUSTOMCTRL-MAP / CUSTOMCTRL-DRAW /
FORM-SET / SNAP`。

## 9. `nodio.designer.dsg` — DSG 解析/生成

| API | 说明 |
|---|---|
| `nodio.designer.dsg.parse(text)` | 解析 `/*DSG{{*/ ... /*}}*/` → item 数组 `{name; cls; props}` |
| `nodio.designer.dsg.generate(items)` | 生成 DSG 文本（含 `/*DSG{{*/`、`/*}}*/` 包裹） |

## 10. 通用约定

1. 组件不抛未捕获异常；失败时返回 `null` 并保留错误串到 `err` 字段。
2. 组件不直接依赖 IDE 私有代码，只能依赖 `win.ui` / `win.subclass` / 官方库。
3. 所有几何量用**客户区坐标**（相对父窗体）。
4. 组件函数命名小驼峰，与官方库风格一致。
5. 每个组件模块必须能被无头脚本导入并断言（见 `tests/`）。
6. **尺寸常量按逻辑像素写，用 `nodio.ui.px()` 换算**（见 §0.6）；
   传入组件的矩形是物理像素；设计画布里的组件矩形是逻辑像素。
7. 文字用 `bd.format`（`nodio.ui.boardFormat()`：垂直居中 + 超长省略号截断），
   不要用 `null` 格式 —— 默认格式是左上对齐且不截断。
   `bd.format` 已带 `_StringFormatFlagsNoWrap`（0x1000）：不加这个标志时
   GDI+ 会把长文本折成多行，固定行高的列表/树里会上下串行。
8. **字面包含判断用 `string.indexOf`，不要用 `string.find`** ——
   官方 `string.find` 是**模式匹配**（正则），`string.find(s,".")` 永远命中。
9. **aardio 里空串是真值**（只有 null/false 为假），
   `if(!s) continue` 拦不住 `""` —— 要用 `#s == 0` 判长度。
10. **读 UTF-8 文本文件用 `string.load(path)`**，不要用 `io.readFile(path)`：
    官方 `io.readFile = file.read`，读回来的是**原始字节**（每字节一个字符），
    中文源码会整篇乱码；`string.load` 按 UTF-8 解码（还认 BOM）。
11. **自绘控件之间的 z 序要显式定**（`SetWindowPos(HWND_TOP/HWND_BOTTOM)`）。
    官方 `form.add` 一次只加一个控件，层次就等于添加顺序，谁先加谁在上面 ——
    全屏背景板会压住所有面板，鼠标事件全被吃掉。
12. **拖动窗口的增量用 `win.getCursorPos()`**，不要用控件坐标：拖动时窗口跟着
    光标走，控件内坐标不变，从控件坐标算增量永远是 0。
13. **去掉窗口标题栏要在 `show()` 之后**，并且**最大化时不要接管
    `WM_NCCALCSIZE`**（接管会把内容顶出屏幕）。
14. **`for(k,v in 表)` 只遍历表的「数组部分」** —— 字符串键的表用这种写法
    **一个元素都遍历不到**（按钮画不出来、也点不到）。要么改成数组 +
    `for(i=1;#t;1)`，要么用 `for(k in t)` 只取键。
15. **`'\0'` 是字节转义**（带它的字面量是「字节串」），跟普通字符串拼接会把
    整串降级成字节串（中文被按 UTF-8 编码成字节）。要 NUL 又要保持普通字符串，
    用 `'\u0000'`。
16. **自绘控件的坐标一律用客户区相对坐标**（绘制原点与鼠标命中测试给的
    都是客户区坐标）；父窗口绝对坐标会把图元画到控件外面。

## 11. `nodio.ui.editor` — 代码编辑器组件

编辑器本体（`lib/nodio/ui/editor.aardio`，约 37KB）。底座是官方 `richedit`
（选区/剪贴板/IME/撤销白送），组件负责标准 IDE 该有的那一圈东西。

### 11.1 对外 API

| API | 说明 |
|---|---|
| `nodio.ui.editor(winform, opt)` | 建组件。`opt.ctrl` 复用已有 richedit（IDE 复用 DSG 里的 `ed`）；`opt.fontSize/fontName`；`opt.rect` 初始矩形；`opt.gutter=false` 关掉行号栏 |
| `ed.setText(s)` / `ed.getText()` | 读写文档（内部一律 CRLF） |
| `ed.getTextForSave()` | 按文档自己的换行符还原后返回（保存用） |
| `ed.setEOL(eol)` / `ed.eol` | 换行符（`'\r\n'` / `'\n'`） |
| `ed.normalize(s)` | 把任意文本规范成 CRLF（文档模型统一用它） |
| `ed.isDirty()` / `ed.markSaved()` / `ed.setSavedText(s)` / `ed.markDirty(v)` | 脏标记（**由内容推导**，见 §11.4） |
| `ed.undo()` / `ed.redo()` / `ed.canUndo()` / `ed.canRedo()` | 撤销/重做（组件自己的快照栈，见 §11.5） |
| `ed.cut/copy/paste/selectAll()` | 剪贴板与全选（走 WM_COPY/WM_CUT/WM_PASTE，不读 `selText`） |
| `ed.indent()` / `ed.unindent()` / `ed.toggleComment()` | 行级编辑命令（按行取内容 + 按行替换） |
| `ed.findNext(word, fromCaret)` / `ed.findPrev(word)` / `ed.replaceOnce(word, repl)` / `ed.replaceAll(word, repl)` | 查找替换（位置单位 = UTF-16 码元） |
| `ed.showFind(withReplace)` / `ed.closeFind()` / `ed.findText()` / `ed.replaceText()` | 查找替换浮条 |
| `ed.gotoLine(line)` / `ed.gotoPos(pos)` / `ed.updateCursor()` | 定位；返回 `line, col` |
| `ed.setDiagnostics(list)` | 槽内诊断标记（波浪线 + 错误色）+ 行号栏诊断圆点。`list` 每项 `{line=1基; col=0基; endLine; endCol; msg; sev}`，`sev` 1=错误 2=警告 |
| `ed.toggleFold(line)` / `ed.foldAll()` / `ed.unfoldAll()` | 代码折叠（见 §11.7） |
| `ed.isFoldable(line)` / `ed.isFolded(line)` | 某行是不是折叠头 / 是不是已折叠 |
| `ed.requestComplete()` | 触发代码补全（算前缀 → 调 `onComplete` 回调，见 §11.8） |
| `ed.showCompletion(items)` | 展示候选（`items` 每项 `{label; kind; detail}`；`kind` 是 LSP CompletionItemKind） |
| `ed.hideCompletion()` / `ed.completionVisible()` | 关掉 / 查询候选浮层 |
| `ed.completionMove(delta)` / `ed.completionAccept()` | 上下移动选择 / 插入选中候选 |
| `ed.onDefinition` / `ed.onReferences` / `ed.onRename` | 导航回调 `function(line, col, word)`：`line` 1 基行号、`col` 1 基**列**（单词首字符）、`word` 光标处的标识符（见 §11.9） |
| `ed.gotoLineCol(line, col)` | 跳到指定行（1 基）+ 列（1 基），跨文件跳转的落点 |
| `ed.replaceTextAt(text, line, col, len, newText)` | 在**文本**里按 (行, 列, 长) 替换一段，返回新文本；列/长是 1 基 UTF-16 码元（见 §11.9） |
| `ed.layout(l,t,r,b)` / `ed.gutterWidth()` / `ed.control()` | 布局与访问底层控件 |
| 回调 | `onCursor(line,col)` / `onChange(text)` / `onDirty(dirty)` / `onGotoLine()` / `onComplete(line,col)` |

### 11.2 位置单位（最容易踩的坑）

- richedit 的位置 = **UTF-16 码元**（1-based，CRLF 占 1 个码元）
- `..string.len(s)` = **wchar 码元数**（与 richedit 同单位）
- `#s` / `s[i]` / `string.sub` = **UTF-8 字节**
- 行首位置**用组件自己的行首表** `__lineStart(line)`（不是官方 `lineToChar`）：
  折叠后 `EM_LINEINDEX` 对隐藏行返回的位置是错的（见 §11.7）；
  行内偏移用 UTF-8 首字节推算的码元数

### 11.3 官方 richedit 的几个实测行为

- `getSel()`：**无选区时只返回 1 个值**（0-based 插入点）；有选区返回 2 个值（1-based 闭区间）。
- `setSel(a,b)` 选 1-based 闭区间；`setSel(x)` 单参数把插入点放到 **0-based** 索引 x。
- 控件内部换行是 CRLF，但 `ctrl.selText`（EM_GETSELTEXT）返回**只有 CR** 的文本，
  而且**读完会破坏选区** → 行级编辑不能靠选区文本切行。
- `EM_POSFROMCHAR`（0x426）**把坐标打包在返回值里**（低 16 位 x，高 16 位 y），
  不往 lParam 指针写 → 行高 = y(第2行) − y(第1行)。
- 控件级事件路由不通（`EN_CHANGE` 收不到）→ 变更检测用 250ms 轮询比对文本。

### 11.4 脏标记是**推导**出来的

`dirty = (当前文本 != 上次保存的文本)`，不是开关。好处：撤销回保存时的状态会
自动变回干净。文档模型里每个页签各存一份「保存时的文本」，切页签时
`setSavedText()` 交给组件。

⚠ 两边必须**同形**：文件里的 LF 文本要先 `normalize()` 成 CRLF 再存，
否则「刚打开就显示为已修改」。

### 11.5 撤销栈是组件自己实现的

**richedit 自带的撤销栈只收键盘输入**：程序化的 `EM_REPLACESEL`
（缩进/注释/替换全都走它）**不进撤销栈** —— 实测改完文本 `EM_CANUNDO` 仍是 0，
`EM_UNDO` 什么也不做；而且 RichEdit 根本不实现 `EM_CANUNDO`（那是 edit 控件的）。

所以组件自己存文本快照：

- 键盘输入：在 `WM_CHAR` / Delete / 粘贴 / 剪切**之前**存（撤销粒度 = 一次按键）
- 程序改动：改之前存（`__beginEdit`）
- 轮询发现「没存过快照的改动」：补一个
- 栈深 200，撤销/重做各一条栈

### 11.6 浮条画板的三个坑（都实测过）

1. **plus 控件默认把父窗口背景整块拷进自己的画板**。浮条浮在 richedit 上方时，
   父窗口背景在 richedit **之下** → 自绘内容被 richedit 盖住（浮条一片空白）。
   解法：建板时 `transparent=true`（改用自己的画板绘制）。
2. **画板绘制的原点在客户区**，鼠标命中测试给的也是客户区坐标。
   按钮矩形必须用**客户区相对坐标**；用父窗口绝对坐标会把按钮画到画板外面
   （浮条只剩左边的标签，按钮全没了）。
3. **重绘会把画板提到 z 序最前** → 必须排在摆输入框**之前**，
   否则输入框被画板盖住（只剩一个输入框，浮条本体看不见）。
   另外官方 ctrl 没有可用的 `redraw` 成员（调用抛「非函数值」），
   整块重绘用引擎 sysapi：`::InvalidateRect(hwnd,0,1)` + `::UpdateWindow(hwnd)`。

### 11.7 诊断标记与代码折叠（都靠 CHARFORMAT2 的原始位）

两个功能都**不用**官方 `richedit.setCharformat`，而是组件自己构造
`CHARFORMAT2` 结构体、直接发 `EM_SETCHARFORMAT`（0x444）。
两个原因（都实测）：

1. 官方 `setCharformat` 的 `mask` **只加 `CFM_UNDERLINE`，从不加
   `CFM_UNDERLINETYPE(0x800000)`** → 控件按规范忽略 `underlineType`
   → **永远只画单线，出不来波浪线**；
2. 官方用 `if(textColor)` 判断要不要设颜色，而 **aardio 里 0 是真值**
   （只有 null/false 为假）→ 只传 `underline` 也会把选区文字色刷成 0（黑）。

原生 RICHEDIT50W 的 `CHARFORMAT2W` 真实布局 == aardio 官方 `CHARFORMAT2` 类
（`textColor@20 / bUnderlineType@112`，含 `pad` WORD，total = 116），
组件在 `lib/nodio/ui/editor.aardio` 里自己定义了一份。

**诊断标记**（`setDiagnostics`）：

- 波浪线：`mask = CFM_UNDERLINE|CFM_UNDERLINETYPE|CFM_COLOR`，
  `effects = CFE_UNDERLINE`，`underlineType = 8`（`CFU_UNDERLINEWAVE`）；
- 清标记：`mask` 只带 `CFM_UNDERLINE|CFM_UNDERLINETYPE`（**不带 CFM_COLOR**）
  → 只清下划线，不碰文字色；
- 行号栏：错误行画圆点 + 行号染成诊断色；
- 诊断集合带**指纹判重**：IDE 心跳每 300ms 喂一次诊断，
  不判重的话编辑器每 0.3 秒全文重刷一次。

**代码折叠**：

- 折叠靠 `dwEffects` 的 **`CFE_HIDDEN(0x100)`**：给正文区打 hidden 格式，
  控件就把这些段落**整个收起（连行高都没有）**；隐藏的只是显示属性，
  文本本身不丢，保存出去永远是完整文本；
- 隐藏区间要**连最后一行行尾的 CRLF 一起盖住**，否则折叠后留一条空行；
- ⚠ **折叠后官方 `lineToChar` / `lineFromChar` / `lineRange` 返回错的位置**
  （`EM_LINEINDEX` 对隐藏行返回隐藏行自己的起点）→ 组件自己维护
  `lineStarts` / `lineLens`（文档行 → 字符位置）和 `visLines`（可见文档行），
  行号栏按 `visLines` 画；
- 行号栏宽 64 逻辑像素：左诊断圆点、中行号、右折叠标记（▸ 已折叠 / ▾ 展开，
  自己用矩形拼的实心小三角，不依赖字体字形）；点标记即折叠/展开；
- 折叠区按 aardio 花括号配对识别（字符串/注释里的花括号不算）。

### 11.8 代码补全浮层

**分工**：组件负责「算前缀 / 画浮层 / 键盘导航 / 插入」，
IDE 负责「问 LSP 要候选」——两边用 `onComplete(line,col)` 出、
`showCompletion(items)` 进两个回调解耦，换后端不用改组件。

| 环节 | 做法 |
|---|---|
| 前缀 | 光标处往左扫 ASCII 标识符（`__wordAtCursor`）；返回 `{word; line; col; start}`，`start` 是 1-based 的 richedit 位置 |
| 过滤 | `showCompletion` 里用 `string.startsWith(label, prefix, true)`（忽略大小写）筛；**一个都不匹配就退回全量**（我们的 LSP 给的是全量候选） |
| 浮层 | 自绘 plus 画板（`transparent=true`，z=70），宽度跟着最长候选走（170~340 逻辑像素），最多 8 行、超出滚动（`compScroll` 保证选中项在可见区） |
| 键盘 | `Ctrl+Space` 请求、`↑↓` 选择、`Enter/Tab` 插入、`Esc` 取消 |

三个必须记住的坑：

1. **`Ctrl+Space` 的请求不能放在窗口过程里**：`onComplete` 会同步问 LSP
   （最多 1 秒），在按键处理里阻塞会卡住窗口过程 → 用
   `__scheduleComplete()`（`setTimeout(...,1)`）推迟到下一轮消息循环。
2. **吃掉按键还不够，WM_CHAR 也要跟着吃**：`TranslateMessage` 早一步就把
   `WM_KEYDOWN` 翻译成 `WM_CHAR` 投回控件 → 只吃 KEYDOWN 的话，
   Tab 会「既缩进又插制表符」、Ctrl+Space 会「多出一个空格」。
   组件用 `__eatChar` 记住刚吃掉的键的字符码，下一个 WM_CHAR 若是它就一起吞。
3. **`string.sub(s,start,end)` 的第三个参数是结束位置（含），不是长度** ——
   按长度写会得到空串（前缀算出来永远是空的）。

### 11.9 代码导航（F12 / Shift+F12 / F2 / Ctrl+点击）

组件只负责**识别意图 + 报出位置**，跳转和查询由 IDE 做
（IDE 要同步问 LSP，不能在窗口过程里做）。

| 回调 | 触发 | 签名 |
|---|---|---|
| `ed.onDefinition` | `F12`、`Ctrl+点击` | `function(line, col, word)` |
| `ed.onReferences` | `Shift+F12` | `function(line, col, word)` |
| `ed.onRename` | `F2` | `function(line, col, word)` |

`line` 是 1 基行号，`col` 是**单词首字符**的 1 基列号，`word` 是光标处的标识符。

**取词用的是 `__wordAtPos`，不是 `__wordAtCursor`**：
补全要的是「光标**前面**那段前缀」，而点击可能落在单词**开头**
（此时前缀是空的）或单词**中间**。`__wordAtPos` 改成**夹住**位置 ——
先看光标前面那个字符，不是标识符再看光标后面那个，然后向左右扩展成完整单词。

**两个和 LSP 换算有关的 API**：

| API | 干什么 |
|---|---|
| `ed.gotoLineCol(line, col)` | 落点。LSP 给的列是 **0 基 UTF-16 码元**，编辑器内部是 **1 基** |
| `ed.replaceTextAt(text, line, col, len, newText)` | 跨文件重命名时改文本。aardio 字符串是 **UTF-8 字节**，LSP 的列是 **UTF-16 码元** —— 单位不同，必须换算（否则中文标识符旁边就错位） |

`replaceTextAt` 内部用 `string.unpack` 拿字节数组，再用 `__byteOfCol`
（UTF-16 码元列 → 字节偏移）定位，替换后用 `string.join` 拼回去 ——
**按 `\n` 切行时每行还带 `\r`**（CRLF 文件），`string.split` 保留分隔符以外的
全部内容，所以拼回去还是原来的换行符。

> ⚠ `string.left(s,n)` 是「前 n 个字符」，**去掉前缀要用 `string.slice(s,k)`**；
> `string.replace` 返回**两个**值（结果 + 替换次数），直接 return 会把次数漏出去。

## 11.10 导航栏（面包屑）

VS 的导航栏长在**编辑区正上方**：跟着光标显示
「工程 › 目录 › 文件 › 命名空间 › 函数」，点哪一段就跳到哪一段。

组件只负责**画 + 命中 + 上报**，内容由 IDE 算好塞进来。

| API | 说明 |
|---|---|
| `ed.setNavChain(chain)` | 设置分段。`chain` 是段数组：`{text; kind; line; col; path}`，`kind` = `"dir"` / `"file"` / `"symbol"` |
| `ed.onNavClick` | `function(seg)` —— 点了某一段（IDE 决定怎么跳） |
| `ed.navHeight()` | 导航栏高度（物理像素），布局要给它让位 |

### 三个坑（都实测过）

1. **必须用 `redraw()`，不能用 `invalidate()`**。
   `invalidate()` 只置脏位，更新区域（`rcPaint`）可能是残缺的 ——
   GDI+ 按裁剪区把自绘内容裁掉一块。表现是**导航栏永远停第一次画出来的样子**
   （内容更新了但画面不变）。`nodio.ui.board` 的 `redraw()` 走
   `InvalidateRect(整块) + UpdateWindow` 才是整块重绘。

2. **导航栏占了编辑区顶部一条，查找条与补全浮层要跟着下移**。
   `__layoutFind` 里 `y = r.top + navHeight() + px(8)`；
   补全浮层的 `__layoutCompletion` 里绝对 y 要加 `navHeight()`
   —— `__posOf` 给的是 **richedit 客户区**坐标，而 richedit 摆在导航栏下面。

3. **层级链必须在 IDE 侧本地算**。
   符号树（`documentSymbol`）只在文档变化时向 LSP 拉一次；
   光标移动只做纯内存遍历 —— 每动一下都同步问一次 LSP 会把编辑器卡死。

### 层级链的判定是「包含」不是「在上方」

`line <= 光标行 <= endLine` 才算在符号里面。只看起始行的话，
光标停在一个**刚结束的函数之后**的顶层语句上时，会被误算成
「还在那个函数里」。所以服务端的 `documentSymbol` 必须带 `endLine`
（由 AST 子树的最大行号算出，见 `docs/capability-gap.md` §1.20）。

变量（kind 13）**不是作用域**，不参与面包屑。

## 12. `nodio.ui.filedlg` — 系统文件对话框（打开/另存为）

| API | 说明 |
|---|---|
| `nodio.ui.filedlg(winform, opt)` | `opt.owner`（父窗体）、`opt.filter`、`opt.defExt` |
| `dlg.open(filter, title, dir, defFile)` | 打开文件，返回路径或 `null` |
| `dlg.save(filter, title, dir, defFile)` | 另存为，返回路径或 `null` |
| `dlg.dir` | 上次使用的目录（记住） |

过滤器格式：`"描述|通配符|描述|通配符|"`（竖线分隔成对），内部转成 Windows 要的
`描述\0通配符\0…\0\0`。

### 12.1 为什么自己手写 OPENFILENAMEW

官方 `fsys.dlg` 的 `OPENFILENAME` 在 Nodio 上算出 **144 字节**，而 Windows 的
`OPENFILENAMEW` 是 **152 字节**：Windows 里 `hwndOwner` 是 `HWND`（8 字节），
官方声明成了 `INT`（4 字节），后面所有字段整体错位 8 字节 →
`GetOpenFileName` 读到的全是垃圾，**立刻返回 FALSE**（实测：秒退、返回 null）。

组件里按 Windows 的真实偏移逐字段排好（见 `OFN` 的注释），实测对话框正常弹出。

### 12.2 NUL 必须用 `'\u0000'`

aardio 的 `'\0'` 是**字节转义**：带它的字面量是「字节串」，跟普通字符串拼接会把
整串降级成字节串（中文被按 UTF-8 编码成字节），再做 UTF-16 转换就成乱码
（实测过滤器下拉框显示乱码）。`'\u0000'` 不是字节转义，得到的仍是正常字符串。

UTF-16 缓冲区统一用 `..raw.buffer((..string.len(s)+1)*2, ..string.toUtf16(s))`
造（`string.len` 是 wchar 数，字节数 = 2 倍）。

## 13. `nodio.ui.prompt` — 输入提示条

| API | 说明 |
|---|---|
| `nodio.ui.prompt(winform, opt)` | `opt.title` |
| `p.show(title, defText, onOk)` | 显示；`onOk(text)` 返回 `false` 表示校验没过、留在原地 |
| `p.close()` | 关闭（触发 `onCancel`） |
| `p.layout(l,t,r,b)` / `p.layoutCentered(pl,pt,pr,pb)` | 布局 |

用途：转到行（Ctrl+G）、以后接重命名/新建文件也很自然。
不用官方 `win.inputBox`：那套走 `win.form` 的 `doModal/endModal`，
在 Nodio 上没验证过（模态 + 树遍历解释器很容易重入出问题）。

⚠ 与 §11.6 同源：提示条是浮在 richedit 上面的，`transparent=true` 必设，
按钮矩形用客户区相对坐标。

## 14. `nodio.ui.searchPanel` — 工程全文搜索面板（左栏「搜索」页）

| API | 说明 |
|---|---|
| `nodio.ui.searchPanel(winform, opt)` | `opt.rect` |
| `p.setScope(root, files, texts?)` | 搜索范围：`files` = `{path=相对路径; full=绝对路径}` 数组；`texts` 可选（`full` → 内存里的文本，打开中的文档用） |
| `p.search(keyword)` | 逐文件读文本、逐行找关键字（**不区分大小写**），返回命中数 |
| `p.keyword()` | 输入框里的当前关键字 |
| `p.onOpenFile(path, line)` | 双击结果 → 打开文件并跳到那一行 |
| `p.onStatus(text)` | 状态栏提示（"找到 N 处（搜索 M 个文件）"） |
| `p.focus()` / `p.show(flag)` / `p.layout(l,t,r,b)` | 焦点 / 显隐 / 布局 |

**为什么用自绘列表**：原生 listview 的配色消息在本机 comctl32 里会把 lParam 当指针解引用 →
进程崩 0xC0000005（§1.11.4），深色主题做不了。结果表走 `nodio.ui.list`。

**dock 只认一个 ctrl**：面板里有三个控件（输入条画板 + 输入框 + 结果表），
`self.ctrl` 给的是一个**可见性代理**（只有 `show()` 的普通表），
dock 的 `__applyVisibility` 调的正是 `ctrl.show(flag)` —— 这样页签切换时三者一起显隐。
面板自己实现 `layout(l,t,r,b)`，dock 认得 `{ctrl=..; layout=fn}` 这种自绘组件。

### 14.1 三个坑（都实测过）

1. **`string.find` 是正则**，判断"这一行有没有关键字"必须用 `string.indexOf`
   （1-based，找不到返回 null）。`string.lower` 两边都小写即可不区分大小写。
2. **按 `\n` 切行后每行还带 `\r`**（CRLF 文件）：预览列会出现一个看不见的回车，
   末尾还会把单元格高度算歪。用 `string.endsWith(line,'\r')` 判断，
   再 `string.sub(line, 1, #line-1)` 去掉。
   ⚠ `#line` 是 **UTF-8 字节数**，`string.sub` 的位置参数也是字节位置 —— 两者一致。
3. **双引号是原样串**：`"D:\Nodio"` 里的反斜杠**原样保留**（不处理转义），
   而单引号串里 `"\t"` 是 TAB、`"\\"` 才是**一个**反斜杠。
   路径要写成 `"D:\Nodio\ide\nodio"`（双引号，一个反斜杠）或者 `'D:\\Nodio'`（单引号，两个）。
   写混了目录就建不出来（`\t` 变成 TAB）。

### 14.2 搜索范围怎么来

IDE 侧（`main.nodio`）在工程树刷新时一并收集：工程根目录下的**文本文件**
（按扩展名白名单过滤，二进制搜了没意义还刷屏），再交给 `setScope`。
打开中的文档可以用 `texts` 覆盖成内存里的版本（未保存的改动也能搜到）。

## 15. `app.lspClient` — 语言服务客户端

LSP 走 `Content-Length` 分帧的 JSON-RPC over stdio（`apps/nodio-lsp.exe`）。

### 15.1 公开 API

| API | 说明 |
|---|---|
| `start(exePath, filePath, docText)` | 起服务 + `initialize` + 首次 `didOpen`；返回 `"ok"` / `"spawn-failed"` |
| `openDoc(path, docText)` | 切换**当前文档**（首次 `didOpen`，之后 `didChange`） |
| `pushDoc(path, docText)` | 推送某个文件的最新内容，**不**改当前文档（重命名后回灌索引用） |
| `didChange(docText)` | 当前文档内容变了 |
| `setProject(root, files)` | 告知工程根目录 + 源文件清单（建立项目级符号索引） |
| `completion(line, character)` | 候选；`line`/`character` 是 **0 基** |
| `hover(line, character)` | 悬停文本 |
| `symbols()` | 当前文档的**层级**大纲：`[{name; kind; line; endLine; col; children}]` |
| `definition(line, character)` | 转到定义 → `{path; line; col; len}`（`line` 1 基，`col` 0 基） |
| `references(line, character)` | 查找引用 → 同上 |
| `rename(line, character, newName)` | 重命名 → 同上（要改的位置清单） |
| `workspaceSymbols(query)` | 项目符号搜索 → `{name; kind; path; line; col}` |
| `getDiags()` | 当前文档的诊断 |
| `stop()` / `isRunning()` | 收工 / 状态 |

### 15.2 五个必须记住的坑

1. **`files` 必须是数组表**：`JSON.stringify` 只把带 `_type="array"` 的表
   当数组输出，普通表会被写成对象。构造方式：
   ```aardio
   var files = [ @{_type="array"} ];
   ..table.push(files, "app/shell.nodio");
   ```
2. **`uriToPath` 去掉前缀要用 `slice(s,9)`**：`string.left(s,n)` 是
   「前 n 个字符」，用 `left(s,#s-8)` 会把**尾巴**切掉。
   另外 `string.replace` 返回**两个**值（结果 + 替换次数），
   直接 `return` 会把次数漏出去。
3. **多文档要按 uri 分别记 `didOpen`/`didChange`**：只有一个 uri 的话，
   切标签页后补全/诊断/定义查询全都作用在旧文件上。
4. **编辑器缓冲是 CRLF，磁盘文件常常是 LF**。两者都要能正常解析 ——
   服务端的 JSON 解码器必须**完整实现 JSON 转义**（`\r` 尤其关键，
   漏掉它会把每个换行变成一个 `r` 标识符，IDE 于是对自己的源码报假语法错误；
   见 `docs/capability-gap.md` §1.19）。**不要**在客户端另存一份 LF 副本，
   那会让编辑器文本、服务端索引、诊断行列三者对不上。
5. **服务端回包把非 ASCII 转义成 `\uXXXX`**：`Content-Length` 是**字节数**，
   而 aardio 侧 `string.mid` 的长度参数是**字节上限**、起始却是**字符序号**，
   两者混用时原始 UTF-8 字节会让帧长度算不准。转义成纯 ASCII 后
   「字节数 == 字符数」，一个数字就够（`apps/nodio-lsp/main.cpp` 的 `JStr`）。
   客户端 `JSON.parse` 能解开 `\uXXXX`（实测通过）。
6. **`documentSymbol` 的位置在 `range`/`selectionRange` 里，没有 `location`**。
   老代码读 `v.location.range.start.line` → 行号恒为 0（整棵树都没有位置），
   而且把 `children` 丢掉了 —— 导航栏面包屑的层级链就靠它。
   现在 `symbols()` 返回**层级树**，位置取 `selectionRange.start`
   （`line` 1 基 / `col` 0 基），并额外带回 `endLine`（符号结束行）。

### 15.3 诊断按 uri 分开存

`publishDiagnostics` 通知带 `uri`，客户端按 uri 存进 `diags[uri]`，
`getDiags()` 取当前文档的那一份 —— 多文档切换时诊断不能互相覆盖。

## 16. 底部「引用」面板

底部 dock 新增一页（第 5 页）：`nodio.ui.list`，列是 `文件 / 行 / 说明`。

- **查找引用**（`Shift+F12`）：每行 `{cells={短路径; 行号; 符号名}; tag={path; line; col}}`；
- **项目符号搜索**（`Ctrl+Shift+T`）：同一个面板，`说明` 列放符号名。

双击（`onDoubleClick`）→ 打开文件（没开就顺手打开）→ `gotoLineCol` 落点。

> 「引用」和「符号」共用一个面板是有意的：两者都是「一堆位置，双击跳过去」，
> 形状完全一样，分两个页签反而增加记忆负担。
> ⚠ 行号栏/列表里显示的**行号是 1 基**，LSP 给的是 0 基 —— 转换点只有一处
> （`main.nodio` 的 `navFindReferences` / `navGotoDefinition`）。