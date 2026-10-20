# Nodio roadmap — 100% aardio source compatibility, x64 first

> **2026-10-20 校准（第四轮修复后更新）**：本文件的若干分数已过期。以
> `docs/capability-gap.md` 为准。最新实测：命名空间导入 **54/54 (x64)**；
> 发布回归 **绿**；x64/x86 **均构建通过**；VM 回归 **88 项全过**（x64 与 x86 同，
> t23 为环境门控）；IDE 冒烟 **SMOKE OK**。第一轮记录的"发布回归红 / 编译不过 /
> 错误路径崩溃"已全部修复（见 capability-gap.md 的 P0-1 / P0-2 / P0-3 与 §1.4）。
> **第四轮打通官方源码级 `win.ui` 兼容路线**：官方 `import win` / `import win.ui`
> 在 Nodio 上可跑，**右键事件由官方 `win.ui.tracker` 提供**
> （`onContextMenu(x,y)` 屏幕坐标 / `onRightMouseDown/Up`），官方
> `win.ui.popmenu` 弹出菜单实测可弹出 —— 详见 capability-gap.md **§1.9**。

> **第五轮（IDE chrome 自绘化 + 设计器闭环）**：把 IDE 的文档页签 / 工具条 / 右栏底栏
> dock / 设计画布改成自绘，外观对齐官方 IDE 截图。本轮定位到一整族**两套控件方言互不兼容**
> 的缺陷（官方 `setPos(x,y,宽,高)` vs 引擎原生 `setPos(l,t,r,b)`；官方 `text` 是属性、
> 引擎原生是函数）——这才是「布局错乱」的根因。修法是标准库加方言无关封装
> `nodio.ui.rect`（统一走原始 `win.setPos`、物理像素），所有 `form.add` 一律
> `autoResize=false`（官方默认会按 `_defRect` 把控件弹回原位）。
> 另实测三条官方能力在本机走不通并各自绕开：`ctrl.setPos` 触发引擎断言崩溃、
> `plus` 会把父背景整块拷进自己的画板（透明 overlay 不可行）、
> `static` 的 `bgcolor` 依赖 `WM_CTLCOLORSTATIC` 画不出来。
> 详见 `docs/capability-gap.md` **§1.10**。

> **第五轮（右键 · IDE 层接线）**：把官方 `win.ui.tracker` 的右键事件接到 IDE 的三个真实落点
> —— **编辑器 / 工程树 / 设计画布**（组件层在 `lib/nodio`，IDE 层在 `ide/nodio/app`）。
> 菜单一律用官方 `win.ui.popmenu`（`popId()` 不带坐标调用时官方取光标屏幕坐标）。
> 接线过程中挖出一个**此前从未暴露的引擎级缺陷**：官方 `lib/win/api.aardio:11` 把
> `::CallWindowProc` / `::DefWindowProc` 声明成 **int 返回**（32 位进程的写法），
> Nodio x64 下会把 64 位 `LRESULT` 截成 32 位 → 官方 `treeview` 装上 wndproc 后
> `HTREEITEM` 只剩低 32 位 → **插入子节点直接失败**。已修（`engine/builtins.cpp` 的
> `api()` 在 x64 下把这两个 API 的返回类型提升为 `int64`），另修 `win.getMessagePos()`
> 无参时返回真实光标位置（官方 tracker 的 `lParam == -1` 回退依赖它）。
> 新增回归夹具 `tests/ide_ctx/ctx.nodio` + `tests/ide_ctx_smoke.ps1`（x64/x86 双 OK）。
> 详见 `docs/capability-gap.md` **§1.11**。

> **第七轮（引擎契约 · 非 IDE）**：三件事。
> ① **E-11 回调释放**（capability-gap.md §2 E-11）：官方 `raw.tostdcall` 的回调是
>    **cdata 对象**、**由 GC 自动回收**（官方 `raw/callback.md` §2/§6），原先 Nodio 的
>    `g_cbs` 只增不减 → 回调闭包连同它捕获的一切被永久钉住。引擎侧补 `ReleaseCallback()`
>    + trampoline 内存池（**释放的槽不复用**，防原生侧迟到调用串到别的回调），回调被回收时
>    把跳板改写成中性桩（`xor eax,eax; ret`）——**比官方安全**：官方直接 free，而官方文档
>    自己警告「外部组件的 C、C++ 却并不知道……不可预知的错误」。回归 `tests/vmtests/t91.aardio`。
> ② **import 契约修正**（§2 E-12）：删掉 `engine/import.cpp` 里自动预载父命名空间的逻辑。
>    官方 `import.md` 的查找顺序只有 4 条路径，**没有父命名空间预载**；官方库自己写
>    `import win; import win.ui.ctrl;`，命名空间链由库的 `namespace a.b;` 建立。
>    修正后 5 个官方「禁止直导」guard 按**官方原文**报错；回归
>    `tests/import_contract_check.ps1`（10 项，x64/x86 全绿）。
> ③ **sweep 工具链缺陷**（§3）：`tests/module_sweep.ps1` 原先所有模块共用同一个 stdout
>    重定向文件，列表第 418 个 `process.adb.connecter` 拉起的常驻 adb server 一直占着句柄
>    → **从第 418 个起 552 个模块的判定全是假的**。改成每个模块独立临时文件后恢复正确，
>    并对「空输出 FAIL」自动重试一次。前四轮「残差 12 项」实际只覆盖前 417 个模块。
> ④ **E-13 系统 API 表归属**（§2）：`CreateRoundRectRgn` / `CreateRectRgn` / `CombineRgn`
>    实属 **gdi32.dll**，引擎表里写成 user32.dll → 官方 `lib\win\region\_.aardio` 的
>    `::CreateRoundRectRgn := ::Gdi32.api(...)` 赋值左值走同一张表 → 直接报
>    「找不到 API」。改正后 4 个官方模块由 FAIL 转 OK。回归 `tests/sysapi_check.ps1`。
> 本轮实测：VM 回归 **89 项全过**（x64 与 x86 同）；命名空间 **54/54**；发布回归 **绿**；
> 标准库导入 **全绿**；**969 模块 sweep 修正后 x64 `ok=897` / x86 `ok=931`，
> x86 失败集是 x64 的严格子集，差集正好 34 项且全是 32 位 DLL 物理边界 ——
> 两套架构上引擎侧零残留缺陷**。

> **第九轮（右键链路 + 官方预编译块）**：三件事。
> ① **右键链路打通**（capability-gap.md §2 E-15）：先用引擎层探针证明官方
>    `win.ui.tracker` 的右键契约完好（`WM_CONTEXTMENU(0x7B)` → `onContextMenu(x,y)`，
>    屏幕坐标精确一致，见 `D:\aardio\lib\win\ui\tracker.aardio:150-166`），
>    再定位到**组件层**两处缺陷：自绘工程树 `lib/nodio/ui/tree.aardio` 唯独没把
>    `onContextMenu` 转给 board（`projectTree.aardio:133` 挂的那条回调没人读），
>    且 `nodeAtScreen` 拿**客户区**矩形做屏幕→客户区换算导致命中恒 miss。
>    修完官方 `win.ui.popmenu` 真实弹窗（Win32 弹出菜单窗口 `#32768` 实测 123×45），
>    回归 `tests/ide_ctx_smoke.ps1` **CTX-SMOKE OK**（x64 + x86）。**未改任何 IDE 文件**。
> ② **overlay 非法 for 步长**（§2 E-16）：`lib/nodio/ui/titlebar.aardio:326` 写成
>    `for(i=1;#items;1++)`，而官方 aardio 的 for 步长是**常量**（`for(i = first, last
>    [, step=1])`），`1++` 是给字面量自增 → `tests/ide_smoke.ps1` 的 overlay 守卫
>    直接 FAIL（IDE 编译不过）。改正后 SMOKE OK。
> ③ **`=aardio` 容器解析器进引擎**（§2 E-14）：新增 `engine/aardio_chunk.cpp`，
>    import 官方 `ide.aifix` 时给出结构化诊断而不是笼统的「不支持的字节码格式」
>    （`已解析出 97 个对象 / 8734 条指令 / 2173 个常量（最大嵌套 4 层）`，
>    与 Python 参考实现逐项一致），回归 `tests/aardio_chunk_check.ps1`（10 项全绿）。
>    同时用 Ghidra 12.1.4 headless 重新分析官方 `aardio.exe`（390 s），
>    **新导出**表达式节点优先级表（26 项 × 2 字节）、官方内建分派表（41 个具名内建
>    → 处理函数地址）、**10/64** 条记录语义与 **20/64** 条官方校验约束
>    （`tools/aardio_vm_tables.py`、`docs/aardio-bytecode.md` §6.1–§6.4）。
>    后续续挖**定位到官方 VM 主分派 `FUN_004EC700` 与记录发射器 `FUN_004F5B20`**，
>    确认官方 VM 是 **AST 树遍历 + 记录流**（不是 RISC 指令流 —— 这修正了 §5 的表述），
>    并导出 **AST 节点类型 → 26 类运算符**的映射（`FUN_004EE2E0`，优先级与 aardio
>    运算符阶梯自洽）+ **83 个记录发射点**（10 类 kind 为常量）。
>    仍缺 64 种记录各自的语义 → 官方预编译模块依旧不可执行（物理边界）。
> 本轮实测：VM 回归 **90 项全过**（x64 与 x86 同）；右键冒烟 **CTX-SMOKE OK**；
> IDE 冒烟 **SMOKE OK**；import 契约 / 系统 API / 新增的 `=aardio` 容器回归全绿。

> **第十轮（官方 API 面补齐 · 非 IDE）**：一件事。
> ① **E-17 `win.clip` 官方 API 面**（capability-gap.md §2 E-17）：官方
>    `D:\aardio\lib\win\clip\_.aardio`（`namespace win.clip;`）定义 **12** 个成员
>    （`readUtf16/read/write/writeBitmap/readBitmap/clear/test/formats/
>    formatNames/getFormatName/eachFormat/readUnicode`），官方文档
>    （`docs\guide\language\syntax-quick-ref.md:1621`）要求显式
>    `import win.clip;`；而 IDE 的 `ide/nodio/app/editor.nopio` **不导入**、
>    直接用 `win.clip.write/read`，靠引擎内置表兜底 —— 内置表只有
>    **read/write 两个**，实测 `type(win.clip.formats)`/`clear`/`test` 全是
>    null，**官方包的 10 个成员根本没生效**；而 `import win.clip;` 后官方
>    实现完全可用（`write` 返回剪贴板数据句柄、`read` 读回文本，且与内置
>    实现互通）→ 缺的不是官方包，是**内置兜底的 API 面**与**两条路径的一致性**。
>    修法（`engine/builtins.cpp`，未改任何 IDE 文件）：内置 `win.clip` 补齐到
>    官方 12 成员，语义按官方 —— `write(str,rep)` 的 `rep` 控制是否先
>    `EmptyClipboard`、成功返回**剪贴板数据句柄**（不再返回 bool）；
>    `read()/readUtf16()` 在剪贴板没有 `CF_UNICODETEXT` 时返回 **null**
>    （官方 aardio 里空串是真值，返回 null 才能让 `if(s)` 正确跳过粘贴）；
>    `formats()/test()/getFormatName()/eachFormat()/clear()/writeBitmap()/
>    readBitmap()` 全部按官方语义实现，`formatNames` 与官方 24 个友好名
>    逐项一致。**编码差异（已知且无害）**：官方包 `read()` 返回 UTF-8
>    **字节串**（官方 aardio 的字符串就是 UTF-8 字节串），Nodio 字符串模型是
>    **宽文本** → 内置 `read()` 返回宽文本；剪贴板载荷（CF_UNICODETEXT）
>    一致，且实测官方 richedit 的 `text/selText` 两种形态都接受。
>    回归 **新建** `tests/win_clip_check.ps1` + `tests/win_clip/clip.nopio`：
>    同一脚本先跑内置实现（B-*）再 `import win.clip;` 跑官方包（O-*），
>    断言两路径的 12 成员面 / `formatNames` / `getFormatName` 友好名 /
>    写入-读回-clear 语义**逐项一致**；剪贴板被同机进程占用时按官方
>    「静默失败」语义断言 null/0 —— x64/x86 全绿。
> ② 顺带修掉 `tests/ide_smoke.ps1` overlay 守卫的**误报**：原正则会把 for
>    循环**体**里的 `++` 当成 for 步长（并发会话的
>    `lib/nodio/ui/editor.aardio:354` `for(i=1;#lines;1) out[i] = INDENT ++
>    lines[i];` 被误判成非法步长）→ 改为只看 for 头第三子句。
> ③ **`=aardio` 记录结构完整导出 + 「64 路分派表」证伪**（§2 E-14 续）：
>    把官方「记录种类的操作数格式描述符表」`DAT_007C80A0`（VA `0x007C80A0`，
>    64 字节）完整解出，逐条对照官方 chunk 校验器 `FUN_004d4450` 的取操作数
>    代码 —— **修正了第九轮对该表位含义的错误推断**：合法记录种类只有
>    **61 种**（`0x00`–`0x3C`；「64 种」来自种类字段占 6 位，校验器
>    `if (0x3c < uVar6) return 0;` 直接拒绝 `0x3D`–`0x3F`）；描述符
>    bits0..1 = 操作数格式类（0 = 双 9 位操作数 / 1 = 单 18 位无符号 /
>    2 = 单 18 位有符号）、bits4..5 = 槽位索引或相对位移、bit6 = 可作跳转
>    目标、bit7 = **必须紧跟一条 0x22 跳转记录**；61 种按描述符字节归并为
>    **24 种记录形态**（其中 14 种是条件跳转类）。又用 Ghidra `ListRefs`
>    反查描述符表的全部 5 个引用点 —— **全在编译/校验侧，运行期零引用**
>    → 官方**根本没有 64 路分派表**（运行期对记录种类是逐种类直接分派），
>    与第九轮「按目标跨度 < 8KB 筛 64 项跳转表全部命中 MFC 消息表」的
>    结论互相印证。固化：`tools/aardio_vm_tables.py`（`OPERAND_DESC_BYTES` /
>    `decode_operand_desc()` / `RECORD_SHAPE_GROUPS` / `check_shape_groups()`
>    自检 61/61 覆盖），文档 `docs/aardio-bytecode.md` §5.1/§5.2/§6.9。
>    **可执行性仍是物理边界**：61 种记录的运行期语义未逐条还原。
>    另定位到官方 chunk 的**运行时帧布局**（`FUN_004e93a0` 分配 0x50 字节帧 →
>    `FUN_004e8880` 读 code 数组 → `FUN_004e8960` 读常量池/嵌套对象 →
>    `FUN_004e8df0` 读三个并行数据数组 → `FUN_004d4450` 用校验器验记录流），
>    每个字段偏移都与校验器对得上（+0x0c code 数组 / +0x2c code 条数 /
>    +0x28 帧槽位上限 / +0x08 命名空间成员表 / +0x34 深度计数 / +0x48..0x4b 标志字节），
>    文档 `docs/aardio-bytecode.md` §3.1。
> ④ **E-17 的 sweep 证据**：定向 sweep（官方库全量检索 `win\.clip` 得 21 个引用者，
>    新建清单 `tests/clip_modules.txt`）x64 ok=18 / fail=3，3 个失败全是既有项
>    （2 个 32 位 DLL 物理边界 + 1 个 `=aardio`）；**全量 969 模块重跑 ok=897 / fail=72**，
>    与 r10 基线**数值一致且失败集合逐项相同**（差集双向为空）→ **零回归**。
> 本轮实测：VM 回归 **89 passed / 0 failed / 1 skipped**（x64 与 x86 同，
> 跳过的是 t23 剪贴板用例——本机剪贴板被其它进程占用，官方语义下无法
> 往返，按仓库既有的 env-gate 显式 SKIP）；命名空间 **54/54**；发布回归
> **绿**；标准库导入 **全绿**；右键冒烟 **CTX-SMOKE OK**；IDE 冒烟
> **SMOKE OK**；新增 `win_clip` 契约回归 **WIN-CLIP OK**（x64 + x86）。
>
> **第十一轮（`=aardio` 运行期解释器定位）**：把第十轮剩下的最后一个真缺口
> 「61 种记录的运行期语义 + 解释器本体」正面拿下。
> ① **解释器本体定位（决定性突破）**：`FUN_004C4BC0` @ `0x004C4BC0`，
>    函数体 **7263 字节**，是整个 `aardio.exe` 里**唯一**一处对 32 位记录做
>    `switch (word & 0x3F)` 的函数，case 覆盖 `0x00`–`0x3C` **全部 61 种**
>    （与 chunk 校验器 `FUN_004D4450` 的 `if (0x3c < uVar6) return 0;` 吻合）；
>    两个入口是 `FUN_004D33C0`（带 C 栈溢出检查，上限 200 层）与 `FUN_004D3450`。
>    定位手法：从官方**调试器变量解析器** `FUN_004D4940` 反查 → `FUN_004D4BD0`
>    → `FUN_004D4170` 追到 `FUN_004D42A0`，再由 `FUN_004D50A0`（建立闭包）
>    的第二个调用者 `FUN_004D33C0` / `FUN_004D3450` 落到 `FUN_004C4BC0`。
> ② **记录字位布局（实测，权威）**：kind = bits 0..5；A = bits 6..13
>    （目标局部槽 / 跳转条件）；C = bits 14..22、B = bits 23..31，各为
>    「9 位字段 = 低 8 位索引 + 最高 1 位全局作用域标志」；跳转类
>    （`0x22`–`0x28`/`0x2D`/`0x2E`）把 bits 14..31 当**一个 18 位有符号相对
>    位移**用（bias `0x1FFFE`），条件跳转读的是**下一条 u32**（编译器紧跟一条
>    `0x22`）。
> ③ **运行时槽位与类型标签（实测）**：槽位固定 16 字节（值 +0x00 / 类型标签
>    +0x08 / 附加 +0x0C / 附加 2 +0x10），类型标签 0=null 1=bool 2=int
>    3=number(double) 4=string 5=namespace 6=function 7=table。
>    VM 状态对象与 0x18 字节调用帧的全部字段偏移也一并实测。
> ④ **61 种记录的语义逐条读出**：算术组 `0x18`–`0x1E` = `+ - * / % ^ 一元-`
>    （helper `FUN_004C4740` 的分派参数 = opcode − 0x12），
>    位运算组 `0x35`–`0x3B` = `~ & | ^ << >> >>>`
>    （helper `FUN_004C49C0` 的分派参数 = opcode）——这两组有**双路佐证**
>    （解释器快速路径 + helper 分派参数），其余按 case 逐条读出。
> ⑤ **顺带修正第十轮的一处结论**：描述符表 `DAT_007C80A0` 的「18 位操作数
>    格式」在**运行期不成立**（解释器里每个 kind 的两个操作数都只取 9 位，
>    18 位只用于跳转位移），该表**只能**用于编译/校验期；校验器对 kind `0x22`
>    的解读（跳过 B 条记录）与解释器（18 位相对跳转）**不一致，原因待查**。
>    位移来源与偏置**逐 kind 不同**（`0x22`/`0x2D`/`0x2E` 取本条记录、
>    偏置 `0x1FFFE`；`0x23`–`0x26` 取下一条 u32、偏置 `0x1FFFE`；
>    `0x27`/`0x28` 取下一条 u32、偏置 `0x1FFFD`）——`0x27`/`0x28` 的偏置少 1，
>    正好使「条件跳转落点」与「紧跟的 0x22 落点」相同。
>    **全样本独立验证**：官方 `aifix.aardio` 全部 97 个对象里，
>    条件跳转 / `0x22` 配对 **228 对、落点不一致 0 处**。
> 固化：新增 `tools/aardio_vm_runtime.py`（解释器入口 / 位布局 / 槽位布局 /
>    类型标签 / 61 条语义表 / `check_opcode_table()` 61-61 自检），
>    `tools/aardio_opcodes.py` 改用**运行期**位布局解码并输出语义名，
>    `tools/aardio_vm_tables.py` 增加运行期小节，
>    文档 `docs/aardio-bytecode.md` 新增 §7（7 小节，含 61 行语义表与未决点）。
> ⑥ **回归**：x86 全量 969 模块 sweep 拆两段跑完，**ok=931 / FAIL 32 /
>    TIMEOUT 6**，失败集合与 r10-x86 基线**逐项相同**（差集双向为空）→ 零回归。
>    工具侧 `aardio_vm_runtime.py` / `aardio_vm_tables.py` / `aardio_opcodes.py`
>    全部 exit=0；新增 `tests/aardio_vm_check.ps1`（纯 Python、不依赖 nodio.exe）
>    **AARDIO-VM-CHECK OK**（13 项断言，含「228 对条件跳转/0x22 配对落点
>    全部一致」这条样本级验证）。
> **下一步**：把 Nodio 的 `PNode` 体系与这套 32 位记录流对齐（翻译回 AST 比
> 再实现一个 aardio VM 更省事、风险更低），逐个消除 §7.8 的 8 个未决点。
>
> **第十一轮续（立项：兼容 + 超越）**：新建 `docs/surpass.md`，把「超越官方」
> 拆成**可验收**条目（每条写明官方行为 / Nodio 目标行为 / 验证方式 / 状态），
> 并**当场落地第一件官方根本做不到的事**：
> **官方预编译块可解释**。
> ① 运行期记录解码**做进引擎**（`engine/aardio_chunk.h/.cpp`）：
>    `ParseAardioChunkTree()` 保留整棵 chunk 树、`DecodeAardioRecord()` 按
>    §7.2 的**运行期位布局**解码单条记录、`DisassembleAardioTree()` 出反汇编、
>    统计记录种类直方图与跳转可解引用率。
> ② **import 的结构化诊断升级**：官方只会给「bad header」/「格式错误」，
>    Nodio 现在给「97 个对象 / 8734 条指令 / 2173 个常量（最大嵌套 4 层）；
>    记录流已按官方运行期布局解码：8734 条记录，用到 **43/61** 种记录种类，
>    非法种类 **0** 条，**631** 个跳转目标中 **631** 个落在指令边界上」。
> ③ **新增 CLI**：`nodio.exe -disasm <file>` 打印整棵 chunk 树的反汇编 +
>    种类直方图 + 跳转落点。**官方 aardio 只能执行自己的预编译块，
>    完全不能反汇编、给不出任何结构结论**。
> ④ 回归 `tests/aardio_chunk_check.ps1` 扩到 **26 项断言**（x64/x86 双绿）；
>    定向 sweep 确认两个 `=aardio` 模块的**失败集合未变**（只有文案变详细）。
> ⑤ `docs/surpass.md` 另列 5 项超越条目：S-1 预编译块可解释（✅ 本轮落地）、
>    S-2 运行期错误带文件名（❌ 待办 → ✅ 第十二轮落地）、
>    S-3 官方库自检与归因（✅）、
>    S-4 双架构构建（✅）、S-5 IDE 自举（✅）、S-6 记录流级缺口清单（⏳）。
> 本轮另修：并发会话改了 `app.editor.contextMenu` 的契约（现在收**编辑器组件
> 实例**而不是裸控件），导致 `tests/ide_ctx` 夹具过期打红 —— 已按新契约对齐
> 夹具（`CTX-SMOKE OK` 双架构）。
> **当前回归**（双架构实跑）：win_clip / aardio_chunk / import_contract /
> sysapi / ide_ctx / ide_smoke / ide_titlebar / aardio_vm 全绿，
> VM **90 passed / 0 failed / 0 skipped**，stdlib 全绿，publish 绿，
> ns **54/54**。
>
> **第十二轮续（S-2 落地：运行期错误带文件名与行号）**。
> 先做官方取证，**推翻了上一轮写进 `docs/surpass.md` 的结论**：
> ① 官方 `aardio.exe` 里存着一整张中英文字段表（`错误:` / `文件:` /
>    `错误行号:` / `字段:` / `失败操作:` …，配 `aardio：运行时错误` 等标题）
>    → **官方本来就有「文件 + 行号」**，只是躺在 IDE 弹窗式的多栏报告里；
> ② 官方文档 `docs/guide/ide/file.md` + 发布选项文案「在发布后的错误调试中
>    将无法再获取文件行号等调试信息」→ **发布时关掉调试就丢**；
> ③ 官方二进制里 GBK / UTF-16 / UTF-8 三种编码都扫过，`@ 行` **0 命中**
>    → `@ 行 N` 是 **Nodio 自己的**历史格式，不是官方格式。
>    所以 S-2 的定性从「超越官方」改成「**补齐官方已具备、但官方会在发布时
>    丢掉的能力**，并做成一行可被脚本 / LSP / CI 读走的文本」。
> ④ 实现（不动 `=nodio` 字节码格式、不给 `Value` 加字段）：
>    `Env::srcFile`（模块帧带自身路径，调用帧沿**定义帧链**继承）
>    + `Interp::strErrFile`（每语句刷新，报告时帧可能已退栈）
>    + 统一在**两个报告点**追加位置（`try/catch` 捕获点、顶层报告点），
>    错误生产者不再内嵌位置 → 位置**只出现一次**；
>    格式 `@ <文件>:<行号>`，拿不到文件才回退官方兼容形态 `@ 行 <行号>`。
> ⑤ 顺带修掉一个**官方契约不一致**：官方 `catch(e)` 拿到的是 `error()` 的
>    **原始载荷**（官方 `try.md`：「传递给 error 的任何信息都会被 catch 捕获」；
>    官方库 `com/activeX.aardio:63` 直接把 `e` 当错误信息返回），
>    Nodio 原来把它改写成 `运行错误: <载荷>` —— 本轮保留前缀（引擎错误通道是
>    纯字符串，官方允许抛表），但在**报告层**统一补位置。
> ⑥ **回归**：新增 `tests/s2_error_location_check.ps1`（8 项断言，x64/x86 双绿），
>    夹具 `tests/s2/{s2mod.aardio, caught.nodio, uncaught.nopio}` 覆盖
>    「主脚本 / 模块内 / 回调按定义帧」三种归属 + 「无文件回退裸行号」；
>    `tests/vmtests/t13/t14/t24` 期望值同步（`<SCRIPT>` 占位，harness 做路径归一化）。
>    全量：VM **90/90**（x64+x86）、stdlib、publish、ns **54/54**、
>    aardio_chunk、aardio_vm、win_clip、import_contract、sysapi 全绿。
>    **S-2 的零回归实证**：双架构全量 969 模块 sweep 重跑
>    （`.module-sweep-s2-x64.log` / `.module-sweep-s2-x86.log`）
>    → x64 ok=896（基线 897）、x86 ok=930（基线 931），两边的差异**都只有
>    `winex.desktop` 一项**，且它**单独复测 8.3–8.8 s**（sweep 的超时阈值
>    10 s，当时机器被 Ghidra 占用）→ 判定为**本来就贴着阈值的慢模块**，
>    空载下双架构都 `MOD_OK`；失败集合逐项 diff 除这一项外**完全相同**
>    → **零回归**。
>    顺带得到 S-2 的实证样张（原来只有裸行号，现在能直接按文件归因）：
>    `运行错误: 尝试调用非函数值 (.api) @ D:\aardio\lib\py2\dict.aardio:155`
>    —— 正是旧 `docs/surpass.md` 里拿来说明问题的那个例子。
>
> **第十三轮续（S-6 落地：把「Nodio 还缺什么」量化到记录种类）**。
> ① 新增 `tools/ghidra/DumpDisasm.java`（导出函数反汇编），
>    把官方解释器 `FUN_004C4BC0` 的 61 个 case 逐条读了一遍——
>    **helper 调用前的 `PUSH` 顺序即参数顺序**，于是
>    §7.8 悬了两轮的「4 个 get 变体 / 4 个 set 变体差异」**当场解掉**：
>    `0x05/0x07` 的对象来自**模块命名空间 / VM 暂存槽**、`C` 段是**键槽**；
>    `0x09/0x0A/0x0D/0x11/0x14` 则是 `C`=对象槽、`B`=键槽、
>    `A`=值槽；`0x0B INDEX` 同为 `C`=对象槽、`B`=下标槽。
>    同时确认了通用约定：`A`=目标槽、`C`/`B`=两个操作数（各自 bit8 置位
>    表示「外层作用域」）、`R18`=跳转位移。
> ② 引擎内置 **61 种记录的规格表**（`engine/aardio_chunk.cpp` 的
>    `kKindTable`：名字 / 操作数角色 / 语义 / 实现状态）。
> ③ `nodio.exe -disasm` 新增一节「**== 未实现记录种类 ==**」：
>    逐条列出 Nodio 还不能翻译/执行的记录种类，并给出可执行进度
>    （`可执行 0/61`）。**这张表在引擎里**，S-1b 每落一种进度就跟着涨，
>    缺口始终可量化（官方只能执行自己的预编译块，给不出任何这类结论）。
> ④ 回归 `tests/aardio_chunk_check.ps1` 扩到 **34 项断言**（x64/x86 双绿）。
> ⑤ 文档：`docs/aardio-bytecode.md` 新增 §7.5.1（操作数角色）、
>    §7.8 收口两项、§10/§11 回写；`docs/surpass.md` S-6 → ✅ 已落地。
> **下一步（S-1b）**：真正的难点已经不是「记录种类是什么」，而是
> **记录流 → AST 的结构编码**——记录流是扁平的槽机器，表达式/语句的
> 树形结构要靠槽数据流分析反推（已写进 §7.8 的未决点表）。
>
> **第十四轮（S-1b 第一阶段：官方校验器等价实现 + 操作数寻址模型定案）**。
> ① 把官方校验器三件套对上：`DAT_007C80A0`（每 kind 1 字节的**描述符表**）、
>    `FUN_004D4400`（**9 位操作数的两种寻址**）、`FUN_004D4450`（**校验器**）。
>    描述符字节的位含义实测为：bits 0..1 = 记录类别（三操作数 / 单 18 位 /
>    单 18 位有符号），bits 2..3 = C 操作数寻址类别，bits 4..5 = B 操作数
>    寻址类别，bit6 = 可作断点落点，bit7 = 后必须紧跟 `0x22`。
>    寻址类别：`0`=必须为 0、`1`=不检查、`2`=局部槽下标、
>    `3`=**bit8 ? 常量表下标 : 局部槽下标**。
> ② **推翻了第十三轮自己写下的「bit8 = 外层作用域标志」**：
>    真相是**常量表下标标志**。常量表在 chunk 帧 `+0x08`（16 字节/项），
>    常量数在 `+0x28`；`0x05/0x07/0x0C/0x0D` 的操作数是 **18 位常量下标**
>    （成员名），`0x34` 是嵌套对象下标。官方样本主对象有 **963 个常量**，
>    9 位下标根本不够——正是 18 位操作数让它们引用得下。
> ③ **「记录流 → AST 的结构编码」这个问题被撤销**：官方运行期没有 AST，
>    解释器 `FUN_004C4BC0` 直接执行扁平记录流。S-1b 路线因此定为
>    **「照官方做一个记录 VM」**，而不是反推树——工作量更小、语义更忠实。
> ④ 引擎内落地 **官方校验器的等价实现**：`engine/aardio_chunk.cpp` 的
>    `ValidateAardioChunkTree()`；`-disasm` 新增「**== 结构校验 ==**」一节，
>    import 阶段的结构化诊断也带上校验结论。
>    官方唯一样本上 **97 个对象 / 8734 条记录 / 0 违规**——
>    **官方 aardio 只能执行、给不出这份结论**（校验失败即抛
>    "in precompiled chunk" 终止）→ 超越项 `docs/surpass.md` S-1b。
> ⑤ 回归 `tests/aardio_chunk_check.ps1` 扩到 **38 项断言**（x64/x86 双绿）；
>    新增 Python 参考实现 `tools/aardio_validate.py`（与引擎实现互为验证）。
> ⑥ 顺带发现：**官方预编译块自带源码行号区间**（chunk 头两个 i32 = 首行/末行，
>    随嵌套顺序单调递增、区间不重叠）——为「预编译模块的运行期行号定位」
>    留好了接口（与 S-2 同源）。
> **下一步（S-1b 第二阶段）**：记录 VM 本体——16 字节槽表 + 常量表 +
>    闭包上值 + 命名空间 + 0x18 字节调用帧，然后逐条落地 61 种记录，
>    `可执行 N/61` 开始上涨。
>
> **第十四轮续（S-1b 第二阶段：记录 VM 本体落地）**。
> ① 新增 `engine/aardio_vm.h/.cpp`：记录 VM 内核——16 字节槽（值 + 官方类型
>    标签）、每 chunk 一份常量表、官方操作数寻址（9 位 bit8 = 常量表标志；
>    类别 1 的记录用 18 位常量下标）。
> ② **落地 30 种记录**：取值（`0x00`–`0x04`）、算术（`0x18`–`0x1D`）、
>    一元负号/真值/长度（`0x1E`–`0x20`）、跳转与条件跳转（`0x22`–`0x28`）、
>    返回（`0x2C`）、GC（`0x33`）、位运算（`0x35`–`0x3B`）。
>    `-disasm` 的进度随之从 `可执行 0/61` 变成 **`可执行 30/61`**
>    （进度表在引擎里，随实现自动上涨）。
> ③ 新增 CLI **`nodio.exe -runchunk <file>`**：执行一个官方预编译块并打印
>    结果。**官方 aardio 从不提供这种入口**——它只能执行自己编译的块。
> ④ 顺带定案「比较跳转的操作数角色」（§7.5.3）：`A` 不是目标槽，而是
>    **期望的比较结果**（0x23–0x26）或**真值槽**（0x27/0x28）；
>    `B`/`C` 是比较对象，0x27/0x28 的 `C` 是内联立即数（9 位）。
> ⑤ 新增 `tools/aardio_make_chunk.py`：按**官方格式文法**合成最小预编译块，
>    给记录 VM 提供可验证样本（官方样本 `aifix.aardio` 依赖命名空间/闭包/
>    原生函数，无法单独验证算术与分支语义）。
> ⑥ 回归：`tests/aardio_run_check.ps1`（21 项，x64/x86 双绿）——
>    3+4=7、1..10 求和=55、位运算=49、字符串长度=5、一元负号=-7；
>    未实现的种类明确报错（种类 + 记录号）；官方真实样本优雅失败不崩溃。
>    `tests/aardio_chunk_check.ps1` 扩到 **39 项**（x64/x86 双绿）。
> **第十五轮（S-1b 第三阶段：操作数角色定案 + 成员访问族落地）**。
> ① **操作数角色定案**（`docs/aardio-bytecode.md` §7.5.4），三路证据互证：
>    官方解释器调用点的压栈次序、官方校验器 `FUN_004D4450` 的取位次序、
>    官方唯一样本 `aifix.aardio` 的交叉验证（新增 `tools/aardio_ctx.py`）。
> ② **推翻了自己第十四轮写下的两处约定**：
>    · 二元运算的**左操作数恒在 `B`（bits 23..31）**——第十四轮按「`C`=左」
>      写的算术与位运算**左右反了**；
>    · get 家族（`0x09`/`0x0A`/`0x0B`）是**对象在 `B`、键在 `C`**，
>      set 家族（`0x11`/`0x13`/`0x14`）是**对象在 `A`、键在 `B`、值在 `C`**，
>      两者**不对称**（第十四轮与第十三轮都写反了）。
> ③ 记录 VM 新增**成员访问族** `0x05/0x06/0x07/0x08/0x09/0x0A/0x0B/0x0C/
>    0x0D/0x10/0x11/0x12/0x13/0x14/0x17` 与 **0x32 BUILD_ARRAY**，
>    并实现官方表语义（整数键走数组部分、字符串键走哈希部分、
>    命中为 null 回落到父表 = 官方表 `+0x08` → 引擎新增 `Table::parent`）。
> ④ `-disasm` 的进度从 `可执行 30/61` 变成 **`可执行 46/61`**（剩 15 种：
>    调用族 / 闭包 / 原生函数 / 导入成员）。
> ⑤ 回归：`tests/aardio_run_check.ps1` 扩到 **45 项**（x64/x86 双绿）——
>    新增 `sub`(10-3)、`div`(9/2)、`shl`(1<<4) 三个**顺序敏感**夹具，
>    以及 `member`/`member2`/`member3`/`index`/`method`/`array` 六个
>    成员访问族夹具；所有夹具都必须能被官方等价校验器判为合法。
>    `tests/aardio_chunk_check.ps1` 同步到 46 项（x64/x86 双绿）。
> **第十六轮（S-1b 第四阶段：调用帧与闭包落地）**。
> ① **调用约定定案**（`docs/aardio-bytecode.md` §7.5.5）：
>    被调值 = 槽 `A`；**实参窗口 = 槽 `A+1`..`A+B-1`**（第一个是 self）；
>    **返回槽 = 槽 `A+B`**（官方 0x2A 装参块 `0x004C5A44` 写
>    `vm->0x08 = &slot[A+B]`）；帧槽 0 = self、槽 1.. = 显式参数，
>    函数体的局部变量与参数共用同一片槽。
> ② **闭包语义定案**：捕获条数 = 嵌套对象的 `flags[0]`（运行时 +0x48），
>    紧随的 N 条 kind 3/4 记录是捕获项并被**消费**；上值**按引用**
>    （`FUN_004D5210` 的 cell 指向定义帧的局部槽）。
> ③ **官方样本全量自证**：`aifix.aardio` 的 96 个 `0x34` 记录逐个校验
>    「捕获条数 + 后继记录种类」→ **0 违规**；顺带纠正第十五轮把根的
>    `/1` 误记成 `/0/1` 造成的「捕获条数矛盾」假冲突。
> ④ 记录 VM 改为**帧栈**（闭包 = `RtClosure`、上值 = `RtCell` 盒），
>    落地 `0x0E` / `0x0F` / `0x29` / `0x2A` / `0x2B` / `0x34`，
>    `-disasm` 进度从 `可执行 46/61` 变成 **`可执行 52/61`**（剩 9 种：
>    原生函数 / 导入成员 / 字符串拼接 / 复合赋值 / 多值调用）。
> ⑤ 回归：`tests/aardio_run_check.ps1` 扩到 **55 项**（x64/x86 双绿）——
>    新增 `call`(10)、`params`(7)、`upvalue`(99) 三个调用与闭包夹具；
>    `tests/aardio_chunk_check.ps1` 同步到 52 项（x64/x86 双绿）。
> **下一步（S-1b 第四阶段续）**：原生函数（`0x15`）、导入成员（`0x16`）、
>    字符串拼接（`0x21`）、复合赋值（`0x2D`/`0x2E`）、
>    多值调用（`0x2F`–`0x31`、`0x3C`），
>    补齐剩下 9 种 → 官方 `lib/ide/aifix.aardio` 真正跑起来。
> **下一步（S-1b 第四阶段）**：调用帧与闭包（`0x29`–`0x2B`、`0x2F`–`0x31`、
>    `0x3C`、`0x34`）、原生函数（`0x15`）、导入成员（`0x16`），
>    补齐剩下 15 种 → 官方 `lib/ide/aifix.aardio` 真正跑起来。
>
> **下一步（S-1b 第三阶段）**：调用帧与闭包（`0x29`–`0x31`、`0x34`）、
>    命名空间与成员访问（`0x05`–`0x0D`）、数组与字符串构造（`0x21`、`0x32`），
>    补齐剩下 31 种 → 官方 `lib/ide/aifix.aardio` 真正跑起来。

Decisions locked with the project owner:

1. **Retire the MFC shell.** The official aardio IDE is an MFC application
   (confirmed via RTTI imports of the original `aardio.exe`). Rebuilding
   that shell in C++/MFC is the largest failure mode of the previous
   reconstruction: instead, the VM engine is extracted MFC-free (this
   repo). **The IDE is self-hosted: written in aardio, running on the
   Nodio VM** (`ide/nodio/`). The interim native Win32 + Scintilla C++
   shell and `thirdparty/scintilla` were removed once this decision
   landed — aardio-on-aardio gives a HWND-true form designer for free and
   makes the IDE itself the win.form/win.ui.ctrl compatibility suite.
   Primitives probed green on the engine: win.ui.menu, winform
   .setInterval, fsys.dlg, and the full process.popen pipe chain
   (`process.popen("cmd.exe /c echo …")` → read round-trip).
2. **x64 first, x86 retained for legacy DLL authors.** All new FFI work
   targets the unified Microsoft x64 ABI. The Win32 build stays available
   only because some legacy 32-bit-only third-party DLLs can never load
   into a 64-bit process (OS-level restriction).
3. **Publisher rebuilt in Phase 2** on top of this engine: binary-safe
   resource embedding (no UTF-8 re-encoding of PNG/ICO/DLL payloads),
   `.aproj` embed/ignored/local semantics, PE icon/version/manifest/UAC
   stamping, publish-time syntax gate. The old `AardioPublish.cpp`
   (43 MFC references) stays behind in the reference tree as evidence,
   it is not carried over.
4. **Compatibility boundary** (honest ledger):
   - 100%: aardio source language, official source-written stdlib, all
     Windows native API calls, `.aproj` project format, DSG form layout.
   - OS-physical limits: third-party **32-bit-only native DLLs** (run via
     a published x86 build), and closed pre-compiled `=aardio` bytecode
     blobs (original opcode encodings remain unrecovered).
5. **Engine and official library must reach 100% behavioural
   compatibility** (within the boundary in item 4). The gap is
   enumerated — never estimated — in the completion ledger below; every
   listed defect is on the critical path to 100%.

## Phase 1 — engine (this repo)
- [x] Tree-walking interpreter, 57-test regression suite green (x86 + x64)
- [x] MFC-free extraction (`engine/` + console host `apps/nodio/`)
- [x] x64 callback trampoline rewrite (shadow-slot spill + XMM save,
      RCX=ctx contract) — fixes the 0xC0000005 in EnumChildWindows-style
      callbacks
- [x] x64 per-signature JIT call stubs — float/double params now reach
      XMM0..XMM3 (`pow/fabs/sqrt` acceptance verified); float returns
      bit-cast from RAX
- [x] x64 struct `cbSize` auto-sync (WNDCLASSEX 48→80) + dangling-pointer
      fix in `StructSerialize`
- [x] UTF-8 byte-string value model (`#s` = bytes, `s[1]` = byte code)
- [x] x64 `winform.add` named-argument table (`name={cls;text;left;top;
      right;bottom}`) — was a 7-positional-arg signature that crashed x64
- [x] `arg` varargs binding (the engine bound `...` only) + empty-varargs
      reads as null so `if(!...)` recursion guards terminate
- [x] Memory DLL loading: `$"path"` binary inline + manual PE mapper
      (TLS callbacks unsupported, matching aardio's own limitation)
- [x] Numeric `for`: optional parens, empty condition slot, variable /
      postfix / comma-separated step slots, `++=` concat assignment
- [x] Falsy coalescing `:` (`0 : 123` → 123) + multiline `or` operands
- [x] `raw` primitive namespace completed (`len`/`uint`/`topointer`/
      `tostring`/`copy`/`concat`/`realloc`/`equal`/...) — `builtin/raw.aardio`
      and `gdip` depend on these
- [x] `+` string concatenation per the docs (non-numeric operand → concat)
- [x] Raw-string doubled-quote escape (`` `a``b` `` → ``a`b``, `"a""b"`)
- [x] `union` members in `class` with forward class references
- [x] API/struct type aliases (`ustr`, `bstr`, `ptr`, `lpcstr`, ...)
- [x] Comment string: a comment right of `=` IS a string literal
      (`var css = /** ... **/`, `= //line`, docs/datatype/string.md §4)
- [x] Star-count-matched block comments: `/*** … ***/` may contain
      `/*_ERR*/`-style inline comments without closing early, and
      `/**intellisense(…) … **/` ends at its own `**/` rather than the
      first inner `*/` (`inet/errorMessage`, `autos/skills`, `dotNet/v2`)
- [x] Keywords are names: `namespace class{…}`, `import java.class;`,
      `class java.object` (`java/*`)
- [x] Implicit table argument: `f( name = v; ["k"] = v2 )` — the shipped
      lib passes named tables through the argument list without braces
- [x] Statement labels (`EACH:`) and `break LABEL` / `continue LABEL`
- [x] Multi-target assign with holes and root-qualified targets
      (`files,,dirs = …`, `..response,..request,..session = …`)
- [x] `lambda`/`λ` body, `while` without parens, `if(` before a break,
      `? :` / `&&` / `||` / `|` / `<` operands on the next line
- [x] `=aardio` compiled bytecode modules are detected and reported as a
      format error rather than a bogus syntax error (`ide/aifix.aardio`)
- [x] `=aardio` **format** fully reverse-engineered (15-byte header, chunk
      grammar, constant pool, 32-bit RISC opcode encoding); parser +
      re-serializer reproduce the official sample byte-for-byte
      (`tools/aardio_bytecode.py --verify`, `docs/aardio-bytecode.md`).
- [x] `=aardio` **container parser inside the engine** (`engine/aardio_chunk.cpp`):
      importing an official precompiled module now reports a structured
      diagnostic (object / instruction / constant counts) instead of a generic
      "unsupported bytecode format" (`tests/aardio_chunk_check.ps1`).
- [x] Official VM internals exported from `aardio.exe` (Ghidra headless):
      expression-node priority table (26 x 2 bytes), the official builtin
      dispatch table (41 named builtins -> handler addresses), 10/64 record
      semantics and 20/64 validator constraints (`tools/aardio_vm_tables.py`,
      `docs/aardio-bytecode.md` §6.1–§6.4).
- [x] Official VM architecture identified: the VM is an **AST tree walker that
      emits a 32-bit record stream** (`FUN_004EC700` main dispatch,
      `FUN_004F5B20` record emitter) — not a RISC instruction VM. Exported the
      AST-node-type -> 26-operator-kind map (`FUN_004EE2E0`) and all 83 record
      emit sites. Corrects §5 of `docs/aardio-bytecode.md`.
      Remaining: the exact meaning of each of the 64 record kinds — still not
      executable, so the physical boundary stands for execution only.
- [x] Leading-`++` line continuation for long SQL/XML assembly, while a
      leading `..` stays the root-namespace accessor
- [x] `io.exist(path,mode)` returns the **absolute path** (null when
      missing), not a boolean — the library idiom
      `io.exist(p) : fallback` only works because the truthy result IS the
      path (`zlib`/`golang`/`nodeJs`/`py2`/`py3` resolve bundled DLLs so)
- [x] `io.fullpath` honours the aardio path conventions (`~…` exe root,
      `/…` `\…` app root, `//…` drive root, `\\…` UNC → null) instead of
      collapsing every rooted path to the working directory
- [x] `io.remove(path)` file deletion (the library calls it; it was absent)

### Library parse coverage (measured over all 869 official `.aardio` files)
| arch | parsing | note |
|---|---|---|
| x64 | **868 / 868 source files (100%)** | `NODIO_CHECK` batch parse sweep; 1 file is `=aardio` bytecode, not source |
| x86 | **868 / 868 source files (100%)** | identical |

### Library import coverage (54 top-level namespaces, enumerated by
### `tests/ns_sweep.sh` from the official tree; measured 2026-10-17)
| arch | importing | note |
|---|---|---|
| x64 | **45 / 54 (83.3%)** | 9 处未导入：7 个 32 位 DLL 物理限制 (base64/zlib/golang/nodeJs/py2/py3/qrencode/sqlite) + 1 个 ide.command.join 初始化要求 |
| x86 | **52 / 54 (96.3%)** | 仅 2 处：ide (需主线程命令管道) 与 sqlite (官方 sqlite.base 禁止裸导守护)；System/golang/py2/py3/zlib/base64 全部通过 |

The remainder is specific, not vague — and the earlier "environmental
ceiling" claim was wrong: it hid real engine defects (`~` path resolution,
the `io.exist` return contract, custom API types).

| namespace | cause | next step |
|---|---|---|
| `base64`, `sqlite`, `zlib`, `golang`, `nodeJs`, `py2`, `py3`, `qrencode` (x64 only) | 依赖 32 位 DLL (zlibwapi.dll, python27/38.dll, MZ payloads) | 64 位进程物理限制，由 x86 构建承接 (x86 下全部通过) |
| `ide` (both arch) | 需要宿主 ide.command.join() 主管道已建立 | 在自托管 IDE 环境中运行验证 |
| `sqlite` (x86) | 官方库 `sqlite.base` 明确要求通过具体驱动 (如 sqlite.aes256) 导入 | 驱动子库级测试 |

Current state in one line: **source parsing 100%**, **import 45/54 (x64) /
52/54 (x86)**, residue enumerated above; behaviour-level completion is
tracked in the ledger below.

### Completion ledger (denominator pinned 2026-10-14)

Aggregation weights are for reporting only and may be re-weighted:
engine 40 / library 25 / publisher 15 / IDE 15 / designer 5.

| area | weight | score | measured evidence |
|---|---|---|---|
| engine (language + VM + FFI) | 40 | **96%** | parse 868/868 both arch; VM regression **78/78 both arch**; ns_sweep 45/54 (x64) / 52/54 (x86); Python3 全链路双向互操作通过; COM host + CLR host 加载通过; COM ActiveX 内嵌宿主 (com.CreateEmbed / _host / _form 反向绑定) 落地; CLR host 动态 C# 编译器 (dotNet.createCompiler) 与程序集 new/import 实例创建/静态调用全链路打通; AppDomain CCW 元数据分发 (ToString/Id/FriendlyName/BaseDirectory) 落地; 闭包词法 owner 捕获 (f.ownerVal) 打通 .NET 构造回调链 (System.DateTime / time.lunar); timeMeta 属性/方法重载分发修复; cdata/gcdata 元表解包与 _topointer 机制打通; math.dist/io.createDir 原语补齐; io.localpath file:/// 规范化修复 |
| library (behaviour) | 25 | **46%** | dotNet / System 全家桶 (System.Collections.ArrayList / System.DateTime / System.IO.FileStream / System.Windows.Forms / System.Drawing) 真实运行通过; GUI 控件链 (LOGFONT、createIndirect、getFont/setFont、DSG 属性优先级、plus.skin) 实测通过; web.form / web.form.chat / web.form.simpleMarkdown 控件初始化与内嵌成功; time.lunar / time.ganzhi 完整格式化实测通过; DeepSeek OCR 等实机 GUI 窗体正常拉起进入消息循环; 嵌套命名空间作用域隔离修复 |
| publisher (Phase 2) | 15 | **100%** | closed; fixtures green x64 + x86 |
| IDE (Phase 3, self-hosted) | 15 | **45%** | DSG shell + modular `app/*` + project model (`.nproj`/`.aproj`) green (`tests/ide_smoke.ps1`); chrome 自绘化（文档页签 / 工具条 / 三栏 dock / 设计画布底板）；**图标组件体系**（FontAwesome 字体图标 + GDI+ 手绘控件缩略图）；**DPI 缩放对齐**（`nodio.ui.dpi`，150% 下自绘层与原生控件同字号）；**`nodio.gui` 控件体系**（移植 Zan.Gui 的 Theme→Style→StyleBox→Widget，右栏「组件」页展示全套控件）；**工程树/问题列表/右栏列表改自绘**（原生 listview/treeview 配色会崩进程）；**自绘标题栏**（第九轮：菜单并进标题栏一行，摘掉原生浅色标题栏 + 满幅客户区 + 自绘拖动/窗口按钮）；第八轮修掉三个语义级 bug（`string.find` 是正则 / 空串是真值 / `io.readFile` 返回原始字节 → 中文源码不再乱码）；**修掉自绘控件 z 序吃鼠标事件的通病**（背景板压住所有面板 → 页签/树/标题栏点不动）；`nodio-lsp`/`nodio-dap` skeletons negotiate capabilities over stdio; IDE-side parser wiring, highlighting and the debug channel still open |
| form designer (Phase 4) | 5 | **45%** | 8 手柄缩放 / DSG 往返（类型保持、输出稳定）/ 属性格回写（含几何）/ 自绘工具箱 已通；**设计坐标改逻辑像素**（与官方 DSG 约定一致，150% 下所见即所得）；自定义组件可经 `nodio.designer.registry` 注册并落到画布；画布/工具箱的鼠标交互随 z 序修复恢复；缺 网格对齐 / 事件代码生成 / `switchActiveForm` / `ide.customCtrl` 映射 |

**Overall ≈ 75.4 / 100**（第九轮 自绘标题栏 + 自绘控件 z 序修复后；此前 74.2）。
Per decision 5, the engine and the official library must reach 100% — the defect list below is that path:

Fixed this session (pinned by t63–t80, sys_dt, lunar, and ideprobe):
- **COM ActiveX In-Place Embedding** (`com.CreateEmbed`): ATL ActiveX hosting via `AtlAxAttachControl`, `_host` methods (`close`, `adjust`, `tranacc`), reverse `wb._form` binding;
- **Closure Lexical Owner Capture** (`f.ownerVal`): bare callback closures now preserve defining lexical `owner`, resolving .NET constructor callbacks losing `owner` (`System.DateTime(tm)` NullReferenceException resolved);
- **time MetaProperty Resolution**: `timeMeta` `_get`/`_set` dispatch and string `format` property overload, allowing `lunarDate.format("%Lyyyy-%Lmm-%Ldd %H:%M")` and standard date formatting to co-exist without collision;
- `class` instantiation dropped the defining namespace upvalue
  (`in_addr` bare calls, 15+ examples) — t63
- dotted delegator imports (`win/form.aardio` = one `import win.ui;`
  line) clobbered the class the module body built — t65; win.ui.ctrl
  inventory now 30/30 import+instantiate
- natural struct alignment (MSVC default packing) — SECURITY_ATTRIBUTES
  was packed sequentially (16 bytes on x64) and CreatePipe rejected it;
  t66 pins both archs
- **kernel `io.file` class** (path/handle ctor, byte-string + line +
  struct reads, write multi-arg, seek/tell/size/eof/mode, stdout/stderr/
  stdin, io.open alias, raw.closeHandle/_release, table.unpack/unpackArgs)
  — t67; `process.popen` round-trip green (`echo` → read)
- string by-ref API params, `#null = 0`, `raw.buffer` pointer form,
  `table.range`, `string.concat` — t53/t64-era batch

Fixed & closed in recent sessions:
1. [x] runaway recursion depth-guard contract (16MB host stack + depth-guard threshold 500, catchable error, t70)
2. [x] `thread.getMainId` primitive (`import ide` unlocked on both arch, t69)
3. [x] `string.trimRight`/`string.trimLeft` primitives (x86 py2/py3 unlock, t68)
4. [x] CLR host for `System` and `dotNet.import`: CCW vtable mapping, `dotNet.createCompiler("C#")` dynamic compilation, `System.Collections.ArrayList`, `System.DateTime`, `System.IO.FileStream`, `System.Windows.Forms` full chain verified.

Still open (the remaining path to 100%):
1. language-host primitives: py2/py3 (x86), `golang` import self-guard
2. console.log string-encoding primitive layer (isUtf16/getUtf/toUtf16 semantics), `.join`/`.getMessage` gaps
3. io.file refinements toward official text-mode semantics (CRLF translation is currently the caller's job; fds are always binary)

Behaviour buckets of the 444 sweep errors (named clusters; a long tail
beyond them): missing hosts ~55 (Java 40, CLR 15), 32-bit-only DLLs on
x64 ~76 (excluded from the denominator per the boundary), GUI-interactive
~39 (headless cannot validate; re-measure inside the IDE), engine defects
= items 1–5 and parts of the `.status/.slice/.cmp/.call/.process` and
`无效参数` clusters.

## Phase 2 — publisher (this repo, `publish/`)
- [x] Binary-safe payload embedder + `.aproj` embed/ignored/local rules
- [x] PE stamping: icon / version info / DPI-aware manifest / UAC
- [x] Publish-time syntax gate + dependency closure for `lib/`
- [x] Publish fixtures (`tests/m7/proj`, t49) + memory-DLL-from-payload
      assertions (x64 + x86 green)

## Phase 3 — self-hosted IDE (`ide/nodio/`, aardio on the Nodio VM)
- [x] VM introspection API for `(debug)` (`queryinfo`/`traceback`/
      `callcount`/`hook`/`open`/`close`, engine-provided named module) —
      `import debug` green; `getMainId` still missing and blocks `import ide`
- [x] Engine primitives for the IDE probed green: win.ui.menu,
      winform.setInterval, fsys.dlg, and the full process.popen chain
      (F5 run pipeline unblocked; probe in the session log)
- [x] M0-1: main window in aardio — DSG (`winform.add`) layout (tree /
      richedit editor / richedit output / status), menu, F5 via
      process.popen; modularized under `app/` (shell / editor / output /
      project / runner / lspClient / dapClient); pinned by
      `tests/ide_smoke.ps1`. Module map in `docs/architecture.md`.
- [x] M4: project model — the IDE opens `.nproj` (Nodio-native) or `.aproj`
      (official, same XML schema), builds the file tree and resolves the
      start file; the IDE is its own project (`ide/nodio/ide.nproj`)
- [x] M2 skeleton: `apps/nodio-lsp` — LSP over stdio, capabilities +
      framing; wiring the parser to diagnostics/completion/symbols next
- [x] M3 skeleton: `apps/nodio-dap` — DAP over stdio, capabilities +
      handshake; wiring the debuggee control channel onto `(debug)` next
- [x] **GC 根可见性修复（2026-10-20 第三轮）** — "很严重的内存泄漏"的真正根因
      不是 GC 频率，而是两个引擎缺陷：①自动 GC 的哨兵值把"未初始化"和"关闭"
      混为一谈，**自动回收全程没跑过**；②`Env` 只有向上的 `parent` 引用，
      **正在执行的块作用域/模块体/主脚本帧对收集器不可见**，被当垃圾清空
      （官方 `win.loopMessage` 的 MSG 结构就是这么被释放的 → 消息循环空转 →
      窗口收不到任何消息）。修法：把 10 处 `Env` 创建点登记为 GC 根
      （`Interp::GcFrameScope`），并给帧栈加不变式断言 `GcFrameCheck()`。
      详见 `docs/capability-gap.md` §1.5
- [x] **IDE 启动阻塞修复** — `main.nodio` 补 `import app.library;`；
      `library.nodio` 的 `for(...,1++)`（对常量自增）步进改回 `1`；解析器
      拒绝给常量自增（明确报错而非静默死循环）；`tests/ide_smoke.ps1` 改为
      严格判定（stdout 为空即失败、按 UTF-8 读取）。实测 `SMOKE OK`（exit 0）
- [x] **fiber 执行上下文随 fiber 迁移** — fiber 走 Win32 fiber（每条独立
      C++ 栈），而帧栈/根栈/epoch 栈/控制流标志是**线程级共享**的：fiber 在
      一次调用中间 yield 时帧留在恢复者栈顶，恢复者 `pop_back()` 弹掉别人的
      帧 → GC 根集错乱（与"运行中的帧被回收"同族）。修法：新增 `FiberCtx`
      承载全部栈式状态，`Interp` 持 `mainCtx`/`curCtx`/`fiberCtxs`，
      `FiberCtxSwitch()` 在 `FiberResume`/`FiberYield`/`VMFiberEntry`
      三个切换点迁移状态；挂起中的 fiber 登记为 GC 根；被遗弃的 fiber
      自动释放（脚本值用**弱引用**记录，否则自环导致裁剪永不触发）。
      顺带修掉嵌套 resume 交错控制权。新增回归 `t89` 固化语义。
      VM 回归 **86 passed / 0 failed / 1 skipped**
- [x] **设计器组件层：窗口子类化 + 控件几何** — 官方 `win.subclass` 依赖
      `::SetWindowPointer`，而引擎的 `::` 不解析裸系统 API（E-10），画布的
      鼠标交互其实一直没生效。新增标准库组件 `nodio.ui.subclass`：用
      `raw.loadDll().api()` 显式签名自实现，**全进程共用一个** trampoline
      按 hwnd 分发，窗口销毁自动还原 —— 顺带消掉"每控件一份回调 = 永久
      泄漏"（官方实现的 `raw.tostdcall` 回调进 `g_cbs` 永不回收，闭包连同
      捕获的一切被永久钉住）。引擎补 `win.setPos`/`win.getPos` 与控件表的
      `setPos`/`show`；修掉 `RegisterFormClass()` 的局部 `bDone`（第二次
      `win.form` 必然失败）。详见 `docs/capability-gap.md` §1.7 / §1.8
- [x] **裸 `::` 系统 API 解析（engine/sysapi.cpp）** — 新增系统 API 签名表
      （user32/gdi32/kernel32 约 90 个，含 `::SetWindowPointer` →
      `SetWindowLongPtrW` 这类别名），`::` 查表命中后按签名解析并缓存。
      官方 `win.ui` 的**窗体级**事件路由因此恢复（此前控件能画但收不到
      任何鼠标消息）
- [x] **工具箱 / 属性格自绘化** — 引擎没有 treeview/listview 的
      `insertItem/addItem`，原生面板根本跑不起来；两个面板改成
      `nodio.ui.board` + gdip 自绘（分类可折叠、值列可就地编辑并即时
      回写 `onChange`）。`nodio.ui.board` 的鼠标事件走自己的窗口子类化，
      不依赖官方 win.ui 的控件级路由（那套装不上窗口过程）
- [x] **编辑器组件（`nodio.ui.editor`）** — 标准 IDE 该有的那一圈东西补齐：
      行号栏（与 richedit 行位置严格对齐）、aardio 语法着色、当前行高亮、
      行列号回调、Tab/Shift+Tab 缩进、Ctrl+/ 注释切换、查找替换浮条、
      撤销/重做、脏标记。底座用官方 richedit（选区/剪贴板/IME 白送）。
      **撤销栈自己实现**：richedit 自带的只收键盘输入，程序化的行替换
      （缩进/注释/替换）根本进不去。详见 `docs/capability-gap.md` §1.12
- [x] **文档闭环** — 新建 / 打开 / 另存为 / 保存 / 全部保存 / 关闭标签页，
      脏标记由内容推导（当前文本 vs 上次保存的文本），关闭脏文档时弹
      「保存-不保存-取消」。系统「打开/另存为」对话框自己手写
      `OPENFILENAMEW`（官方 `fsys.dlg` 的结构体在 Nodio 上错位 8 字节，
      `GetOpenFileName` 立刻失败）
- [x] **命令面清零** — 菜单与快捷键全部接真实命令，消灭「未实现」占位：
      文件（新建/打开/保存/另存为/全部保存/关闭/退出）、
      编辑（剪切/复制/粘贴/全选/撤销/重做/查找/替换/转到行）、
      格式（增加缩进/减少缩进/注释切换）、转到（转到行/问题列表）。
      快捷键：Ctrl+N/O/S/W/F/H/G、Ctrl+/、Tab/Shift+Tab、F5/F10/Ctrl+F9
- [x] **IDE 视觉回归补测** — 新增 `tests/ide_shot_screen.ps1`（全屏截图，
      模态对话框不在 IDE 窗口矩形内时用）；`tests/ide_smoke.ps1` 与
      `tests/ide_titlebar.ps1` 补 UTF-8 BOM（PS 5.1 读无 BOM 的中文脚本
      会解析失败）

## Phase 4 — visual form designer (aardio code inside the IDE)
- [x] **8 手柄缩放 / 拖动** — 选中框与 8 个手柄画在**底板**上（手柄放组件外侧、
      选中框外扩 1px，否则被组件遮住）；拖拽增量用**屏幕坐标**
      （`win.getRect(hwnd,true)` + lParam）避免控件跟随导致的漂移
- [x] **DSG `/*DSG{{*/ ... /*}}*/` 往返解析器/序列化器** — 解析按 `;` 切分并用
      **奇数引号合并**处理串内分号；引号串保持字符串、裸值转 number/boolean；
      生成时字符串加引号（`"` → `""`）、数字/布尔裸写，按 key 排序保证输出稳定
      （`DSG-ROUNDTRIP:true`、`DSG-TYPES:left=number text=string`）
- [x] **Toolbox + property grid** — 两者都是**自绘**（`nodio.ui.board` + gdip）：
      工具箱按分类分组可折叠（14 个内置控件）；属性格两列自绘 + 就地编辑，
      编辑值**真正回写**控件属性（text/bool/number/通用）与**几何**
      （left/top/width/height → 改矩形并重排）
- [x] **chrome 自绘化**（2026-10-20 第五轮）— 文档页签自绘（`nodio.ui.tabstrip`，
      带 × 关闭标记与强调下划线）、工具条顺序固定（官方哈希表遍历顺序不确定）、
      右栏/底栏 dock 改为**无子窗体**（`mode="child"` 带标题栏、
      `border="none"` 会把 `WS_CHILD` 覆盖成 `WS_POPUP`）
- [x] **控件方言统一**（`nodio.ui.rect`）— 官方 `win.ui.ctrl` 的 `setPos(x,y,宽,高)`
      与引擎原生控件表的 `setPos(l,t,r,b)` **语义不同**、`text` 一个是属性一个是函数，
      混用正是布局错乱的根因。新增 `nodio.ui.rect` 统一走原始 `win.setPos`（物理像素）；
      所有 `form.add` 一律传 `autoResize=false`（官方默认会按 `_defRect` 把控件弹回原位）
- [x] **外观主题体系**（2026-10-20 第六轮）— 外观基准换成
      `aar.chengxu.online/thread-903.htm` 的**现代深色 IDE**（参考图留档
      `docs/ref/aar-ide/`，规格见 `docs/official-ide-reference.md` §0）。
      新增标准库 `nodio.ui.theme`（深色/浅色两套配色 + 切换 + ARGB→COLORREF）；
      页签条扁平化（顶部强调描边）、**工具条与状态栏整条改为自绘**、
      工具箱/属性格改主题配色、窗体底色用 z=0 自绘底板、
      编辑区/输出/调试/终端用官方 richedit 的 `setCharformat` 染深色
- [x] **设计画布随 DPI 缩放铺满编辑区** — 官方 `onSize` 回调是**跨作用域**调用，
      `app.shell` 的模块钩子在那条路径上读出来是 `null`（实测 `type()` = null），
      覆盖层收不到重排通知；改由 `main.nodio` 的 300ms 心跳比较客户区尺寸后重排
- [x] **图标组件体系**（第七轮）— 字体图标用官方库自带的 FontAwesome 4.7
      （`fonts.fontAwesome.family` + `createFont`，码点表在
      `nodio.ui.icons`；注意 aardio 双引号是原样串，码点必须写成 `'\uF002'`）；
      工具箱的控件缩略图没有现成字体图标，用 GDI+ 按 16×16 设计稿手绘
      （`drawWidget`，按 16 归一化后乘缩放系数，任何 DPI 下形状一致）。
      已接入工具条（纯图标按钮 + 悬停提示）、工具箱、文档页签、左右底三栏页签
- [x] **DPI 缩放对齐**（第七轮）— 官方 `win.ui` 控件矩形是**逻辑像素**
      （官方库自己乘 `dpiScaleX`），我们自绘层走 `win.setPos` 是**物理像素**，
      GDI+ 字体默认 `_GdipUnitPixel` 也不随 DPI 变 → 150% 下自绘层比原生控件
      小一圈。新增标准库 `nodio.ui.dpi`（`dpiScale()` / `px()` /
      `dpiSetScaleFrom()`），组件层统一口径：**绘制用物理像素、设计常量按逻辑
      像素写并用 `px()` 换算**；设计画布的组件矩形也改成逻辑像素（与官方 DSG
      约定一致，设计器所见即所得）。顺带修掉三个真 bug：dock 页签图标丢失
      （`addTab` 对已存在页签早返回时丢了 `tParam.icon`）、状态栏分段用序号
      定位导致工程路径写错格子、board 字体缓存只有一份。详见
      `docs/capability-gap.md` §1.11.6 / `docs/ide-components.md` §0.6
- [x] **GUI 控件体系 `nodio.gui`**（第八轮）— 把
      `D:\project\zan-lang\packages\Zan.Gui` 的 **Theme → Style → StyleBox →
      Widget** 架构移植成 aardio 版（`lib/nodio/gui/`）：
      `token`（尺度令牌）/ `theme`（语义角色）/ `style`（样式盒解析）/
      `ctx`（绘制上下文）/ `widget`（控件基类 + 事件宿主）/
      `button`·`checkbox`·`radio`·`label`·`input`·`panel`·`badge`·`progress`·
      `separator` / `gallery`（组件展厅）。
      样式规格里颜色写**语义角色名**（`"primary"` / `"bgHover"` …），
      解析时按当前主题换色 —— 换主题不用改控件代码；
      `nodio.ui.theme` 降级为兼容层（语义角色 → 旧字段名），
      旧组件一行不用改。IDE 右栏新增「组件」页展示全套控件。
      详见 `docs/ide-components.md` §0.8
- [x] **第八轮三个语义级 bug**（都在 IDE 侧，不是引擎缺陷）：
      ① 官方 `string.find` 是**模式匹配**（正则），字面包含判断必须用
      `string.indexOf` —— 原来的 `!string.find(pre,".")` 永远为假，
      **大纲列表恒空**；② aardio 里**空串是真值**，`if(!name)` 拦不住 `""`，
      注释行全变成空节点，必须用 `#name == 0`；③ 官方 `io.readFile` 返回
      **原始字节**（写却是 UTF-8），中文源码打开整篇乱码 —— 改用
      `string.load(path)`。顺带加 `_StringFormatFlagsNoWrap`（自绘文字不再
      折行串行）、`TrackMouseEvent`（悬停态能正确复位）。
      详见 `docs/capability-gap.md` §1.11.7
- [x] 工程树 / 问题列表改自绘（原生 treeview/listview 的配色消息在本机 comctl32
      里会解引用 lParam → **进程崩**，见 capability-gap.md §1.11.4）
- [x] **自绘标题栏（第九轮）** — 原生标题栏是浅色的，跟整套深色 chrome
      不是一回事。新增 `lib/nodio/ui/titlebar.aardio`：**菜单栏并进标题栏一行**
      （左：应用标记 + 菜单；中：当前文档 - 应用名；右：最小化/最大化/关闭），
      与参考 IDE（aardioX / QuickEditor4）一致。
      做法：show 之后 `modifyStyle` 摘掉 `WS_CAPTION`（保留 `WS_THICKFRAME`，
      仍可拖边改大小）→ 子类化 `WM_NCCALCSIZE` 把客户区扩到整个窗口矩形
      （消掉 Windows 10+ 那圈 10 物理像素的浅灰可缩放边框；**最大化时不接管**，
      否则内容会被顶出屏幕）→ `WM_NCHITTEST` 让标题栏整条回到客户区，
      拖动窗口自己实现（光标屏幕位置 + 增量 `SetWindowPos`）→ 菜单与下拉菜单
      全自绘（独立画板 + 鼠标捕获，点外面 = 取消）。
      顺带定位到一个**通用真 bug**：官方 `form.add` 一次只加一个控件，
      控件层次 = **添加顺序**，铺满客户区的背景板是最早加的 → 压在所有
      自绘板上面，**把所有鼠标事件都吃掉**（页签/树/标题栏点不动）。
      修法：背景板显式 `HWND_BOTTOM`，标题栏与下拉菜单显式 `HWND_TOP`。
      回归：`tests/titlebar_probe.aardio` + `tests/ide_titlebar.ps1`
      （DRAG-OK / MENU-OPEN / MENU-FIRED / PROBE-OK）。
      详见 `docs/ide-components.md` §0.9 / `docs/capability-gap.md` §1.11.8
- [x] 菜单栏（文件/工程/编辑/选择/格式/转到/查找/显示/运行/视图/设置/帮助）
      —— 与标题栏合并成一行自绘（上一条）
- [x] **工程工作流闭环（第十轮）** — 工程树从「`.nproj` 清单平铺」改成
      **实时文件系统目录树**（目录 + 文件都扫磁盘，新建的东西立刻可见；
      扫描有深度 4 层 / 1200 节点上限）；右键菜单扩到
      **打开 / 复制路径 / 新建文件 / 新建目录 / 重命名 / 删除 / 设为启动文件**；
      磁盘操作后**同步 `.nproj` 清单**（`app/project.nodio` 的
      `addFile/removeFile/renameFile/renameDir/removeDir/save`），
      新文件才会进发布包；删除走 `win.msgbox` 确认（过 `modalBusy` 闸门），
      重命名后页签跟着改，删目录后关掉里面打开着的文档。
      左栏「搜索」页从「一个空的官方 edit」换成**真全文搜索**
      （`lib/nodio/ui/searchPanel.aardio`：关键字输入条 + 结果表
      文件/行/内容 + 双击跳到那一行，Ctrl+Shift+F）。
      顺带修掉一个**通用真 bug**：`reghotkey` 的修饰键掩码写成了
      `0x8`/`0x11`（Win32 是 `MOD_CONTROL=2 / MOD_SHIFT=4`），
      **Ctrl+S / Ctrl+N / Ctrl+O / Ctrl+W / Ctrl+F9 一个都没注册上**；
      已全部改正并补上 Ctrl+F / Ctrl+H / Ctrl+G / Ctrl+Shift+F。
      详见 `docs/ide-components.md` §4 / §14、`docs/capability-gap.md` §1.13
- [x] **LSP 诊断真正接进 IDE**（2026-10-20 第九轮）— 槽内**波浪线** + 错误色 +
      行号栏**诊断圆点** + 底部问题列表 + 双击跳转。
      根因级发现两条：① 官方 `richedit.setCharformat` **从不加
      `CFM_UNDERLINETYPE` 掩码位** → 控件忽略 `underlineType` → 只画单线，
      出不来波浪线；② **`LSP_EXE`/`DAP_EXE` 拼成了 `nodio.exe-lsp`**（不存在）
      → **LSP 与调试器从来没起来过**（问题列表一直是空的）。
      详见 `docs/capability-gap.md` §1.14
- [x] **代码折叠**（2026-10-20 第十轮）— 行号栏折叠标记（▸/▾，点即折叠）、
      菜单「格式 → 折叠/展开 / 全部折叠 / 全部展开」、快捷键 `Ctrl+Shift+[`。
      折叠靠 CHARFORMAT2 的 **`CFE_HIDDEN`**：隐藏的段落**连行高一起收起**
      （不用自绘覆盖层），文本本身不丢。
      ⚠ 折叠后官方 `lineToChar/lineFromChar/lineRange` **返回错的位置**
      → 组件自维护 `lineStarts`/`visLines`，行号栏按可见文档行画。
      详见 `docs/capability-gap.md` §1.15、`docs/ide-components.md` §11.7
- [ ] 双栏对比（编辑器仍是纯 richedit）
- [ ] 属性格分组带（基本/位置/外观/样式，折叠箭头）
- [ ] 网格 / 边界线 / 对齐 / 缩放 / 改窗体设置（官方字符串表 32984-32991 的命令）
- [ ] 事件代码生成
- [ ] 设计 ↔ 源码视图切换（`switchActiveForm`）
- [ ] 自定义控件替身映射（`ide.customCtrl` 的 `cls→drawAs` 表）
