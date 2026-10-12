# aardio.re 对照原版的 IDE、VM 与发布链路审查

审查日期：2026-10-12。原版目录为 `D:\aardio`，重建项目为 `D:\aardio.re\skeleton`。原版 EXE 的文件版本和产品版本均为 **43.0.0.0**。

**结论：重建项目已具备编辑、简化语言执行、部分 Win32 API 与打包的基础链路，但原版工程迁移仍被语言语义、真实标准库、工程模型、窗体设计器和 IDE 工具接口阻断。还有保存内容丢失、发布对象错误、原生调用崩溃及资源损坏等优先级较高的缺陷。** 本报告列出 26 组问题，并给出修复顺序与验证边界。

审查只修改/新增 `audit` 下的审查产物，没有修复业务源码。运行探针使用独立进程、临时工作目录、独立 TEMP/TMP 和本地 HTTP 测试端口；未操作用户已有 IDE，也未运行会按进程名强杀其他实例的 GUI acceptance 脚本。原版基准来自随软件交付的文档、源码库和模板；本次没有执行原版 VM，文档预期与重建实测明确分开。

## 版本与实测结果

审查期间，其他会话持续更新 `AardioVM.cpp` 和 Win32 EXE。早期 Win32 回归曾 **49/49 通过**。为避免把不同版本的结果混在一起，后续从固定源码快照分别构建 Win32/x64，再复制产物执行所有关键测试。

最终审查的 VM 源码 SHA-256：

`90f0bd19e2e96e9b7580204165d178ec039a6e71f8b0c3caf6d205dc39b19b40`

对应源码已归档为 [AardioVM-2026-10-12.snapshot.cpp](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp)。以下 VM 行号引用这个固定文件；不能将本报告理解为对后续每次修改的自动验证。

| 检查 | 固定快照结果 | 含义 |
|---|---|---|
| Win32 / x64 构建 | 两者通过 | 构建成功不等于语言兼容 |
| Win32 现有完整回归 | 24 通过、25 失败 | 23 项输出受到调试日志污染；t15、t47 另有解析错误 |
| x64 t41–t48 原样回归 | 0 通过、8 失败 | 包含调试日志污染、t47 解析错误，以及 t45 的访问冲突；不能解释成八项独立的原生调用错误 |
| x64 t45 独立运行 | 退出码 3221225477，即 0xC0000005 | 原生回调导致访问冲突，Win32 同用例能输出 children3 |
| 原版常用库导入 | 8 项均未导入成功 | win、win.ui、button、gdi、gdip、sqlite、JSON、builtin.raw 均被解析错误阻断 |
| 语言语义探针 | 六组不符合原版文档预期 | 闭包、数组、类、恒等比较、返回值、UTF-8 字符串 |
| 发布资源/配置探针 | 发现资源转码、遗漏与忽略目录错误 | 测试读取新生成 EXE 的载荷，排除了旧产物假阳性 |
| 语法/运行错误和缺失输入 | 都退出 0 | 缺失输入还没有错误输出 |

结果保存在 [final-checks.jsonl](D:/aardio.re/audit/2026-10-12-final-checks.jsonl) 和 [supplement.jsonl](D:/aardio.re/audit/2026-10-12-supplement.jsonl)。较早产物的五组语义结果另存于 [runtime-checks.jsonl](D:/aardio.re/audit/2026-10-12-runtime-checks.jsonl)，用于说明这些缺陷并非仅由最后一次日志改动引入。复核脚本为 [2026-10-12-check.py](D:/aardio.re/audit/2026-10-12-check.py)，需传入已经构建的源码/产物根目录；脚本记录版本散列，探针失败写入结果文件，脚本自己的退出码不代表产品验收通过。

## 产品覆盖判断

原版四个目录合计有 2,136 个非空 UTF-8 文本 `.aardio`，约 300,137 行，另有一个 `=aardio` 标记的二进制 `.aardio`。其中 lib 868 个文本文件，examples 1,047 个，extensions 160 个，tools 61 个。计数包含注释，按文件路径计数，不等于这些文件已完成兼容性验证。

原版默认 GUI 工程从 [winform-blank/main.aardio:1](D:/aardio/extensions/wizard/project2/template/winform-blank/main.aardio:1) 开始，要求 `import win.ui`、DSG 设计标记、窗体/控件构造、显示窗口和消息循环。真实 `win.ui` 导入失败，意味着这个常规入口尚不能贯通。

| 原版能力 | 当前可确认状态 | 主要阻断 |
|---|---|---|
| 源码编辑、基本打开/保存 | 有实现，存在高风险边界 | 空文件模型不同步；新建、查找、替换入口错误 |
| `.aproj` 工程管理 | 发布器只读取一部分属性；IDE 树是目录扫描 | 缺少稳定工程根、启动文件、资源与虚拟目录模型 |
| 窗体设计器、设计/源码往返 | 关键类和入口为空 | 布局绘制、控件操作、代码解析生成、序列化未实装 |
| 智能提示、代码模板与大纲 | 静态名单补全 | 没有真实库注释、实例类型、设计器控件和用户符号的数据链 |
| 语言执行 | 源码树解释器和内建子集可运行 | 多项基本语义不符合原版，八个真实库解析失败 |
| raw/API 调用 | 部分调用和 Win32 用例成立 | 数值 by-ref 崩溃、x64 回调崩溃、浮点形参等明确限制 |
| IDE 插件、外部工具与 IPC | 发送/dispatcher 部分代码存在 | 接收入口、文本应用和原版编辑器识别协议未贯通 |
| 编译和发布 | 可生成带源码载荷的 EXE | 原版字节码未实现；错误源码也能发布；资源和工程选项有错误 |
| 调试/运行反馈 | 断点与控制台命令子集存在 | 输出不实时、失败不反映到状态、原版错误帮助链未接通 |

## IDE 问题

以下 P1 表示内容丢失、崩溃或主要工作链路阻断；P2 表示明确的可用性和集成缺陷。没有将尚未证明的全局事故定为 P0。

**1. P1：空文件编辑后 Ctrl+S 可写出空内容。源码确定，GUI 未复现。**

[CAardioView.cpp:209](D:/aardio.re/skeleton/src/view/CAardioView.cpp:209) 的 `ColorizeAll()` 设置 `m_bLoading=true`，遇到空文本在第 215–218 行提前返回，跳过第 322 行复位。加载文件的 400ms 定时器会进入这条分支。后续输入通知在 [第963行](D:/aardio.re/skeleton/src/view/CAardioView.cpp:963) 因标志未复位而跳过，不更新 `m_strText` 与 modified。保存则只序列化 [CAardioDoc.cpp:73](D:/aardio.re/skeleton/src/doc/CAardioDoc.cpp:73) 的模型文本。

触发条件：打开空 `.aardio`，等待着色后输入，再 Ctrl+S 或关闭。保存可能仍为空，关闭也不提示这些输入尚未保存。F5 另有读取编辑器内容的同步路径，因此 F5 能运行不证明 Ctrl+S 正常。

**2. P1：F7 获取错误文档并可能跳过保存，输出路径还覆盖工程配置。源码确定，多文档 GUI 未复现。**

[MainFrm.cpp:316](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:316) 使用 `GetTopLevelFrame()->GetActiveDocument()`，没有像 F5 的 `GetActiveFrame()` 一样读取 MDI 子框架；为空时选第一份有路径的文档。fallback 不同步、不保存。保存分支也忽略 `OnSaveDocument()` 的返回值并清除修改标志，且不保存所有依赖文档。

另外 [第353行](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:353) 预填当前源码旁的 `release\源码名.exe`，作为非空输出路径传入 Publisher，覆盖工程 `output/publishDir`。先打开 A，再激活 B 发布，存在发布 A 和磁盘旧内容的风险；默认确认 F7 也不能保证遵循工程输出设置。原版 publish 的对象是当前工程，见 [ide.aardio:430](D:/aardio/extensions/ide.aardio:430)。

**3. P1：工程管理缺少 `.aproj` 工作区模型。明确缺口。**

[CProjectPane.cpp:32](D:/aardio.re/skeleton/src/panes/CProjectPane.cpp:32) 只列 `.aardio`、递归最多三层；同目录在第 36 行直接跳过刷新。主框架 [MainFrm.cpp:559](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:559) 从活动文件目录推导工程目录。打开 forms/lib 中的文件会改变所谓工程根；资源文件、虚拟目录、启动文件属性、增删同步都没有完整实现。

原版工程格式和独立工程根见 [file.md:9](D:/aardio/docs/guide/ide/file.md:9)、第 41–53 行。Publisher 的局部 XML 读取不能替代 IDE 工程模型。

**4. P1：窗体设计器缺少实际设计、代码往返与持久化。明确缺口。**

[CDialogEditorView.h:13](D:/aardio.re/skeleton/src/designer/CDialogEditorView.h:13) 绘制为空，[CDiagramEditor.cpp:9](D:/aardio.re/skeleton/src/designer/CDiagramEditor.cpp:9) 仅建立绘制上下文，[CDialogEditorDoc.cpp:12](D:/aardio.re/skeleton/src/designer/CDialogEditorDoc.cpp:12) 序列化为空。Ctrl+U 和相关 IPC 的文本应用/切换路径未接通。

欠缺控件拖放、尺寸/对齐、属性修改、事件生成、DSG 解析与生成、设计/源码切换，以及布局保存。不能把注册了文档模板当作实现了设计器。空序列化另有写空文档的潜在风险，但本次未证明用户能通过正常 UI 进入该保存路径。

**5. P1：IDE IPC 与原版插件/工具接口不通。明确缺口。**

[AardioIpc.cpp:120](D:/aardio.re/skeleton/src/app/AardioIpc.cpp:120) 的 dispatcher 没有有效接收调用链，主/子框架没有相应 `WM_COPYDATA` 接收入口；部分 apply-text 分支只刷新视图，不应用传入内容。原版 [command.aardio:23](D:/aardio/lib/ide/command.aardio:23) 与 [trigger/init.aardio:17](D:/aardio/extensions/trigger/init.aardio:17) 的工具协议不能靠发送代码单独成立。

原版工具还按 `AardioCodeEditor.+` 识别编辑器，重建视图使用标准 RichEdit；如 [indent.aardio:25](D:/aardio/tools/Code/indent.aardio:25) 的工具无法直接沿用。插件启动、工程对象、编辑文档操作和脚本工具菜单需作为完整接口契约验收。

**6. P2：新建确认后不能创建文档。源码确定。**

[MainFrm.cpp:62](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:62) 显示通用对话框，确认后调用 `OpenDocumentFile(nullptr)`；[CAardioApp.cpp:490](D:/aardio.re/skeleton/src/app/CAardioApp.cpp:490) 先对空路径检查文件属性并返回。新建类型列表、模板选择、文件/工程向导没有完成。

**7. P2：Ctrl+F、Ctrl+G 在对话框销毁后读取输入。源码确定。**

[MainFrm.cpp:385](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:385) 和第 405 行在 `DoModal()` 返回后才 `GetDlgItemText()`，没有 DDX 或 OnOK 缓存。查找输入资源 [dialogs.rc:13](D:/aardio.re/skeleton/src/dialogs.rc:13) 还是未填充的 `CBS_DROPDOWNLIST`。全字、模式、方向、多文档和全部标记选项也没有相应行为。窗口生命周期错误会阻断正常输入传递，Debug 下还可能触发断言。

**8. P2：Ctrl+H 打开的是仅查找对话框。源码确定。**

[MainFrm.cpp:82](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:82) 调 `CFindReplaceDialog::Create(TRUE,...)`，TRUE 是 `bFindDialogOnly`。虽有替换分支，入口没有替换控件，也不会正常产生这些通知。

**9. P2：查找/单次替换混用 CRLF 与 RichEdit 坐标。源码确定混用，具体 GUI 结果待复现。**

[CAardioView.cpp:769](D:/aardio.re/skeleton/src/view/CAardioView.cpp:769) 直接把 `GetWindowText()` 文本下标送入 `EM_EXSETSEL`；单次替换同样处理。补全代码在同文件第 437、584 行明确区分 CRLF 文本空间和控件 CR 空间，搜索没有对应转换。多行命中有偏移和替换错位置的风险，不能将风险写成已实测覆盖用户代码。

**10. P2：运行输出不是实时反馈，错误状态和帮助链不完整。源码确定。**

[MainFrm.cpp:249](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:249) 把输出累计到进程退出才显示，长运行 GUI/服务无法持续看日志；[第288行](D:/aardio.re/skeleton/src/ui/MainFrm.cpp:288) 忽略退出码，统一显示“运行完成”。调试分支释放进程句柄后不再跟踪。原版运行报错后的 F1 帮助机制见 [codeEditorHelp.aardio:397](D:/aardio/extensions/trigger/codeEditorHelp.aardio:397)，当前没有对应集成。本报告没有把 C++ 式 Watch/调用栈 UI 当作未经确认的原版要求。

**11. P2：智能提示、大纲和库模板没有接入真实数据。明确缺口。**

[CAardioView.cpp:26](D:/aardio.re/skeleton/src/view/CAardioView.cpp:26) 静态写死成员，补全不分析真实 import、实例和用户符号；[CCodeOutlineParser.cpp:4](D:/aardio.re/skeleton/src/editor/CCodeOutlineParser.cpp:4) 将源码包装为一个 token，实际视图也未形成大纲调用链。原版要求处理提示注释、实例签名、通配字段、模板插入位置和 DSG 控件提示，见 [intellisense.md:3](D:/aardio/docs/library-guide/intellisense.md:3)。

## VM 与语言问题

**12. P1：真实标准库导入被语言解析缺口阻断。实测。**

设置 `AARDIO_LIBDIR=D:\aardio\lib`，每个库用新进程执行 `import 库; io.print("IMPORT_OK");`。八项全部失败，未到达 IMPORT_OK：

| 原版库 | 固定快照首个错误位置 |
|---|---|
| win | win/_.aardio 第72行，期望 } |
| win.ui | win/ui/_.aardio 第646行，期望 } |
| win.ui.ctrl.button | button.aardio 第27行，意外的 = |
| gdi | gdi/_.aardio 第21行，期望 } |
| gdip | gdip/_.aardio 第25行，意外的 @ |
| sqlite | sqlite/_.aardio 第5行，期望 ) |
| JSON | JSON/_.aardio 第52行，期望 } |
| builtin.raw | builtin/raw.aardio 第62行，期望 } |

这些是首次失败位置，不能单凭行号断言全部根因。例如 gdip 第25行在提示注释区域，需要跟踪嵌套注释词法，不能直接归因于缺少普通 @ 运算符。原版 [sqlite/_.aardio:5](D:/aardio/lib/sqlite/_.aardio:5) 的 `$` 文件嵌入则确实是常用编译期语法。现有回归使用 `minimal shape` 库，未覆盖真实库的语法组合和初始化闭包。

**13. P1：闭包只保存命名空间，丢失外层局部变量。实测与源码一致。**

[快照第3048行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3048) 的函数值只捕获 `nsTable`，调用局部环境直接指向 globals。`make(){var x=7;return function(){return x;}}` 返回的函数输出 null，原版文档要求 7。基准见 [closure.md:18](D:/aardio/docs/language-reference/function/closure.md:18)、第31–61行。计数器、工厂函数、事件处理和长期回调受影响。

**14. P1：覆盖数组元素写入 map，形成与数组不一致的第二份数据。实测与源码一致。**

[快照第3746行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3746) 仅追加位置写 arr，其余写 map。`var a={10,20};a[1]=99;` 直接读取是99，`table.concat` 仍是 `10,20`，`table.count` 变3。正常预期是99、`99,20`、2。原版排序算法本来就依赖覆盖下标，见 [SelectionSort.aardio:116](D:/aardio/examples/aardio/Array/ComparisonSort/SelectionSort.aardio:116)。

**15. P1：类成员构造时机与方法 this 绑定错误。实测与源码一致。**

[快照第3910行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3910) 在类定义时计算成员值，实例化只复制模板再跑 ctor；普通函数值又没有固定实例绑定。ctor 写 `this.v=7` 后的成员 `copy=this.v` 得到 null；`var f=b.get;f()` 也得 null。原版要求 ctor 后按顺序构造成员、this 静态绑定实例，owner 根据调用点动态绑定，见 [syntax-quick-ref.md:1195](D:/aardio/docs/guide/language/syntax-quick-ref.md:1195)、第1255行和 [class.md:280](D:/aardio/docs/language-reference/class/class.md:280)。

**16. P1：不同对象和不同函数的恒等比较为 true。实测与源码一致。**

[快照第3625行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3625) 用类型和 ToString 比较 `===`。两个不同空表、两个不同函数分别得到 true，文档预期都是 false。基准见 [equality.md:7](D:/aardio/docs/language-reference/operator/equality.md:7)。对象判重、缓存、事件解绑等逻辑会错误。

**17. P1：无 return 的结果和末尾多返回展开不符合原版。实测与源码一致。**

[ExecBlock 第3814行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3814) 返回最后语句值；调用参数逐个压入，不展开 `mrets`，表构造也没有尾部展开。实测：无 return 的 `function(){123;}` 返回123；`take(pair())` 的第二参数为null；`{pair()}` 长度为1。原版预期依次为null、2、2，见 [result.md:8](D:/aardio/docs/language-reference/function/result.md:8)、第27–43行。

**18. P1：字符串的长度、下标与二进制模型不兼容。实测与源码一致。**

Value 把字符串存成 `wstring`，[# 运算第3043行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:3043) 返回 UTF-16 单元数，字符串下标不能沿表访问路径取出字节。`var s="中";io.print(#s);io.print(s[1]);` 实测为1和null，原版 UTF-8 预期为3和228。原版字符串是可包含任意二进制的字节序列，见 [string.md:9](D:/aardio/docs/language-reference/datatype/string.md:9)、第21–37行。这会影响长度、模式/解析位置、资源、编码转换与 FFI；局部修补 print 编码无法解决类型模型问题。

**19. P1：数值 by-reference 参数会作为地址解引用，普通调用可崩溃。实测。**

最小片段：

```aardio
var f=User32.api("GetWindowThreadProcessId","int(addr,int&)");
var tid,pid=f(0,0);
io.print(tid);
io.print(pid);
```

重建 Win32 进程退出 `0xC0000005`。实现 [第5505行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:5505) 将数值 by-ref 记为显式指针，返回阶段 [第5681行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:5681) 根据这个值形成 pBuf 并解引用；零值因而变成空指针。这项崩溃无需假定原版实际执行结果即可确认。

**20. P1：x64 callback trampoline 违反调用约定，回归 t45 访问冲突。实测与源码一致。**

[t45.aardio:10](D:/aardio.re/scripts/vmtests/t45.aardio:10) 注册回调并通过 EnumChildWindows 计数；Win32 输出children3，x64访问冲突。生成代码 [快照第462行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:462) 将 ctx 装进 R10，却没有放入 C++ shim 要求的 RCX；也没有保存原始前四个参数到 shim 所读取的 home slots。x64 能构建不能证明 x64 FFI 可用。浮点参数、cdecl callback、线程/生命周期仍需要独立矩阵验证。

**21. P1/P2：增量修改引入连接运算解析回归并留下调试输出。实测与源码一致。**

[快照第2389行](D:/aardio.re/audit/AardioVM-2026-10-12.snapshot.cpp:2389) 和第1850行无条件写 `[CAT]`、`[INC]` 到stderr，污染23项现有验收。增量/连接区分还只比较 `Peek(1).text` 的分号/右花括号，没有检查这是标点 token。字符串内容 `";"` 被当语句结束：`io.print("a" ++ ";");` 报“期望 )”，并阻断t15、mytable/t47。应把输出污染与实际解析回归分别验收。

**22. P1：原版编译代码、字节码与宿主契约仍未实现。明确缺口。**

[AardioVM.h:1](D:/aardio.re/skeleton/src/engine/AardioVM.h:1) 和公开接口表明当前是树解释器，只有 RunSource 与 CheckSyntax；F8 是解析检查。发布封装是自定义 ARDP 源码载荷，不能据此声称支持原版 `=aardio` 编译代码。原版确实交付二进制 `.aardio`，例如 `lib/ide/aifix.aardio`，格式说明见 [file.md:5](D:/aardio/docs/guide/ide/file.md:5)。本次未逆向完整原版字节码格式，尚不能给出实现成本或完整指令差异。

## 发布与错误反馈问题

**23. P1：发布会破坏二进制资源字节。实测。**

[AardioPublish.cpp:293](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:293) 收集目录中所有文件却统一调用 [第199行](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:199) 的 UTF-8 源码读取器，先解码再重编码。14字节非UTF-8测试资源进入EXE后变成多个 `EF BF BD` 替换字符。载荷恢复 [第706行](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:706) 又把内容转成wstring。

PNG、ICO、DLL、字体和任意二进制资源都可能受影响。资源应有独立字节载荷和读取契约；修一个图片 API 不足以修复整个打包路径。

**24. P1：库嵌入与资源嵌入错误耦合，ignored/local 语义不完整。实测及明确缺口。**

[AardioPublish.cpp:475](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:475) 仅在 `libEmbed=true` 分支收集嵌入目录。工程 `libEmbed=false`、res `embed=true` 时，EXE仅有main，资源缺失。`ignored=true`、`embed=true` 的目录仍被收集，实测载荷存在res。XML读取 [第168行](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:168) 只取path/embed，没有local/ignored，非嵌入本地目录也无完整复制路径。

原版这些选项有独立语义，见 [file.md:41](D:/aardio/docs/guide/ide/file.md:41)。忽略目录被错误打包还会把本不应发布的文件带入产物；本次用的是人工测试文件，没有证明已有工程发生敏感内容泄露。

**25. P1/P2：发布不先验证源码，去调试符号选项不生效，产物保留明文源码。实测及源码确定。**

构造 `var = ;` 的非法启动源码，Publisher仍报告“发布完成”；新EXE运行才报语法错误。发布器没有调用编译/CheckSyntax，且 [AardioPublish.cpp:615](D:/aardio.re/skeleton/src/app/AardioPublish.cpp:615) 直接忽略 bStripDebug。载荷中源码可直接读取，见第460–600行。

因此 UI 中的编译/去调试选项目前不能代表原版编译管线。这里没有把“去调试符号”误解为“自动加密源码”；事实是该参数完全未处理，当前产物也不使用原版编译代码。

**26. P1/P2：运行错误、缺失输入和发布失败没有可靠退出契约。实测及源码确定。**

[CAardioApp.cpp:208](D:/aardio.re/skeleton/src/app/CAardioApp.cpp:208) 丢弃 RunSource 返回值，AARDIO_RUN分支最后返回FALSE；语法错误、运行error、缺失文件都得到退出码0，缺失输入还静默。已发布程序的RunSource返回值也被丢弃。

另一个独立控制流问题：[第269行](D:/aardio.re/skeleton/src/app/CAardioApp.cpp:269) 在 BOOL InitInstance 中 `return bOk ? 0 : 1`，失败时反而返回TRUE，随后可能进入MFC消息循环；成功返回FALSE也不是设置进程状态码。本次未用失败发布路径作动态验收，这一项的失败行为范围仍以源码判断为准。统一的错误/退出协议对IDE状态、回归和CI都必要。

## 测试可信度与尚未覆盖的范围

- 现有脚本比较的是人工fixtures；[win fixture:1](D:/aardio.re/scripts/vmtests/win/_.aardio:1)、[mytable:1](D:/aardio.re/scripts/vmtests/mytable.aardio:1) 和发布库都不是直接执行原版库。应保留子集回归，同时增加真实标准库、默认模板和原版例子测试集。
- [run_vm_tests.ps1:59](D:/aardio.re/scripts/run_vm_tests.ps1:59) 先等待退出，再读stdout/stderr。较大输出可塞满管道导致阻塞并触发超时强杀；没有检查退出码和明确记录超时。第61–63行删除所有换行，忽略输出行边界，也不保留stdout/stderr真实交错顺序。
- [第88行](D:/aardio.re/scripts/run_vm_tests.ps1:88) 的t49仅检查exe存在，不先删旧产物、不核对发布退出码/新鲜度。本次复制fixtures时排除旧EXE，避开这个假阳性。
- [accept_r32.ps1:96](D:/aardio.re/scripts/accept_r32.ps1:96) 强杀其他同名aardio_re进程；[accept_publish.ps1:19](D:/aardio.re/scripts/accept_publish.ps1:19) 全局匹配对话框而不绑定PID，第88行关闭它。某些accept脚本只打印失败观测而不失败退出，本次未执行这些GUI脚本。
- IDE结论主要是完整源码链的确认，没有完成独立GUI实例的操作复现；尤其多文档F7和多行查找偏移还需要交互验收。未以截图外观推断功能。
- 尚未完成GC/循环引用、跨线程全局状态、长期callback生命周期、全量raw结构与调用约定、COM/浏览器控件、性能与原版完整字节码审计。这些是未覆盖范围，不能据此当作确定缺陷，也不能给出整体还原百分比。

## 建议处理顺序

1. **先保住内容和进程稳定性。** 修空文件保存链、F7工程对象/保存失败处理、数值by-ref和x64callback；同时清除调试输出、恢复现有回归并建立可靠退出码。
2. **把语言语义作为统一契约修正。** 优先闭包、数组单一数据、对象恒等、类构造/this、返回协议、字符串字节模型。将本报告最小片段固化为可执行断言，防止各库内建分别绕过错误。
3. **以原版常用库和默认GUI模板作为准入条件。** 按实际首个失败追词法/解析/运行时，逐步贯通win、win.ui、gdi/gdip、raw、JSON、sqlite及所需传递依赖。仅把import解析过关还不足够，随后要覆盖初始化、控件事件和消息循环。
4. **建立稳定工程上下文与统一构建发布链。** `.aproj`决定工程根、启动文件、资源及发布目标；编辑缓冲区、保存、F8/F5/F7使用同一上下文。资源保持原始字节，embed/local/ignored独立处理，产物在隔离运行环境验证。
5. **再完成设计器和IDE生态接口。** 布局文档/代码往返、属性与事件、库元数据补全、工具插件和IPC。原版编译代码/宿主格式应作为独立的大项明确范围，避免用源码载荷的可运行性代替兼容结论。

本轮交付的是功能与缺陷评估及可复核证据，未应用上述修复。
