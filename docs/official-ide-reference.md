# 官方 aardio IDE 参考事实（从 D:\aardio 提取）

提取日期：2026-10-19。来源优先级：官方库**源码** > 官方 `config` 数据 > `aardio.exe` 二进制字符串。
本文件是 Nodio IDE 实现的对照依据；凡与 `docs/ide-components.md` 冲突，以本文件的"官方事实"为准。

## 0. 外观基准（2026-10-20 更新）：现代 aardio IDE

> **外观基准已更换。** 之前以官方老 IDE（MFC 风格、浅色、系统控件）为外观参照；
> 现在以 **aar.chengxu.online/thread-903.htm** 上那套**现代 aardio IDE**
> （aardioX / QuickEditor4，帖子 `dgczy` 于 2026-10-19 发布）为参照。
> 理由：用户明确要求「IDE 应该抄这个」。
> 下面的**能力面**（命令表、DSG 格式、窗体模型）仍然以官方老 IDE 为准，
> **只有外观**改用新基准。

参考图（帖子正文，共 17 张；本地留档见 `docs/ref/aar-ide/`）：

| 图 | 内容 |
|---|---|
| 1792403417 | 窗体设计器 + 属性编辑器 + 控件面板（**浅色**主题） |
| 1792403492 | 工程管理器 + 代码编辑器 + 双栏对比 + 问题列表 + 语法（**深色**主题，默认） |
| 1792403494 | 窗体设计器、代码编辑器双栏 + 代码大纲 + 问题列表 |
| 1792409655 | 运行 & 错误输出 |
| 1792403496 | 可视化调试 & 断点调试 |
| 1792403499 | AI 助手 & 文件大纲 & 输出 |
| 1792403500 | 可视化 Git & Git 改动对比 & 工具 |
| 1792403764 | 工程文件搜索、跳转 & 命令终端 |
| 1792403766 | 文件时间线 & 标准库 |

### 0.1 外观规格（从截图逐条抄下来的实现依据）

**整体**：VS Code 风格、扁平、深色为主（帖子右上角有「深色（默认）」主题下拉，
另有「几十种外观主题」）。顶栏是**自绘标题栏**：应用图标 + 菜单
（文件/工程/编辑/选择/格式/转到/查找/显示/运行/视图/设置/帮助）+ **居中**的
工程/文件标题 + 右侧最小化/最大化/关闭。下面一排**纯图标**工具条（无文字）。

**配色**（深色主题，接近 VS Code Dark+）：

| 用途 | 值 |
|---|---|
| 窗体/编辑区底色 | `#1E1E1E` |
| 面板底色（树/列表/问题面板） | `#252526` |
| 分隔线/描边 | `#3C3C3C` |
| 正文 | `#D4D4D4` |
| 次要文字（分组标题、列头） | `#9D9D9D` |
| 强调（选中标签描边、选中框、焦点） | `#0F6CBD` |
| 选中行 | `#04395E` |
| 悬停行 | `#2A2D2E` |
| 错误 / 警告 | `#F14C4C` / `#CCA700` |

**布局**：左栏（工程/属性/搜索/大纲/时间线/Git）· 中栏（文档页签 + 编辑区，
可**双栏对比**）· 右栏（控件/语法/标准库/范例/工具/AI）· 底栏（问题/输出/调试/终端）
· 底部状态栏。页签条是**扁平**的：选中项 = 稍亮底 + **顶部 2px 强调色描边**（不是底部下划线）。

**代码区**：行号槽 + 折叠箭头 + 语法着色（关键字粉紫、字符串橙、注释绿、函数浅黄）
+ 槽内错误红叉 / 警告黄三角；问题列表按 `行 / 级别 / 说明` 三列。

**设计器**：左属性格按**分组带**（基本/位置/外观/样式…，每带一个折叠箭头），
值列可就地编辑（下拉/颜色块）；中间是设计画布，选中框 + 8 个手柄；
右栏是带图标的控件清单（分组「组件/整体」）。

**状态栏**：左 = 工程名 + git 分支 + 文件路径 + 语言 + 编码 + 换行符 + 错误数；
右 = 行/列/位置/行数。

### 0.2 Nodio 当前的差距（对照上面逐条）

| 项 | 状态 |
|---|---|
| 深色主题 + 配色表 | ✅ 标准库 `nodio.ui.theme`（深色/浅色两套） |
| 扁平页签条（顶部强调描边） | ✅ `nodio.ui.tabstrip` |
| 自绘工具条（深色、无边框、悬停底色） | ✅ `nodio.ui.toolbar`（已从原生 button 改为自绘） |
| 自绘状态栏 | ✅ `nodio.ui.statusbar` |
| 窗体底色 / 编辑区深色 | ✅ 底板自绘 + 官方 richedit 的 `setCharformat`/`bgcolor` |
| 工具箱 / 属性格深色 | ✅ `nodio.designer.toolbox` / `propertyGrid` |
| 工程树 / 问题列表深色 | ❌ **原生 treeview/listview 改不了色**（comctl32 配色消息在本机直接崩，见 capability-gap.md §1.11）→ 要改成自绘 |
| 自绘标题栏（菜单进标题栏） | ❌ 未做（仍是系统标题栏） |
| 语法着色 / 折叠 / 槽内标记 | ❌ 未做（编辑器仍是纯 richedit） |
| 双栏对比编辑 | ❌ 未做 |
| 菜单栏 | ❌ 未做（当前只有工具条按钮） |
| 属性格分组带 | ❌ 未做（当前是平铺两列） |

---

## 1. 官方 IDE 的命令表（aardio.exe 0x003C66BC 起的连续字符串块）

官方 IDE 把可被外部进程/插件调用的能力做成一张**命令名表**，
`ide.command.dispatchIdeCommand(name, ...)` 按名字分发。完整命令名（逐条抄自二进制）：

```
showStatus          gotoLine            processList
pluginViewShow      pluginViewCreate    setConfig
getConfig           createProcessEx     createThread
saveAll             save                insertCode
setSelectedControl  getSelectedControl  setControlParam
getControlParam     switchActiveForm    getActiveForm
loadPrivateFont     redrawCodeEditor    setActiveCode
getActiveCode       newForm             newDocument
openDocument        refreshStdLib       refreshUserLib
projectIsOpened     setProjectProperty  getProjectProperty
getProjectMainFile  getProjectPath      getProjectDir
getMainHwnd         getActiveViewHwnd   updateVersion
isInProject         getProjectIcon      killOnExit
```

**Nodio 落地建议**（自托管 IDE 的 `ide` 命令面）：

| 官方命令 | Nodio 实现 | 归属 |
|---|---|---|
| `openDocument` / `newDocument` / `newForm` | 打开/新建文件与窗体 | IDE |
| `save` / `saveAll` | 保存当前/全部文件 | IDE |
| `setActiveCode` / `getActiveCode` / `insertCode` | 编辑器文本读写与插入 | IDE |
| `gotoLine` / `showStatus` | 跳转行 / 状态栏 | IDE |
| `getSelectedControl` / `setSelectedControl` | **设计器**选中控件 | 设计器 |
| `getControlParam` / `setControlParam` | **设计器**属性读写 | 设计器 |
| `switchActiveForm` / `getActiveForm` | 设计视图 ↔ 代码视图切换 | 设计器 |
| `pluginViewCreate` / `pluginViewShow` | **插件视图**（右侧栏页签）创建/显示 | IDE |
| `getConfig` / `setConfig` | IDE 配置读写 | IDE |
| `projectIsOpened` / `getProjectPath` / `getProjectDir` / `getProjectMainFile` / `setProjectProperty` / `getProjectProperty` / `isInProject` | 工程模型 | app.project |
| `refreshStdLib` / `refreshUserLib` | 标准库面板刷新 | IDE |
| `createProcessEx` / `createThread` / `processList` / `killOnExit` | 外部进程/线程 | app.runner |
| `getMainHwnd` / `getActiveViewHwnd` | 窗口句柄 | IDE |
| `loadPrivateFont` / `redrawCodeEditor` / `updateVersion` / `getProjectIcon` | 杂项 | IDE |

## 2. 插件视图 = 右侧栏页签（官方 aardio 源码）

官方右侧栏的「范例 / 工具 / 添加文件」等页签是**独立 aardio 脚本**，以**子窗体**形式挂在 IDE 里：

```aardio
// D:\aardio\extensions\plugin\pane\tool.aardio
mainForm = win.form(text="工具";right=270;bottom=534;bgcolor=0xFFFFFF;
    border="none";exmode="none";mode="child";parent=...)   // ← 父窗口由 IDE 注入
mainForm.add(
    btnAiChat={cls="plus";text="问 AI";...;iconText='\uF0E6';notify=1;...};
    treeview={cls="treeview";left=0;top=27;right=270;bottom=532;anchors={...};z=1}
)
mainForm.show();
win.loopMessage();
```

关键事实：
1. 页签标题 = 子窗体 `text`（"工具" / "范例"）。
2. 面板宽度固定 **270px**（`right=270`），IDE 的右栏就是这个宽度。
3. 面板用 `mode="child"` + `parent=...`（`...` 是 IDE 注入的父窗口占位）。
4. 面板通过 `import ide` 调用 IDE：`ide.createProcess(filePath,,op,mainForm.hwnd)`、
   `ide.createThread(filePath)`、`ide.openDocument(newPath)`。
5. 面板与 IDE 的双向通道是 `process.command.join("{GUID}.aardio.ide.update")` +
   `process.command()` 监听器（`ide.command` 的同族机制）。

**Nodio 落地**：右栏 = `nodio.ui.dock`（标签条 + 客户区），每页是一个
`nodio.plugin` 面板（同样是 `mode="child"` 子窗体），由 `nodio.plugin.register` 注册，
对应官方 `pluginViewCreate` / `pluginViewShow`。

## 3. 自定义控件进入设计器（官方机制）

官方 `lib/ide/customCtrl.aardio`（436 字节）：

```aardio
__IDE_CUSTOMCTRL = {
    // 自定义控件类名 » 窗体设计器显示的替代控件类名
    "radio":"radiobutton";
    "vlistview":"listview";
    "close":"plus";
    "ipaddress":"edit";
    "textbox":"edit";
    "custom.button":"button";
    "custom.edit":"edit";
    "custom.checkbox":"checkbox";
    "custom.radio":"radiobutton";
    "custom.trackbar":"trackbar";
}
namespace ide;
customCtrl = __IDE_CUSTOMCTRL;
```

语义：**设计器画不出来的控件类，用一个可画的"替身"类代画**（DSG 里仍写真实类名）。
Nodio 的对应字段：`nodio.designer.registry.register({ cls="my.ctrl"; drawAs="button"; ... })`。

## 4. 官方 IDE 的配置与主题

| 文件 | 内容 | Nodio 对应 |
|---|---|---|
| `config/USR.CFG` | `editor_font_size=12`、`editor_font_name=Consolas`、`menu_font_name=Microsoft YaHei UI`，以及**设计器网格**：`dlg_grid` / `dlg_restraint` / `dlg_margin` / `dlg_snap` / `dlg_zoom_defalut` / `dlg_grid_width` / `dlg_grid_height` / `dlg_margin_left/top/right/bottom` | `nodio.ui.settings`（键名沿用） |
| `config/SYS.CFG` | `version=46.8.3` | — |
| `config/RECENT.PROJECT.TABLE` | 最近工程表 `{[1]="路径"; ...}` | `app.project` 的最近列表 |
| `config/themes/*.xml` | 主题 XML：`<Theme><Title>…</Title><Editor>` 段含 `Background` / `SelectionColor` / `SelectionBackground` / `OutlineBackground` / `OutlineColor` / `LineNumberBackground` / `LineNumberColor` / `SidebarBackground` / `Error` / `MatchingBracket` / `HighlightWord`；`<Keywords>` 段逐条列关键字与配色 | Nodio 主题沿用同一 XML 形状（编辑器配色 + 关键字配色） |
| `config/lang/lib.txt` | 标准库面板数据：`命名空间 = 中文说明`（如 `gdi = gdi GDI 绘图`） | 右栏「标准库」页数据源，可扫库生成 + 覆盖说明 |
| `config/intellisense/winform.txt` | 58KB 的 winform 智能提示库（`!winform.xxx = 说明` 形式） | LSP 提示数据源 |
| `config/intellisense/kernel.txt` | 150KB 内核/语言提示库 | LSP 提示数据源 |

官方主题 XML 片段（可直接作为 Nodio 主题文件格式）：

```xml
<Theme>
  <Title>浅色主题</Title>
  <Editor>
    <Background>FFFFFF</Background>
    <SelectionBackground>0083EC</SelectionBackground>
    <LineNumberBackground>FFFFFF</LineNumberBackground>
    <LineNumberColor>008284</LineNumberColor>
    <SidebarBackground>E3E3E3</SidebarBackground>
    <Error>FF4444</Error>
  </Editor>
  <Keywords>
    <Color>0000FF</Color>
    <CaseSensitive>true</CaseSensitive>
    <Keyword>if</Keyword>
    <Keyword>lambda</Keyword>
  </Keywords>
</Theme>
```

## 5. 二进制侧的边界（实测）

对 `D:\aardio\aardio.exe`（6,902,784 字节，版本 46.8.3）做字面量探测：

| 名称 | 是否出现在 exe 内 | 结论 |
|---|---|---|
| `registerClassEx` / `invalidate` / `redraw` / `setPos` / `getPos` / `getRect` / `setFont` / `getClientRect` / `createWindowEx` / `getMessagePos` / `getStyle` / `isVisible` / `getParent` / `loadForm` / `hitTest` / `getItemData` / `setItemData` / `getItemPath` / `getParentItem` / `expand` / `collapse` | **全部 0 次** | 这些 API 属于**标准库源码**（`lib/win/**`），不是 exe 原生面。Nodio 走"官方源码 + 引擎补原生缺口"的路子是对的 |
| `showStatus` / `gotoLine` / `pluginViewCreate` / `setControlParam` / `insertCode` / `newForm` / `openDocument` / `refreshStdLib` … | 命中（0x003C66BC 起的连续表） | IDE 命令面在 exe 内，见 §1 |
| `oncommand` / `onnotify` | 各 1 次 | 官方钩子名 |
| 任何中文字符串（UTF-16 / GBK） | **0 次** | 该 exe 的 IDE 外壳是英文面；截图里的中文标题来自**面板脚本**（`extensions/plugin/pane/*.aardio`，其 `text="工具"` / `text="范例"` 是中文） |

## 6. 官方窗体/控件实现方式（已实测，Nodio 同款）

- `import win.ui` 加载**源码版** `lib/win/ui/_.aardio`（70KB）：`class win.form`、
  `namespace win.form`、控件工厂 `..win.ui.ctrl.factory` 全部是 aardio 源码，
  最终经 `::CreateWindowEx` FFI 建窗。
- 控件类（`lib/win/ui/ctrl/*.aardio`）的类体只设样式，**建窗由 `win.form.add` 统一完成**
  （`win/ui/_.aardio:2049`：`ctrl.hwnd = createWindowEx(...)`）。
- 自绘控件（`plus` 等）通过 `..win.registerClassEx("AAPicturePlus2",{...},"static")`
  注册窗口类——**注意**：该函数定义在 `lib/win/_.aardio`（源码），所以只
  `import win.ui.ctrl.splitter` 而不 `import win` 时会报
  "尝试调用非函数值 (.registerClassEx)"。Nodio 需在 IDE 侧统一 `import win;`，
  或由引擎补一个原生 `win.registerClassEx`（待办）。
- 窗体事件：`onMouseDown` / `onMouseUp` / `onMouseClick` / `onMouseDoubleClick`
  由官方源码派发（0x201/0x202/0x203）；**不派发 `WM_MOUSEMOVE`**，
  拖拽需 `win.subclass` 或定时轮询 `::User32.GetCursorPos`。
- 子窗体可用：`win.form(text=..;mode="child";parent=winform;..)` —— 设计画布即用此法。
