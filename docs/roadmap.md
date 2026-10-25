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
> ⑤ 回归：`tests/aardio_run_check.ps1` 扩到 **58 项**（x64/x86 双绿）——
>    新增 `call`(10)、`params`(7)、`upvalue`(99) 三个调用与闭包夹具，
>    以及 `recurse`(120)：闭包先存进模块命名空间、再用 `GET_MEMBER` 取回
>    「自己」递归调用，一次跑通闭包进表 / 从表取回 / 属主命名空间可见 /
>    嵌套帧。`tests/aardio_chunk_check.ps1` 同步到 52 项（x64/x86 双绿）。
> **第十七轮（S-1b 第四阶段续：命名空间与导入落地）**。
> ① **命名空间与导入定案**（`docs/aardio-bytecode.md` §7.5.6）：
>    `0x15 NEW_NATIVE` = **新建一张空的命名空间表**（type 5，父表 = 全局内建表
>    `DAT_007C89B0`，成员由随后的 `SET_MEMBER3` 填）；
>    `0x16 IMPORT_MEMBER` = **名字在 B 操作数**（bit8=1 → 常量名）、
>    **目标槽在 C**（官方描述符 `DAT_007C80A0[0x16]=0x38` 与反汇编
>    `0x004C62E6` 一致），导入后**模块命名空间切到被导入的命名空间**。
> ② **纠正一处语义偏差**：`vm+0x40`（VM 暂存槽）是**全局命名空间**，
>    不是模块命名空间；模块命名空间与 `0x15` 建出的表，父表都是它。
> ③ 记录 VM 新增 `globalNs` 与 `0x15` / `0x16` 两个 case，
>    `-disasm` 进度从 `可执行 52/61` 变成 **`可执行 54/61`**（剩 7 种：
>    字符串拼接 / 复合赋值 / 多值与命名空间调用）。
> ④ 回归：`tests/aardio_run_check.ps1` 扩到 **67 项**（x64/x86 双绿）——
>    新增 `new_native`(42)、`import`(42)、`import_new`(7) 三个夹具；
>    `tests/aardio_chunk_check.ps1` 同步到 54 项（x64/x86 双绿）。
> **下一步（S-1b 第四阶段再续）**：字符串拼接（`0x21`）、复合赋值
>    （`0x2D`/`0x2E`）、多值调用（`0x2F`–`0x31`、`0x3C`），
>    补齐剩下 7 种；另外官方样本要真正跑起来，还差一层
>    「aardio 标准库 → Nodio 标准库」的映射（根 chunk 第 3 条调全局 `import`）。
> **第十八轮（S-1b 第四阶段再续：字符串拼接与 for 循环落地）**。
> ① **0x21 CONCAT 定案**（`docs/aardio-bytecode.md` §7.5.7）：把**槽 B..C**
>    的值按**升序**拼成字符串（count = C-B+1），先写回槽 B 再拷进槽 A；
>    官方描述符 `DAT_007C80A0[0x21]=0x68`（B/C 都是局部槽下标），
>    第十七轮记下的「实参窗口歧义」就此消除。
> ② **0x2D / 0x2E 定案**：`0x2D` = for 循环推进（槽 A..A+3 = 计数器/上限/
>    步长/内部计数器；上限用 `>` 判越界 → 上限本身计入循环；越界则落空），
>    `0x2E` = 槽 A -= 槽 A+2 后**无条件**跳转，是 for 循环的**序言**。
> ③ 记录 VM 落地 `0x21` / `0x2D` / `0x2E`，
>    `-disasm` 进度从 `可执行 54/61` 变成 **`可执行 57/61`**（剩 4 种：
>    `0x2F` / `0x30` / `0x31` / `0x3C`）。
> ④ 回归：`tests/aardio_run_check.ps1` 扩到 **73 项**（x64/x86 双绿）——
>    新增 `concat`("a12b")、`forloop`(60) 两个夹具。
> **下一步（S-1b 第四阶段收尾）**：多值调用（`0x2F`）、函数值调用（`0x30`）、
>    命名空间调用（`0x31`）、带实参个数的调用（`0x3C`），
>    补齐剩下 4 种；另外官方样本要真正跑起来，还差一层
>    「aardio 标准库 → Nodio 标准库」的映射（根 chunk 第 3 条调全局 `import`）。
> **第十九轮：aardio 标准库「全量可用」的现状盘点 + 记录层收口目标**。
> ① **标准库这条线的真实状态**（`docs/capability-gap.md` §3）：
>    官方公开库共 **969 个模块**，Nodio 用 `tests/module_sweep.ps1` 逐个 import 实测：
>    **x64 `ok=897` / 非 OK 72**，与第七轮可信基线**逐位一致**（无回归）。
>    72 项**零引擎缺陷残留**，全部可归因：物理边界（32 位 DLL，x86 全通）、
>    官方「先导入父命名空间 / 禁止直导」契约（官方同样报错）、
>    下载安装型超时（环境）、官方库自身缺依赖（BouncyCastle 等）、
>    `=aardio` 闭源字节码（**2**）、伪模块 `win.#DEFINE`（清单问题）。
> ② **唯一能靠自己解决的缺口**：`=aardio` 那 **2** 项
>    （`ide.aifix` 与 `autos.tools.handlers` 指向**同一个**官方预编译块
>    `lib\ide\aifix.aardio`）—— 解法就是**记录 VM 收口到 61/61**
>    （`0x2F` / `0x30` / `0x31` / `0x3C` 这四种调用变体）。
> ③ **第十九轮取证**（`docs/aardio-bytecode.md` §7.5.8）：这四种的位移取自
>    **下一条 u32**、基点为本条、偏置 `0x1FFFE`；官方靠「在当前帧槽区搭临时帧
>    再递归进解释器」实现（`FUN_004D33C0`，200 层调用深度上限）；
>    `0x31` 的 helper `FUN_004C00C0` 实为「取表里第一个非空成员」。
>    尚缺的是**实参窗口的确定**（官方未直接写明），需按样本 38 处逐个反推。
> **下一步（S-1b 第四阶段收尾）**：把 `0x2F` / `0x30` / `0x31` / `0x3C` 落地，
>    记录 VM 61/61 → `ide.aifix` + `autos.tools.handlers` 两个官方模块可用；
>    随后标准库这条线就只剩物理边界 / 官方契约 / 环境三类，需写进交付说明。
> **第二十轮：四种动态调用变体落地 —— 记录 VM 收口 61/61**。
> ① `0x2F` / `0x30` / `0x31` / `0x3C` 四种调用变体全部实现
>    （`docs/aardio-bytecode.md` §7.5.9），`-disasm` 的进度从
>    `可执行 54/61` 变成 **`可执行 61/61`**（可执行记录种类**全部落地**）。
> ② 回归 `tests/aardio_run_check.ps1` 全绿（x64/x86），
>    `tests/aardio_chunk_check.ps1` 同为全绿。
> **第二十一轮：官方预编译块真正执行 —— 「aardio 全部标准库可用」收口**。
> ① **调用约定定案（owner 模型，纠正第十六/二十轮）**
>    （`docs/aardio-bytecode.md` §7.5.10）：官方被调值 = 槽 `A`；
>    调用区 = 槽 `A+1..A+B-1`，其中 **`A+1` 是 owner**（隐含首参数，**不算实参**），
>    实参 = 槽 `A+2..A+B-1`；**`C-1` = 返回值个数**（不是实参个数），
>    返回值写回调用方槽 `A..A+C-2`。
>    0x2F 的返回值写回**被调槽 `A+3`**，且**结果为空时跳过那条 `0x22`**
>    （照常跳回会让 `string.gmatch` 的迭代器二次调用已 delete 的游标 → 堆损坏）。
> ② **官方预编译块能跑完**：`engine/import.cpp` 命中 `=aardio` 时用记录 VM
>    **执行该块**并交出模块命名空间 —— `ide.aifix` 的 5153 条记录**跑到底**，
>    `autos.tools.handlers` 同步转 OK。
> ③ **标准库全量复测**：969 个公开模块，**x64 `ok=899` / 非 OK 70**、
>    **x86 `ok=933` / 非 OK 36**；与第十九轮日志**逐模块对比零新增失败**，
>    增量全部来自那 2 个 `=aardio` 模块（x86 另 +1 是上轮 TIMEOUT 项本轮通过）。
> ④ **四类交付表**（物理边界 / 官方契约 / 官方库自身缺陷 / 环境限制）写入
>    `docs/capability-gap.md` —— **Nodio 侧自有缺口归零**，
>    即：**「可以使用 aardio 的所有标准库」这条线在 Nodio 侧已经收口**。
>    验收口径、两套构建各自覆盖什么、回归锁清单见
>    **`docs/aardio-stdlib-delivery.md`**（交付说明）。
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
| IDE (Phase 3, self-hosted) | 15 | **52%** | DSG shell + modular `app/*` + project model (`.nproj`/`.aproj`) green (`tests/ide_smoke.ps1`); chrome 自绘化（文档页签 / 工具条 / 三栏 dock / 设计画布底板）；**图标组件体系**（FontAwesome 字体图标 + GDI+ 手绘控件缩略图）；**DPI 缩放对齐**（`nodio.ui.dpi`，150% 下自绘层与原生控件同字号）；**`nodio.gui` 控件体系**（移植 Zan.Gui 的 Theme→Style→StyleBox→Widget，右栏「组件」页展示全套控件）；**工程树/问题列表/右栏列表改自绘**（原生 listview/treeview 配色会崩进程）；**自绘标题栏**（第九轮：菜单并进标题栏一行，摘掉原生浅色标题栏 + 满幅客户区 + 自绘拖动/窗口按钮）；第八轮修掉三个语义级 bug（`string.find` 是正则 / 空串是真值 / `io.readFile` 返回原始字节 → 中文源码不再乱码）；**修掉自绘控件 z 序吃鼠标事件的通病**（背景板压住所有面板 → 页签/树/标题栏点不动）；`nodio-lsp`/`nodio-dap` skeletons negotiate capabilities over stdio; IDE-side parser wiring, highlighting and the debug channel still open |
| form designer (Phase 4) | 5 | **85%** | 8 手柄缩放 / DSG 往返（类型保持、输出稳定）/ 属性格回写（含几何）/ 自绘工具箱 已通；**设计坐标改逻辑像素**（与官方 DSG 约定一致，150% 下所见即所得）；自定义组件可经 `nodio.designer.registry` 注册并落到画布；画布/工具箱的鼠标交互随 z 序修复恢复；**属性格分组带**（VS 的 Appearance/Behavior/Data/Layout/Misc 顺序 + 折叠箭头）；**画布控件可见**（底板直绘盖掉子控件像素 → 底板刷完补刷组件区域）；**事件代码生成**（第十五轮：双击控件 → 官方形状的事件桩，已有只跳转不重复）；**设计 ↔ 源码同步**（第十五轮：开设计器载入源码的设计块、关设计器写回，switchActiveForm 语义）；**官方 ide.customCtrl 替身映射**（第十六轮：textbox 等官方自定义类用 edit 代画，DSG 仍写真实类名）；**窗体设置 / 对齐网格 / 边距线**（第十六轮：点空白选窗体改 标题/尺寸写回 DSG 头，拖动缩放吸附网格，内容区参考线）；缺 缩放 / 控件间吸附线 / fromDSG 保留控件名 |

**Overall ≈ 78.9 / 100**（第十六轮设计器补齐之后；此前 78.4）。
本轮增量（designer 75% → 85%）：
**官方 ide.customCtrl 替身映射**（官方 `lib/ide/customCtrl.aardio` 原文照抄
进 `nodio.designer.registry.customCtrl`；画布 `__findSpec` 先精确匹配注册表、
再查映射表 —— DSG **仍写真实类名**只换实际画的控件，官方工程的
textbox/vlistview 画得出、写回官方也认）；
**窗体设置**（点画布空白 / 「设计」菜单「窗体设置」= 选中窗体本身，
属性格改 标题/宽度/高度 → DSG 头 `win.form(...)` 跟着变）；
**对齐网格**（拖动/缩放吸附 GRID_SIZE 整数倍，属性格精确值不吸附，可开关）；
**边距线**（表单内容区参考线，默认 8 逻辑像素，可开关）；
**抓图探针加固**（`ide_shot.ps1` 在 SW_RESTORE 之后等窗口矩形稳定 500ms
再抓 —— 此前会抓到延迟重排的混帧：画布旧矩形 + 编辑器白板露出来）。
上一轮增量（designer 55% → 75%，IDE 表行同步到实测口径）：
**事件代码生成**（设计器双击控件 → 源码里出现官方形状的事件桩；
已有的处理函数只跳转不重复生成；右键「查看事件代码」兜底）；
**设计 ↔ 源码同步**（switchActiveForm：开设计器=从当前文档载入 DSG 块、
关设计器=把当前设计写回文档的 DSG 块，编辑器缓冲不落盘）；
**IDE 冒烟加固**（工程清单维护夹具改写 TEMP —— 只读检出 / CI / 沙箱里
仓库不可写时同样能跑；新增 EVENT-STUB / EVENT-STUB-DUP / EVENT-CODE-VIEW /
SYNC-ITEMS / SYNC-WRITEBACK / SYNC-BLOCKS 六条断言）。
上一轮增量（IDE 45% → 52%，design 保持 55%）：
**下拉菜单真正可用**（浮层板 `transparent=true` 保可见 + `SetCapture` 保可点 +
菜单项命中矩形改回画板本地坐标 —— 此前「点得开但看不见、菜单项点不动」）；
**自绘板整窗失效**（`board.invalidate` 改为整块 InvalidateRect ——
局部失效会让 plus 的绘制裁剪区只剩那一小块，标题栏 logo 与前两个菜单整块消失）；
**官方 richedit 补 `autoResize=false`**（此前窗体改尺寸时编辑器被弹回设计位置，
与自绘的行号栏错位 ⇒ 组件乱飞 + 事件区与显示区不一致）；
**状态栏保持期**（心跳每 300ms 写「就绪」把操作反馈全抹掉）；
**心跳不再每 300ms 全量重发文档给 LSP**（卡顿大头）；
**调试器打通**（`dapClient` 改模块式 API，`poll`→`pollEvents`，
断点改成 JSON 数组，**行号栏点一下打/消断点**）；
IDE 启动时定位到主显示器并最大化。
**设计器画布修复**：`board.show()` 之后强制整块重画（创建时隐藏、后来才显示的控件
首次更新区域为空 ⇒ 绘制裁剪区一空就等于什么都没画 —— 窗口在、可见、却一块黑；
这一条同时救活设计画布、提示条、查找条、补全浮层、下拉菜单）；
画布底板显式提到 z 序最前；**不再 hide/show 官方 richedit**（它的 show/hide
会重建控件并清空正文，「开关一次设计器代码就没了」）。
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
5. [x] **官方内建 API 缺口收口（函数 API 层面 17 → 1）**（2026-10-21 第二十三轮）—
   `string.split`（官方 4 种形式）/`splitLiteral`、`io.lines`、`io.libpath`、
   `math.fmod`/`lerp`、`table.setAttr`、`com.NewObject`/`NewControl`/`SafeArray`/
   `IsIUnknown`/`IsMember`/`GetPointer`/`DoObjectVerb`，以及 COM 事件接收器
   `com.Connect`/`AddConnection`/`ReleaseConnection`（dispinterface 型连接点，
   实测 `MSXML2.DOMDocument` / `Shell.Explorer` 事件链路打通）；另修 `%` 运算符的
   floor 语义与 `com.NewObject` 失败路径的错误信息丢失。实测官方内建成员
   **149 / 150 覆盖（99.3%）**，唯一缺口 `com.ImplInterfaceFromTypelib`
   （需按 ITypeInfo 动态构造原始接口 vtable，官方库零调用点）。
   回归：`tests/builtin_api_check.ps1`（5 段断言）+ `tests/builtin_api_coverage.ps1`
   （覆盖率核对），x64 / x86 均绿。详见 `docs/aardio-stdlib-delivery.md` §10
   （该缺口已于第二十五轮闭合，见第 7 项）
6. [x] **comTypeInfo 类型信息契约补齐 + 杀软误报代码层根治**（2026-10-21 第二十四轮）—
   `GetTypeAttr()` / `GetFuncDesc(i)` / `GetVarDesc(i)` / `GetImplType(i)` /
   `GetImplTypeFlags(i)` 按官方 `!comTypeAttr` / `!comTypeFunc` / `!comTypeParameter`
   形状补齐（索引从 1 起）。官方 `com.typeAttrs.summary` / `com.typeAttrs.load` /
   `com.tlbDoc` / `com.interface.loadTypelibrary` 四个标准库消费者由此**全部打通**
   （此前是死代码）：`com.tlbDoc` 现能输出 11121 字类型库文档，
   `loadTypelibrary` 能从任意 TLB 动态生成接口声明类。
   同轮根治火绒把 32 位 `nodio.exe` 报为 `HEUR:Backdoor/Farfli.e` 的误报：
   二分定位到「可写+可执行内存」与「32 位异常过滤器转储原始寄存器」两特征共现，
   改为「申请时可写 → 写入代码 → 降权 `PAGE_EXECUTE_READ`」+ 精简 x86 转储，
   x64 / x86 复测均 CLEAN。回归：`tests/builtin_api_check.ps1` 新增第 6 段（32 条断言）、
   新增 `tests/av_heuristic_check.ps1`（源码级闸门），11 套回归 x64 / x86 均绿。
   详见 `docs/aardio-stdlib-delivery.md` §11、§12
7. [x] **com.ImplInterface / 原始接口事件连接点闭合 —— 官方内建成员覆盖 150 / 150（100%）**（2026-10-21 第二十五轮）—
   `com.ImplInterface(implTable,progID,interfaceName)`（3 参）与
   `com.ImplInterfaceFromTypelib(implTable,typelibPath,interfaceName,coclassName)` 实现，
   连同依赖它们的「原始（非 IDispatch）接口事件连接点 Advise」一并打通。
   - **vtable 生成器**：按 ITypeInfo 递归收集继承链，槽位 = IUnknown(3) + 继承接口 +
     自身方法；每槽一个 stdcall 跳板，参数按 ELEMDESC 转换，方法按名字转发到 aardio 同名成员。
   - 官方文档示例 `com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")`
     **端到端跑通**：`com.AddConnection` 返回 cookie=1，服务器真实派发事件，
     aardio 处理函数收到 `OpenStateChange(1)` / `StatusChange` / `PlayStateChange(9)` … 共 11 次；
     `com.ImplInterfaceFromTypelib` 与 `com.Connect` 自动路径同样收到事件。
   - 三个与直觉相反的实测结论：
     ① 服务器按**源接口 IID** 查 sink（不是 `IID_IDispatch`）—— 纯 IDispatch 接收器会被拒
     （`0x80040202`）；② 事件由 `IDispatch::Invoke` 派发，命中 vtable 第 7 槽，
     所以 IDispatch 四槽**必须**齐备；③ dispinterface 的 `cImplTypes` 在不同类型库副本里
     可能不一致，对 `TKIND_DISPATCH` 需无条件补齐 IDispatch 四槽。
   - `IDispatch::Invoke` 的 `pVarResult` 是 `VARIANT*` 出参，参数码必须硬编码
     （按类型信息推导会读未初始化内存 → `AddRef` 崩溃）。
   回归：`tests/builtin_api_check.ps1` 新增**第 7 段**（`com.ImplInterface / 原始接口事件`），
   11 套回归 x64 / x86 均绿。详见 `docs/aardio-stdlib-delivery.md` §12、
   `docs/capability-gap.md` §1.23
8. [x] **官方标准库可用性 —— 扩展名不再依赖外部配置文件**（2026-10-21 第二十六轮）—
   `nodio.json` 找不到时，默认扩展名列表只剩 `.nodio`，官方标准库的唯一源码
   扩展名 `.aardio` 消失 → **整个官方标准库静默失效**（`import win.ui` 不报错，
   但 `win.ui` 是 `null`；`win.form` 从官方 `class` 退化成引擎内置的
   `native function`）。触发场景都是真实部署形态：发布产物（自包含 exe，
   官方库编译进 payload，外部无 `nodio.json`）、自定义部署目录、CI 临时工作目录。
   - **修法**：官方契约固化进引擎内置默认 —— 默认 `extensions` = `.nodio` + `.aardio`
     （`.nodio` 在前，同名时 Nodio 自己的源码优先），默认 `magicHeaders` 追加 `=aardio`；
     配置文件 / `NODIO_EXTENSIONS` 降级为**追加**而非**替换**。
   - **回归**：新建 `tests/lib_availability_check.ps1`（4 段）——
     无 `nodio.json` 时官方库可用 / 有配置时一致 / 配置只写 `.nodio` 时仍可用 /
     配置缺失时 `=aardio` 魔数仍被识别。x64 / x86 双架构全绿。
   详见 `docs/capability-gap.md` §1.24
9. [x] **发布产物可用官方标准库 —— 字节码序列化与结构体编组余量**（2026-10-21 第二十六轮）—
   发布产物（自包含 exe）此前**完全不可用**：用基线 exe 发布同一工程 → 两种宿主都报
   「运行错误: 载荷中没有可执行的启动脚本」；本轮修复后产物可正常运行。
   - **缺陷一（字节码序列化丢值）**：`CollectStrings()` 跳过空字符串，而解析器**故意**
     产出空名（缺位命名空间段、`...` 可变参数标记）→ `WriteNode` 写出越界哨兵
     `0xFFFFFFFF` → 发布时报「字节码读取错误: 越界的符号名索引」；且 `Node::Bool` 的值
     存在 `num` 字段，却**只给 `Node::Num` 写 double** → 布尔字面量字节码往返后变 `false`
     （官方 `time/performance.aardio:4` 的 `..raw.long(0,true)` → `raw.long(0,false)`）
     → 产物启动即在 ntdll 访问冲突。
     修法：空串入池 + 新增 `EmptyStringIndex()`（找不到名字时写有效索引）、
     `ReadNode` 宽容 `0xFFFFFFFF`、`Node::Bool` 同样写/读 double，
     并给 `SerializeNode` 加**往返字节级自检**
     （序列化→反序列化→再序列化→两次字节必须完全一致），把格式丢信息提前到编译期。
   - **缺陷二（结构体编组缓冲区没有余量）**：发布产物**基名恰好 `app`**（`APP`/`App` 同）
     时在 `Gdiplus!GdiplusStartup` 访问冲突。cdb 实测：引擎为 `struct&` 参数编组出的
     缓冲区是 **16 字节**（4 × `int`）且正好压在 64KB 页尾，崩溃指令读的是
     `[rdx+0x10]`（第 17 字节，已在缓冲区之外）。根因是**官方库按 32 位 aardio 写的**：
     官方 `gdip/core.aardio:13` 声明 `GdiplusStartupInput` = 4 × `int` = 16 字节，
     而 Win32 该结构的字段是 `ULONG_PTR`，x64 下真实结构是 32 字节；官方 aardio 本体是
     **x86**（PE machine = 0x014c），`ULONG_PTR` 同为 4 字节，官方因此从不越界，
     Nodio 的 x64 必然越界（x86 无此问题）。
     修法：`StructSerialize()` 增加 `cbSlack`（零填充余量），
     新增 `StructOverreadSlack()`（按「平台指针宽度 − 声明宽度」求和，下限 64 字节），
     静态 FFI 与动态 FFI 都传这份余量 → 越界读落在分配内部，多出来的字段读到 0，
     与官方在 x86 上的行为一致。
   - **回归**：新建 `tests/publish_official_lib_check.ps1` —— 发布含 `import win.ui` /
     `import builtin.struct` / `import string.format` 的工程 → 复制到**隔离目录**
     （确定无 `nodio.json`、不设 `NODIO_LIBDIR`）→ 断言 `win.ui=table` / `RECT=class` /
     `HIWORD=4660`；产物名**就用 `app.exe`**（曾崩的名字）。修前 FAIL（3 项 `<missing>`），
     修后 PASS，x64 / x86 双架构全绿。
   详见 `docs/capability-gap.md` §1.25 / §1.26、`docs/aardio-stdlib-delivery.md` §14
10. [x] **语句级性能 —— 每条语句一次 `GetEnvironmentVariableW`（3~5 倍提速）**（2026-10-21 第二十七轮）—
    x64 实测：200 万次迭代的空循环 89 ns/次，而 `s=s;` 却要 1838 ns/次 → 成本不在算术而在「执行一条语句」本身。
    cdb 采样栈命中 `ntdll!RtlQueryEnvironmentVariable` → `KERNELBASE!GetEnvironmentVariableW` → `nodio+0x596b9`（`EnvFlag()`）—— 诊断开关 `EnvFlag(L"NODIO_GC_TRACE")` 被放在**每条语句**都会经过的 GC 安全点上（`runtime.cpp` 的 `MaybeCollectGC()` 两处 + `gc.cpp` 8 处，其中 `NODIO_GC_NOFINAL` 每对象一次），每次 `GetEnvironmentVariableW` 扫进程环境块 ≈ 1.75 µs。
    - **修法**：诊断开关改为**进程启动时读一次**（`static const bool`）—— 新增 `GcTraceEnabled()` / `GcNoFinalEnabled()` / `ComTraceEnabled()`；顺带删掉 `com.cpp` 里遗留的写死 `%TEMP%\avtest\comraw.log` 临时诊断块。
    - **改前/改后实测（x64, ns/次）**：空循环 89→80；`s=s` 1838→**359**；`s++` 2209→**453**；`s=s+1` 2183→**449**；`s=(s+i*3)` 2356→600；`%` 2503→732；函数调用 5640→1817；全局变量写 2049→471；局部变量写 1973→496；表字段写 2180→675 —— **语句成本下降 3~5 倍**。
    - **残留**：每条语句仍约 450 ns（`Env::Find` 作用域链 + 命名空间表 2~4 次哈希查找；建议变量名 intern 成整数 slot）；函数调用 ~1.8 µs/次；`import win.ui` 净 ~310 ms；冷启动 ~60 ms；GC 阈值默认 2000 次分配粒度偏粗。
    - **回归**：x64 全 15 套绿，x64 全量模块 sweep 与基线逐项一致（ok=899 / fail=70）。
      ⚠ `tests/win_clip_check.ps1` 在 x86 上偶发红 —— 剪贴板是**全局系统资源**，被同机其它进程占用时官方语义是「静默失败」，而本测试只在开始时判定一次 `CLIP-OK`（该回归锁本身需要改成每次断言前重新探测可用性）。
    详见 `docs/capability-gap.md` §1.27


11. [x] **x64 浮点 ABI 修复 + 按官方路径补 Nodio 自己的 x64 库**（2026-10-22 第二十八轮）—
    x64 上「官方库只带 32 位 PE」一直只是表层原因；顺着补版往下挖出一个
    **引擎级**缺陷：x64 的调用分派写成 `bStub = stub && !sig.bCdecl`，
    于是**带 cdecl 的调用绕开了 ABI 正确的调用桩**，退回把每个实参当整数类
    传的 `InvokeCdl` —— double/float 实参根本进不了 XMM0..3。
    - **实测**：`msvcrt pow(2.0,10.0)` 返回 **1**（期望 1024）；
      `av.audio.dsp` 的 `a_fill` 写不进缓冲区、`a_peak` 恒为 0；
      探针 DLL 显示被调方收到 `a=2.5, b=0`（第二个实参占了第一个位置）。
    - **修法**：MS x64 ABI 只有一套调用约定（编译器忽略 __cdecl/__stdcall），
      x64 一律走按签名逐字节发射的调用桩（位置相关的 XMM 分配本来就是对的）；
      x86 行为不变。
    - **补版**：`lib/av/audio/dsp.aardio` —— 按**官方同名路径**在 Nodio 侧
      提供 x64 版本：`.res/dsp64.dll` 由官方同目录 `.res/dsp.c` 用 MSVC 重新
      编译（x64 / 静态 CRT，38 个导出与官方逐个一致），模块改用
      `io.libpath()` 定位自身目录加载它；x86 构建不命中 overlay，行为与官方一致。
    - **回归**：新建 `tests/ffi_double_check.ps1`（9 项断言，x64/x86 双绿）；
      x64 / x86 各 17 套既有回归全绿；x64 全量 sweep
      **ok=899 → 904 / fail=70 → 65**，修复 `av.audio` 家族 5 项，
      **新增失败 0 项**。
    详见 `docs/capability-gap.md` §1.28

12. [x] **按官方同名路径补 Nodio 自己的 x64 库（6 个模块 / 10 项失败）**（2026-10-22 第二十八轮续）—
    x64 上「官方库只带 32 位 PE」有两层：引擎层的浮点 ABI（第十一项已修）与
    库层的 PE 本身装不下。库层走 Nodio 的 overlay 机制 ——
    `engine/import.cpp` 的 `GetOverlayLibDir()` 把 `D:\Nodio\lib` 放在官方库
    **之前**查找且**相对路径完全一致**，把 Nodio 的模块放成官方同名同路径，
    官方代码 `import xxx` 自动命中 Nodio 版本，**调用方零改动**。
    - **官方随库发布了 C 源码的 6 个模块**：用 MSVC 把同一份 C 源码重编成
      x64（`/MT` 静态 CRT），overlay 模块是官方模块的**逐字副本**，只把 DLL
      来源换成 `io.libpath()` 定位的同目录 x64 DLL：
      `av.audio.dsp`（`.res/dsp.c`）、`win.region.png` / `win.region.bitmap`
      （`.res/build.aardio` 内嵌 C）、`gdip.feather`（`build-feather.aardio`）、
      `raw.argsPointer`（`build.aardio`）、`sys.audioVolume`（`volumeCtrl2.cpp`）。
    - **验证**：6 个模块 x64 全部导入成功；dsp 的 x64 overlay 版与 x86 官方版
      数值**逐项一致**；`PngDataScan0ToRegion` 返回有效 HRGN。
    - **回归**：新建 `tests/x64_overlay_check.ps1`（9 项断言，x64/x86 双绿）；
      x64 / x86 各 17 套既有回归全绿；x64 全量 sweep
      **ok=899 → 909 / fail=70 → 60**（-10，新增失败 0 项）。
    - **剩余 60 项 x64 失败里没有一项是 Nodio 引擎缺陷**：24 项官方只带 32 位
      DLL 且未发源码、13 项 32 位 Python 宿主、6 项官方「先导入父命名空间」
      守卫、6 项自带安装器超时、3 项官方库缺失、1 项官方断言、2 项官方同款
      导入顺序、1 项清单伪条目。
    详见 `docs/capability-gap.md` §1.29

13. [x] **py2 / py3 子模块：按官方同名路径补 Nodio 自己的 x64 版本（13 项失败）**（2026-10-22 第二十九轮续）—
    官方 py2 / py3 的**子模块约定「必须先 import 父命名空间」**：子模块在类体作用域里
    直接 `::Python3.api(...)`，父模块没载入时 `::Python3` 为空 → 导入即报错
    （x86 同样失败，与官方同款，不是 x64 独有偏差）。
    - **宿主**：官方只带 32 位 `python27.dll` / `python38.dll`（PE `machine == 0x014C`）；
      仓库内已备 64 位 `lib/py3/.res/DLLs/python38.dll`（`0x8664`）→ **py3 在 x64 上可用，
      py2 不可用**。
    - **补版**：13 个 overlay 文件全是官方模块的**逐字副本**，只做最小补丁 ——
      `lib/py3/_.aardio` 让 `::Python3` 与 `::Py3Runtime` 指向同一个 DLL 句柄；
      `lib/py3/{dict,export,list,module,object,tuple}.aardio` 自行载入运行时
      （`::Py3Runtime := ::Py3Runtime || loadDll(...)`）并把 API 绑定改绑 `::Py3Runtime.*`
      → 子模块**可独立导入**；`lib/py2/{...}.aardio` 在 `::Python2` 为空时注册
      **明确报错的占位类**并提前返回 → 导入不崩溃、使用时给出与 `lib/py2/_.aardio`
      一致的提示。
    - **`py3.export` 例外**：导入期就执行 Python 代码（`..py3.exec`），官方同样要求先
      `import py3`；overlay 把内部错误换成明确提示，并给它的命名空间 `_set` 元方法加
      `Py_IsInitialized()` 守卫 —— 否则会调用未初始化的 CPython **直接让进程崩溃**
      （实测 `0xC0000409`）。
    - **回归**：新建 `tests/py_overlay_check.ps1`（21 项断言，x64/x86 双绿）；
      x64 / x86 各 20 套回归全绿；x64 全量 sweep
      **ok=908 → 919 / fail=60 → 49**（-11，新增失败 0 项）。
    - **剩余 49 项 x64 失败里仍没有一项是 Nodio 引擎缺陷**：23 项官方只带 32 位 DLL
      且未发源码、12 项官方契约守卫、2 项官方同款导入顺序、6 项自带安装器超时、
      3 项官方库缺失、1 项官方断言、1 项外部安装缺件、1 项清单伪条目。
    详见 `docs/capability-gap.md` §1.30

14. [x] **string.keywords：官方没发源码的模块，Nodio 侧自写 x64 版本（1 项失败）**（2026-10-22 第二十九轮续）—
    官方 `lib/string/keywords/_.aardio` 把 32 位 `keywords.dll` 用 `$"..."` 内嵌进源码再内存加载
    → x64 装不下（实测「无法加载 DLL: (内嵌的内存映像)」）；官方 `.res` 里**只有二进制没有源码**
    → 无法像 `av.audio.dsp` 那样重编成 x64，只能在 Nodio 侧自己实现。
    - **官方语义（x86 上跑官方 32 位 DLL 实测）**：`findOne` 返回**在数据里出现位置最靠前**的
      关键字的 1 基索引（**不是**按关键字表顺序）；`findAll` 在空数据 / 空关键字表时都返回 true；
      三个 `*MatchLines` **只按 `\n` 切行**、**匹配用未剥离的原始行**、
      **报告长度去掉行末所有的 `\r`**、数据以 `\n` 结尾不产生末尾空行，
      关键字表为空时一行都不回调。
    - **补版**：`lib/string/keywords/_.aardio`（官方**逐字副本**，只换 DLL 来源）+
      `lib/string/keywords/.res/keywords64.c`（**Nodio 自写实现，源码入库，可复现**）+
      `.res/keywords64.dll`（MSVC x64 /MT 静态 CRT，导出 5 个符号与官方同名同签名同调用约定）。
      官方代码 `import string.keywords` 自动命中 overlay，**调用方零改动**。
    - **回归**：新建 `tests/keywords_overlay_check.ps1`（44 项断言，x64/x86 双绿）；
      4 组黄金探针在 x86（官方 DLL）与 x64（自写 overlay）上**输出逐字节相同**；
      x64 / x86 各 21 套回归全绿；x64 全量 sweep
      **ok=919 → 921 / fail=49 → 48**（-1，新增失败 0 项）。
    - **剩余 48 项 x64 失败里仍没有一项是 Nodio 引擎缺陷**：22 项官方只带 32 位 DLL
      且未发源码、12 项官方契约守卫、2 项官方同款导入顺序、6 项自带安装器超时、
      3 项官方库缺失、1 项官方断言、1 项外部安装缺件、1 项清单伪条目。
    详见 `docs/capability-gap.md` §1.31

15. [x] **string.markdown：官方没发源码的模块，Nodio 侧自写 x64 版本（8 项失败）**（2026-10-22 第三十轮续）—
    官方 `lib/string/markdown/_.aardio` 把 32 位 `sundown.dll` 用 `$"..."` 内嵌进源码再内存加载
    → x64 装不下（实测「无法加载 DLL: (内嵌的内存映像)」）；官方 `.res/README.TXT` 写明
    该组件基于 `vmg/sundown` 改造、**未随库发布源码**且禁止脱离 aardio 分发
    → 只能像 `string.keywords` 那样在 Nodio 侧自己实现。
    - **补版**：`lib/string/markdown/_.aardio`（官方**逐字副本**，只换 DLL 来源）+
      `.res/markdown64.c`（**Nodio 自写渲染器，源码入库，可复现**）+
      `.res/markdown64.dll`（MSVC x64 /MT 静态 CRT，导出 17 个符号与官方同名同签名同调用约定）。
      结构体布局与官方类**逐字节一致**（`html_renderopt` 24 字节 / `sd_callbacks` 26 个指针，
      含官方自己的 `stuct` 拼写错误字段名）。
    - **回归**：新建 `tests/markdown_overlay_check.ps1`（x64 / x86 双绿）——
      61 例渲染黄金用例（期望值取自**官方 32 位 sundown 的实测**）+ 16 项 API 面断言 +
      overlay 文件/PE 架构/17 个导出符号/模块解析路径；
      x64 / x86 各 **22 套回归全绿**；x64 全量 sweep
      **ok=921 → 929 / fail=48 → 40**（-8，新增失败 0 项）。
    - **顺带修掉一个实现级 bug**：hard-wrap 分支漏 `i++` → 段落含真实换行时无限追加 `<br>`，
      直到 `realloc` 失败把进程打挂（stderr「markdown64: out of memory」），x86 官方同输入正常。
    - **剩余 40 项 x64 失败里仍没有一项是 Nodio 引擎缺陷**：14 项官方只带 32 位 DLL
      且未发源码、12 项官方契约守卫、2 项官方同款导入顺序、6 项自带安装器超时、
      3 项官方库缺失、1 项官方断言、1 项外部安装缺件、1 项清单伪条目。
    详见 `docs/capability-gap.md` §1.33

16. [x] **引擎级缺陷：官方 `stuct` 拼写导致结构体布局错位（与架构无关）**（2026-10-22 第三十轮续）—
    官方 `lib/string/markdown/_.aardio` 的 `html_renderopt` 类里写的是
    `stuct toc_data = { ... };`（`struct` 少一个 r，官方自己的拼写错误）。
    aardio 容忍它；Nodio 的 `IsStructType()` 不认 → 这条声明整个从类的布局里消失
    → `flags` 落到偏移 0、`link_attributes` 落到偏移 4 → sundown 按「24 字节 / flags 在偏移 12」
    读就是**越界读** → 渲染结果在 `<hr>` / `<hr/>` 之间跳变，含 `://` 的链接**直接崩进程**。
    - **实测（pre-fix 引擎用 `git archive HEAD` 导出到独立目录单独构建，与工作区隔离）**：
      x64 `stuct` 类序列化长度 **16 → 24**、flags 偏移 **0 → 12**；
      x86 官方 `html_renderopt` 长度 **8 → 20**；
      x86 官方库渲染 `[a](http://x.com)`：**0xC0000005 崩溃 → 正常输出**。
    - **修法（引擎两处，缺一不可）**：`engine/common.h` 的 `IsStructType()` 加 `L"stuct"` 别名
      （让解析器认它是结构体成员声明）+ `engine/ffi.cpp` 的 `ParseStructDef()` 把
      `stuct` 归一化成 `struct`（让布局按 12 字节算）。
    - **回归**：新建 `tests/struct_alias_check.ps1`（x64 / x86 双绿）——
      两种拼写序列化字节完全相同 + 布局长度 16+指针宽 + flags 落在偏移 12 +
      官方 `html_renderopt` 布局一致；在 pre-fix 引擎上跑它 **3 项失败**（真回归，不是假绿）。
    - 教训：官方源码里落在「类型名」位置的任何拼写错误，Nodio 都会当成未知标量 →
      结构体布局错位。补 x64 版本时要在**引擎侧容错**，不能只在自己的 overlay 里绕开 ——
      否则 x86（唯一能跑官方库的架构）会一直错下去。
    详见 `docs/capability-gap.md` §1.34

17. [x] **av.video.kernel：官方随库发布 C 源码的模块，重编 x64 即可（4 项失败）**（2026-10-22 第三十轮续）—
    官方 `lib/av/video/kernel.aardio` 把 32 位 `kernel.dll` 用 `$"..."` 内嵌进源码再内存加载
    → x64 装不下（实测「无法加载 DLL: (内嵌的内存映像)」）。
    但**官方随库发布了 C 源码**（`D:\aardio\lib\av\video\.res\kernel.c`，注释写明用 tcc 编成 32 位 DLL）
    → 不用自写，直接用官方源码重编 x64 即可（全库 `.res` 下的 C 源码只有两个，另一个 dsp.c 已做）。
    - **补版**：`lib/av/video/kernel.aardio`（官方**逐字副本**，只换 DLL 来源）+
      `.res/kernel64.dll`（官方 `.res/kernel.c` 用 MSVC 编出的 x64 DLL，`/MT` 静态 CRT，零警告；
      13 个导出与官方 DLL **同名同顺序**）。
      依赖链 `av.video.kernel ← av.film ← autos.skills.codingFilm`、`<- av.video.flicker`
      → 补一个模块修好 **4 项**。
    - **回归**：新建 `tests/av_video_kernel_check.ps1`（x64 / x86 双绿）——
      13 个导出全覆盖的像素级黄金值（v_bg 线性/径向、v_grain、v_bounds、v_clear、v_over、
      v_blend、v_blit、v_glitch2、v_resample、v_lumaStats、v_boxMean、v_flickerScore、v_motionCheck）
      + overlay 文件/PE 架构/13 个导出符号/模块解析路径；
      两架构的**全量十六进制输出逐字节一致**（37263 字符全等）；
      x64 / x86 各 **24 套回归全绿**；x64 全量 sweep **ok=929 → 933 / fail=40 → 36**（-4，无新增失败）。
    - **剩余 36 项 x64 失败里仍没有一项是 Nodio 引擎缺陷**：13 项官方只带 32 位 DLL 且未发源码
      （`web.view` 族 4 / `sevenZip.lzma` 族 4 / `raw.fasm` 1 / `web.layout` 族 4）、
      12 项官方契约守卫、2 项官方同款导入顺序、6 项自带安装器超时、3 项官方库缺失、
      1 项官方断言、1 项外部安装缺件、1 项清单伪条目。
    详见 `docs/capability-gap.md` §1.35

18. [x] **收官盘点：x86 对照 sweep —— x64 真正还差 11 项，不是 36 项**（2026-10-22 第三十轮续）—
    之前的失败分类是**按报错文本人工归因**的；更硬的口径是「同一个模块，x86（走官方 32 位标准库）
    也失败吗？」→ 两边都失败且报错一致 = **官方同款行为**（不是缺口，不用做事）；
    只有 x64 失败 = **真缺口**。
    - **做法**：两个架构各跑一次全量 sweep（969 个模块，同一份清单），逐模块比对状态 + 报错文本。
    - **结果**：x64 专属失败 **11** 项；两边都失败报错一致 **24** 项（12 官方契约守卫 + 2 官方同款导入顺序
      + 6 自带安装器超时 + 3 官方库缺失 + 1 官方断言 + 1 清单伪条目）；报错不同 1 项（`py3.export`，
      x64 的提示更友好）；x64 通过 x86 失败 11 项（`py2.*`/`py3.*` 子模块，x64 overlay 让它们可独立导入）。
      **两次 sweep 总数一样（ok=933 / fail=36），含义完全不同。**
    - **11 项真缺口的归属**：`sevenZip.lzma` 族 4（自写 LZMA 编解码，官方 DLL 无源码）、
      `web.view` 族 4（第三方 WebView2 封装，需自写 COM 绑定；本机有 WebView2 运行时 155.0.4283.62）、
      `raw.fasm` 1（自写 x86 汇编器，工作量最大）、`web.layout` 1（**第三方 32 位商业库，不现实**）、
      `com.autoComplete` 1（官方安装器只发 32 位版）。
    - **结论**：24 项**不需要做任何事**（官方自己的契约，Nodio 复现得逐字一致）；
      9 项可解、1 项不现实。
    详见 `docs/capability-gap.md` §1.36

> **第三十一轮（overlay 收口复测 + still-open 清账）**。
> ① **全量 969 模块 sweep 复测**：x64 **ok=943 / fail=26**（§1.36 基线 933/36，
>    增量即 §1.36.3 定性的 9 项可解缺口的 overlay 落地：lzma 4 + web.view 4 +
>    fasm 1 + autoComplete 1）。26 项失败**逐项定性为零 Nodio 自有缺陷**：
>    官方库缺依赖 3、安装器超时 6、官方契约守卫 9、官方同款导入顺序 2
>    （`win.ui.ctrl.splitter`/`thread`——x86 同错，官方冷导入同样失败，
>    源码级核实：splitter 只 import `win.ui.ctrl.metaProperty` 不 import `win`）、
>    web.layout 族 5（第三方 32 位商业库及级联）、清单伪条目 1。
>    **x64 真缺口只剩 web.layout 1 项（不现实）——引擎/标准库线的自有缺口归零。**
> ② **still-open 旧三项全部闭账**：
>    · py2(x86) / golang 守卫——py2 在 x86 76/76 全绿（`tests/test_subsystems.py`
>      修正幻影模块 `sys.hd`/`crypt.sha256` 后），golang 不在失败清单；
>    · console.log 编码原语层——**陈旧错账**：console 解析到官方源码库全量可用，
>      官方本无 `join`/`getMessage` 成员；`string.getUtf` 按官方文档返回 UTF 标记、
>      `isUtf16`/`toUtf16` 语义核对通过；
>    · io.file 文本模式——**本轮落地**：io.file/io.open 默认文本模式（'b' 退出），
>      读 CRLF→`'\n'`、写裸 `'\n'`→CRLF、`'\0'`/`'\x1A'` 终止符（不消耗，后续读
>      停在原地）；CRT fd 恒为二进制（活管道安全），翻译在字节层逐调用完成。
>      **边界（实测定出）**：文本模式只作用于磁盘文件与载荷资源；OS 句柄流
>      （popen 管道）保持字节精确——首版把管道也翻译，LSP/DAP 的
>      JSON-RPC Content-Length 帧错位（`ide_nav_smoke` 符号树变空复现），已改。
>      回归 `tests/iofile_text_check.ps1` 13 项字节级断言 x64/x86 双绿；
>      VM 90/90（双架构）、stdlib、publish、IDE 冒烟、builtin_api 全绿。
>    详见 `docs/capability-gap.md` §1.37

Still open (the remaining path to 100%):

*(2026-10-24 清账：旧三项已全部闭账，见上方第三十一轮。引擎/标准库线剩余
fail 全部为官方契约 / 环境 / 物理边界，自有缺口归零 —— 现状以
`docs/capability-gap.md` §1.37 的 26 项定性表为准。)*

> **第三十二轮（官方 ide.command 外部命令面 + 两个引擎级缺陷）**。
> ① **`ide.command.join()` 首次真正跑通**——账本挂了十几轮的「需要宿主
>    join 主管道」实测是两个引擎缺陷叠加：
>    · `::Kernel32.MapViewOfFile` 等裸调 API 返回被 `(int)` 截成 32 位
>      （`CreateDirectApiWrapper` 默认 ARInt）→ fsys.mmap 拿假指针
>      0xC0000005（cdb 定案：`movsxd rdi,[rdi]` 低 32 位符号扩展）——
>      与第五轮 LRESULT 截断同族，这次在裸调默认路径。内存映射/模块
>      加载族升为全宽返回（HWND/HANDLE 按 Win32 互操作规则本就 32 位
>      有效，不动）；
>    · `table.tostring(tab,true)` 把 `[-1]`/数字键写成裸 `{-1=...}`，
>      自己的解析器读成数组元素 → mmap 注册表往返静默丢失（官方 mmap
>      协议的保留返回槽与 hwnd 键双向丢）→ 非标识符键一律括号化。
> ② **官方 37 命令面挂进自托管 IDE**（`app/ideCommand.nodio`）：全部注册
>    （外部可探测存在性），24 条接真实现（文档/编辑器/工程/配置/设计器/
>    字体/进程），pluginView 暂为注册桩。外部工具/插件与官方同法驱动：
>    `ide.command.join()` + `send(hwnd,"命令名",...)`；IDE 内脚本走
>    `app.ideCommand.dispatch`。
> ③ **回归**：`tests/ide_command_check.ps1` 真实双进程锁（x64/x86 双绿）
>    + VM 90/90 与 89/0/1、IDE 冒烟/右键/导航、编辑器/输入、iofile_text、
>    win_clip、builtin_api、stdlib、publish 全绿，新增失败 0。
>    详见 `docs/capability-gap.md` §1.38

> **第三十四轮（插件视图托管 + 变参语义对齐）**。
> ① **pluginViewCreate/Show 落地**——官方 37 命令面收口：插件面板按官方契约
>    （pane 脚本 `win.form(...;mode="child";parent=...)`，IDE loadcode 后
>    `fn(container)` 变参注入容器）宿主进 dock 插件页签
>    （`dock.addPluginTab` 可见性/布局联动）；样品插件
>    `ide/nodio/plugins/tool.nodio` 随 IDE 自动注册；
>    顺带修复 `ideCommand.dispatch` 读错登记表（`_commands` 而非 `_meta`）。
> ② **变参语义定案**——一轮误判与回退：parameter.md「不放在最后面只能取
>    第一个值」说的是多返回值实参而非 `...`；官方库惯用法
>    `#...`/`...[i]`/`...!==null`（vmtests/mytable.aardio）证明值位置
>    `...` 就是打包表。定案：值位置=打包表、实参调用点=整体展开、
>    `{...}` 数组槽=官方转换数组、具名槽 `k=...`=首变参（pane 契约依赖）。
>    连带修复 6 条内建快速路径绕过 `...` 展开的问题（统一走 evalCallArgs）。
> ③ **回归**：VM 90/90 双架构、ide_plugin_check（新增）双架构、
>    ide_command/iofile_text/builtin_api/stdlib/win_clip/publish/
>    ide_nav_smoke 全绿。
>    详见 `docs/capability-gap.md` §1.40

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
- [x] **代码补全**（2026-10-20 第十一轮）— `Ctrl+Space` 弹候选浮层、
      `↑↓` 选择、`Enter/Tab` 插入、`Esc` 取消，候选来自 LSP。
      组件负责前缀/浮层/导航/插入，IDE 负责问 LSP（`onComplete` ↔ `showCompletion`）。
      ⚠ 我们的 LSP 返回的是**全量**候选，按前缀过滤放在客户端；
      ⚠ 吃掉按键还不够 —— `TranslateMessage` 生成的 `WM_CHAR` 也要一起吞，
      否则 Tab 会既缩进又插制表符、Ctrl+Space 会多一个空格。
      详见 `docs/capability-gap.md` §1.16、`docs/ide-components.md` §11.8
- [x] **IDE 里能打字了**（2026-10-20 第十二轮，引擎级致命缺陷）—
      此前**敲任何键整个进程直接崩**（不是报错，是窗口没了）：
      官方 `win` 消息泵对每个按键消息都调 `win.isDialogMessage(root,msg)`，
      其第 2 参按官方契约是 `struct msg` 按值传，而引擎填出来的消息表
      **没有 `_struct` 定义** → FFI 传了 NULL → `IsDialogMessageW` 读地址 0 → 崩。
      修法：`fillMsgTable` 给消息表补上 MSG 的原生结构定义（48 字节，偏移逐字节对齐）。
      回归：`tests/run_input_tests.ps1` → INPUT-TESTS-OK。
      详见 `docs/capability-gap.md` §1.17
- [x] **项目级代码导航**（2026-10-21 第十三轮）— 把「代码掌控」从当前文件扩到整个工程：
      - `F12` / `Ctrl+点击` **转到定义**（跨文件，落到目标文件的精确列）；
      - `Shift+F12` **查找引用**（底部新增「引用」页签，结果跨文件，双击即跳）；
      - `F2` **跨文件重命名**（弹提示条问新名字，改完的文本同时回灌 LSP 索引）；
      - `Ctrl+Shift+T` **项目符号搜索**（结果复用「引用」面板）。
      索引建在 **LSP 服务端**（复用引擎真 Lexer/Parser）——
      aardio 侧判不准 `//` 右邻 `=` 的 raw string、块注释星号配对、反引号原样串，
      重写词法扫描必然误判。
      配套改造：引擎 `Tok` 增加 `pos`（token 绝对偏移）→ 定义点才有精确列号。
      ⚠ LSP 的列是 0 基 UTF-16 码元，aardio 字符串是 UTF-8 字节 —— 落点与改名
      都要换算，编辑器组件提供 `gotoLineCol` / `replaceTextAt`。
      ⚠ 导航请求同步阻塞（最多 1 秒），按键只记意图，真正去问放到定时器里。
      回归：`tests/lsp_project_smoke.py` → LSP-PROJECT-SMOKE-OK；
      `tests/ide_nav_smoke.ps1` → NAV SMOKE OK（真跨文件跳转 + 改名）。
      详见 `docs/capability-gap.md` §1.18、`docs/ide-components.md` §11.9
- [x] **IDE 源码零假诊断**（2026-10-21 第十三轮续）— IDE 打开自己的工程文件时
      报出一条**不存在的语法错误**（`app/shell.nodio` 第 65 行）：
      - 根因在**传输层**不在词法器：LSP 服务端的 JSON 解码器只认 `\n`/`\t`，
        其余转义退化成字面字符 —— `\r` 变成字母 **`r`**，
        于是 CRLF 缓冲区里每个换行都多出一个 `r` 标识符；
      - 修法：`apps/jsonrpc_stdio.h` 增加 `JsonUnescape()`，
        完整实现 `\n \t \r \b \f \" \\ \/ \uXXXX`（含代理对 → UTF-8）；
      - 回归：`tests/dsg_parse_probe.py` 每个用例**都跑 LF 与 CRLF 两个变体**
        → DSG-PARSE-PROBE-OK；`tests/ide_nav_smoke.ps1` 新增断言 `NAV-DIAGS:0`。
      详见 `docs/capability-gap.md` §1.19、`docs/ide-components.md` §15.2
- [x] **导航栏（面包屑）**（2026-10-21 第十三轮续）— VS 编辑器的标志性 UI：
      编辑区正上方一条 `工程 › 目录 › 文件 › 命名空间 › 函数`，
      跟着光标走，点哪一段跳到哪一段：
      - 服务端 `documentSymbol` 补 `endLine`（AST 子树最大行号）——
        判「包含」而不是「在上方」；`var f = function(){}` 识别成 function；
      - 客户端 `symbols()` 修好：原来读的是不存在的 `v.location`（行号恒为 0）
        且丢掉 `children`，现在返回**层级树**；
      - 符号树**只在文档变化时**拉一次，光标移动走纯内存遍历（不然会卡死编辑器）。
      详见 `docs/capability-gap.md` §1.20、`docs/ide-components.md` §11.10
- [x] **模块钩子整片死区**（2026-10-21 第十三轮续）— 工具条 / 菜单 / 快捷键 /
      工程树双击 / 大纲刷新**全部没被执行过**（代码完整但钩子读到 null）：
      - 根因：引擎里模块体的裸赋值只是**模块帧的局部变量**，
        外部 `app.shell.onX = f` 写的是**模块表成员** ——
        局部副本**遮蔽**了成员，模块内部永远读到初值 null；
      - 修法：24 个钩子收进 `var hooks = {...}` 表（表是同一个对象，谁改都看得见），
        main 侧改写 `shell.hooks.onX = ...`；
      - 顺带把遗留的 `onAfterLayout` 接上（设计画布布局后立刻跟随）。
      回归：`tests/ide_nav_smoke.ps1` 新增**钩子链路端到端**断言 → NAV SMOKE OK；
      诊断模式 `DIAG hooks: all 24 ok`。
      详见 `docs/capability-gap.md` §1.20.3
- [x] **补齐基础功能（2026-10-22 第十四轮）**—「还有很多基础功能都不行，缺了很多东西」。
      这一轮把**编辑器能用**和**基础接线**两层补齐：
  - **编辑器输入行为（P0）**：`lib/nodio/ui/editor.aardio`
    - 回车续接缩进（复制当前行缩进；光标前是 `{`/`(`/`[` 时再多一级）
    - 括号/引号自动配对（补另一半、光标停中间；右边紧跟着闭合符时「跳过」）
    - Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z 走组件自己的撤销栈
    - Ctrl+/ 注释**保留缩进**（标记插在缩进后面，再按一次能取消）
    - 查找条预填当前选词；自动触发补全（`.` 一定问，词长 ≥3 才问）
    - ⚠ **根因**：`ctrlDown()/shiftDown()/altDown()` 写的是
      `GetKeyState(vk) & 0x8000` —— `GetKeyState` 返回的是**字节**状态
      （按下 = 0x80），高字 0x8000 只有 `GetAsyncKeyState` 才有 ⇒
      **三个判断恒为 false**，编辑器所有 Ctrl 快捷键（Ctrl+Z/Y、Ctrl+Space、
      Ctrl+F/H/G、Ctrl+/）**一次都没生效过**，按键全被原生 richedit 吃掉。
      改成 `& 0x80` 后才真正通。
    - 回归：`tests/reg_editor_input.aardio`（22 项断言）→ `REG-EDITOR-INPUT-OK`
  - **悬停提示（hover）**：服务端 `textDocument/hover` 与客户端 `lspClient.hover`
    早就写好了，**IDE 一次都没调**，也没有浮层。
    - 组件负责：鼠标停住 350ms → 算出 (line,col) → 画浮层 → 鼠标移开自动收；
      IDE 负责：问 LSP 要内容，拿到后 `showHover()`。
    - 问 LSP 是同步阻塞的，**不能**在窗口过程里做（鼠标移动消息很密集）。
    - markdown（`**名字**` / `*类型*`）在浮层里剥掉。
    - 回归：`tests/reg_hover.aardio` → `REG-HOVER-OK`；截图见下。
  - **调试语义**：F10 之前停住后走的是 `next()`（单步），
    `dapClient.resume()` 从来没被调用过 —— 状态栏写着「F10 继续」，实际是单步。
    - 改成：F10 = 继续（`resume()`），**F11 = 单步跳过**（`next()`）；
    - 停住时光标自动跳到那一行（之前只在调试面板里打一行字）。
  - **设计的保存/载入**：`designer.toDSG()/fromDSG()` 两个纯函数没有入口，
    画完就丢。现在有两条路：
    - 写回**源码的 DSG 块**（官方 aardio 就是 `/*DSG{{*/ … /*}}*/` 存设计的）：
      `nodio.designer.dsg` 新增 `extractBlock()/replaceBlock()`；
    - 独立 `.dsg` 文件（另存为 / 打开）；
    - 入口在标题栏新加的「设计」菜单里。
  - **Ctrl+Tab 切换标签页**：以前只有鼠标点页签能换文档。
  - 顺带修了两个测试脚本的毛病：`ide_smoke.ps1` 读重定向文件有句柄竞态
    （偶发「IDE produced no stdout」）；`ide_shot.ps1` 在窗体**还没布局完**时
    就去抓图（拍到 158×26 的空壳，改成等窗口够大再抓）。
  - 回归：`tests/run_editor_component_tests.ps1` 新增；
    VM 90 / EDITOR / INPUT / HOVER / SMOKE / NAV / STDLIB **全绿**。
- [x] **双栏对比（2026-10-24 企业生产可用轮次）** — 支持在主窗口内开启左右分栏对比视图（`shell.toggleSplitDiff` 与「视图」菜单「双栏对比」入口），主副编辑器独立运行、内容基线自动同步，关闭后优雅还原完整全宽编辑区。
- [x] 属性格分组带（外观/样式/数据/位置/基本，折叠箭头；VS 的
      Appearance/Behavior/Data/Layout/Misc 顺序）
- [x] **设计画布上的控件「看不见」** — 底板（`plus` 自绘板）内容直绘到窗体背景，
      绘制顺序在子控件**之前**，底板一重绘就把组件区域的像素盖掉，
      而组件不会自动重绘 ⇒ 画布上只剩网格。
      修法：`canvas.__refresh()` 底板刷完把每个组件区域再刷一遍；
      摆放时把组件提到底板之上（`win.setPos(...,_HWND_TOP,...)`）；
      设计器打开时藏掉行号栏（设计面是「表单表面」，不是代码区）。
      详见 `docs/capability-gap.md` §1.23。
- [x] **网格线显示开关（官方 `dlg_grid`，2026-10-24 企业生产可用轮次）** — `canvas.showGrid` 支持在运行时动态开关网格线渲染（间距自适应 DPI 与缩放），在「设计」菜单中提供显式控制入口。
- [x] **事件代码生成（2026-10-23 第十五轮）** — 官方 IDE 的行为：
      设计视图里**双击控件** → 切到代码视图，源码里出现（或跳到）
      它的事件处理函数。
      - 画布组件子类化补 `WM_LBUTTONDBLCLK` → `canvas.onDoubleClick(item)`
        （button/edit/static/listbox/combobox 等标准控件类都带 `CS_DBLCLKS`；
        收不到双击的控件走右键菜单新增的「查看事件代码」兜底）；
      - 事件名按官方控件库的默认事件（对照 `D:\aardio\lib\win\ui\ctrl\*.aardio`
        的 `_commandFuncnames`）：button/checkbox/radio/groupbox → `oncommand(id,event)`、
        combobox/listbox → `onSelChange(id,event)`、edit/richedit → `onChange()`、
        listview/treeview/tab → `onnotify(id,code,ptr)`；
        自定义组件可在 registry spec 里自带 `event` 字段；
      - 桩形状按官方 system-prompt 的模板
        （`winform.<name>.<event> = function(id,event){ }`）；
      - **不重复生成**：源码里已有同名处理函数只跳转过去（官方同款行为）；
      - 插入点：有 DSG 块插在块结束标记后面，没有就追加到文件末尾；
        光标直接落到函数体那一行。
      回归：`tests/ide_smoke.ps1` 新增 `EVENT-STUB / EVENT-STUB-DUP /
      EVENT-CODE-VIEW` 断言。
- [x] **设计 ↔ 源码视图切换（switchActiveForm 语义，2026-10-23 第十五轮）** —
      开设计器 = 从**当前文档**载入 DSG 块（源码里有什么设计，画布上就是什么；
      没有块的文档给空画布）；关设计器 = 把**当前设计**写回文档的 DSG 块
      （只写编辑器缓冲不落盘，落盘交给 Ctrl+S）。两个方向都只在
      「内容真的不一样」时才动文档；画布空且文档本来没有块时不写
      —— 不能往普通脚本里塞空设计；同步是**替换**不是追加（块只有一份）。
      回归：`tests/ide_smoke.ps1` 新增 `SYNC-ITEMS / SYNC-WRITEBACK / SYNC-BLOCKS`
      断言（独立页签双向验证）。
- [x] **自定义控件替身映射（官方 `ide.customCtrl`，2026-10-23 第十六轮）** —
      官方 `lib/ide/customCtrl.aardio` 的 cls→替身表**原文照抄**进
      `nodio.designer.registry.customCtrl`（radio/vlistview/close/ipaddress/
      textbox/custom.* 共 10 条）。画布 `__findSpec` 的解析顺序：
      ①注册表精确匹配（Nodio 能直接画的类优先原样画，如 radio）
      ②映射表 → **DSG 仍写真实类名**，只把实际建的控件换成替身（drawAs）
      （属性表借替身类的）③兜底原样画。
      官方工程里的 `textbox`/`vlistview` 到 Nodio 画得出，写回的 DSG 官方也认。
      回归：`tests/ide_smoke.ps1` 新增 `CUSTOMCTRL-MAP / CUSTOMCTRL-DRAW`。
- [x] **窗体设置 + 对齐网格 + 边距线（2026-10-23 第十六轮）** —
      - **窗体设置**：点画布空白处（或「设计」菜单「窗体设置」）= 选中
        **窗体本身**——属性格改 标题/宽度/高度，写回 DSG 头
        `win.form(text=..;right=..;bottom=..)`（画布记 `form` 状态，
        toDSG/fromDSG 双向携带；没有设计块的文档回默认，不带走上一个文档的窗体尺寸）；
      - **对齐网格**：拖动/缩放时把坐标吸附到网格（GRID_SIZE 整数倍；
        属性格里的精确值不吸附）；「设计」菜单可开关；
      - **边距线**：表单内容区参考线（默认 8 逻辑像素，画在底板上），
        「设计」菜单开关。
      回归：`tests/ide_smoke.ps1` 新增 `FORM-SET / SNAP`。
- [x] **设计器缩放与控件间对齐吸附线（官方 `dlg_zoom_defalut` / `dlg_snap`，2026-10-24 企业生产可用轮次）** — 
      - `canvas.zoom` / `setZoom` 支持 25%~400% 自由缩放，全量缩放几何换算（控件位置、手柄外扩尺寸、网格物理步长均平滑缩放）；
      - 磁性对齐吸附（`alignSnap`）：拖动控件时在 5 逻辑像素范围内智能对齐相邻控件的左、右、顶、底与水平/垂直中线，并高亮绘制橙色辅助对齐线，松手即清空。
- [x] **fromDSG 完整保留原生控件名（2026-10-24 企业生产可用轮次）** — `canvas.add` 支持显式传入 `name` 参数并在属性格首项注入「变量名」支持双向查看与修改；`fromDSG` 与 `toDSG` 严密保持原生变量名不变，官方现有项目导入后事件处理函数绑定零断裂。
- [x] **企业级源码保存原子事务机制（`atomicSaveFile`，2026-10-24 企业生产可用轮次）** — 杜绝断电/进程中断导致的文件清空或半写入损坏，采用临时文件预写 + 大小完整性校验 + `MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 原生原子替换。
- [x] **编辑器书签系统（Editor Bookmarks，2026-10-24 企业生产可用轮次）** — 
      - 行号栏自绘青蓝色旗标矩形（VS 经典深青蓝书签 #007ACC），不遮挡断点圆点；
      - 支持 `toggleBookmark`, `nextBookmark`, `prevBookmark`, `clearBookmarks`, `hasBookmark`, `hasAnyBookmark`；
      - 快捷键深度对齐 VS / 官方习惯：Ctrl+F2 切换当前行书签、F2 跳转下一个书签、Shift+F2 跳转上一个书签；在「转到」菜单中完整挂接。
- [x] **代码片段模板自动展开（Code Snippets Expansion，2026-10-24 企业生产可用轮次）** —
      - 支持 `for`, `forin`, `func`, `function`, `try`, `class`, `if`, `ife`, `winform` 等高频语法触发词；
      - 按 Tab 键自动匹配前缀并展开代码片段，自动继承当前行缩进深度并自动将光标定位至 `$0` 目标锚点；无片段时自动回退为原生缩进。
- [x] **函数参数信息提示（Signature Help，2026-10-24 企业生产可用轮次）** —
      - 在代码输入 `(` 或 `,` 时自动根据光标前函数名唤出悬浮参数信息提示框，精确自适应光标相对坐标并在屏幕边缘自动避让翻转；输入 `)` 或 Esc 时即时收起。
- [x] **窗体设计器批量多选与批量对齐（Multi-Selection & Batch Alignment，2026-10-24 企业生产可用轮次）** —
      - 支持按住 Ctrl / Shift 键点击控件进行自由批量多选；多选状态下全选框同步绘制，仅主选控件展示 8 控向调整手柄；
      - 提供「左对齐」、「右对齐」、「顶对齐」、「底对齐」、「水平居中」、「垂直居中」、「等宽」、「等高」完整批处理对齐指令与「设计」菜单快捷调用。
- [x] **窗体设计器专属撤销/重做事务栈（Designer Canvas Undo/Redo，2026-10-24 企业生产可用轮次）** —
      - 画布独立具备 `undoStack` 与 `redoStack`（上限 50 步），在拖拽移动、尺寸调整、批量对齐、删除控件前自动拍摄快照；
      - 支持快捷键 Ctrl+Z / Ctrl+Y 及「设计」菜单调用，精确无损还原窗体与控件全部属性状态。
- [x] **双栏差异对比导航（Diff Navigation & Markers，2026-10-24 企业生产可用轮次）** —
      - 支持双栏对比视图（`splitDiff`），行号栏边缘自绘差异状态指示条（新增绿色 #2ECC71、删除红色 #E74C3C、修改橙色 #F39C12）；
      - 「视图」菜单提供「下一个差异」与「上一个差异」快捷跳转指令，双栏行号联动同步滚屏。
- [x] **调试器监视表达式（DAP Watch Expressions，2026-10-24 企业生产可用轮次）** — 
      - 支持动态添加/清空监视表达式，程序命中断点停止时自动关联变量作用域解析监视结果，同步输出至调试控制台与状态栏。
- [x] **代码智能自动格式化（Format Document，2026-10-24 企业生产可用轮次）** — 
      - 支持快捷键 Shift+Alt+F 及「格式」菜单「格式化代码」；
      - 逐行自动根据花括号/方括号/圆括号层级深度缩进，支持单行/多行注释保护与字符串字面量边界穿透，自动剔除行尾多余空白，并将整次排版操作封装为单一事务推入撤销栈。
- [x] **全屏沉浸开发模式（Toggle Fullscreen，2026-10-24 企业生产可用轮次）** — 
      - 支持快捷键 Shift+F11 / Alt+Enter 及「视图」菜单「全屏模式」；
      - 具备窗口尺寸与工作区状态无损记录，按键即刻沉浸铺满整个物理显示器，二次触发平滑恢复原窗口几何与排版。
- [x] **外部系统集成与工程代码行统计工具（External Tools & Project Stats，2026-10-24 企业生产可用轮次）** — 
      - 「在资源管理器中定位」：一键唤起 Windows Explorer 并在物理磁盘上高亮选中当前活跃文件或工程根目录；
      - 「在终端中打开」：一键在当前文件或工程工作目录唤起外部命令行终端；
      - 「工程代码行统计」：递归统计工程源码文件总数、代码行、注释行、空行及总行数，弹窗结构化呈现工程代码体量；
      - 「复制文件完整路径」：一键将当前文档绝对路径写入系统剪贴板。
- [x] **快速打开文件（Quick Open / Go to File，2026-10-24 企业生产可用轮次）** — 
      - 支持快捷键 Ctrl+P 及「文件」菜单「快速打开文件...」；
      - 模糊匹配工程文件列表，支持子串与文件名快速索引并即刻定位载入页签。
- [x] **标签页右键上下文菜单与批量关闭（Tabstrip Context Menu & Batch Close，2026-10-24 企业生产可用轮次）** — 
      - 文档标签页右键支持「关闭标签页」、「关闭其他标签页」、「关闭全部标签页」、「复制完整路径」、「在资源管理器中定位」；
      - 批量关闭携带脏文档检查机制，杜绝未保存工作区丢失。
- [x] **状态栏交互增强与编辑视图定制（Interactive Statusbar & View Toggles，2026-10-24 企业生产可用轮次）** — 
      - 点击状态栏「行,列」分段直达「转到行」输入框；
      - 「视图」菜单提供「显示行号」（toggleGutter）与「自动换行」（toggleWordWrap）即时开关。
- [x] **设置面：主题 / 编辑器字号 / 外部工具（IDE Settings，2026-10-24 第三十五轮）** —
      - 新增 `ide/nodio/app/settings.nodio`（配置存 `%APPDATA%\Nodio\ide.cfg`，
        **与官方命令面 getConfig/setConfig 共用同一个文件** —— 外部工具/插件
        用 `ide.command.getConfig("theme")` 就能读到 IDE 的设置）；
      - **主题**（深色 / 浅色）：偏好持久化 + 启动时应用（`main.nodio` 在
        `import app.shell` 之前调 `themeApply()`）。各 chrome 组件是构造期读一次
        配色，运行期再切要重建整排 chrome，官方 aardio 同样是换主题重开 IDE ——
        菜单里明说「重启后生效」，不假装能热切；
      - **编辑器字体大小**（6-24 pt）：**即时生效**。新增
        `nodio.ui.editor.setFontSize(pt)` —— 重设 richedit 字体 → 重新量行高
        （`EM_POSFROMCHAR`）→ 行号栏字体跟着行高走 → 浮层画板字号同步 → 重新布局；
      - **外部工具**（最多 9 个，官方同款约定）：名称 / 命令 / 参数
        （`%f` = 当前文档，`%p` = 工程目录）；「设置」菜单增删、右栏「工具」
        面板分组列出、**Ctrl+1..9** 触发（编辑器组件新增 `onHotkey(vk)` 钩子，
        组件本身不认识「外部工具」）；
      - 回归：新增 `tests/ide_settings_check.ps1`（36 项断言，配置路径由
        `NODIO_IDE_CFG` 隔离，**不碰用户的真实 ide.cfg**）+ `tests/ide_settings/sett.nodio`；
        `tests/ide_smoke.ps1` 增 `SET-FONT / SET-FONT-CFG / SET-THEME / SET-TOOLS` 断言
        （冒烟的配置同样用 `NODIO_IDE_CFG` 隔离）。
- [x] **VM 多返回值语义回归修复（内建快速路径，2026-10-24 第三十五轮）** —
      第三十二轮的「`...` 变参语义定案」把 `evalCallArgs` 上移到内建快速路径
      （io.print / print / tostring / tonumber / error / sleep）之前，导致这些
      路径也开始**就地展开多返回值** —— 官方 `io.print(string.match(...))` 只打印
      **首个捕获值**，不是全部。表现为 VM 回归 t18/t19/t20/t21/t30/t69 六项红
      （84 passed / 6 failed）。修法：新增 `evalFastArgs` —— 与 `evalCallArgs`
      只差「普通实参保留 `mrets`」这一条，`...` 展开照旧。**VM 回归回到 90/90**。

> **第三十五轮（函数调用路径提速 + GC 注册表环安全修复）**。
> ① 调用微基准（每形态 100 万次，Δ 对空循环）：普通 2.17→~1.2µs、方法
>    4.27→~1.3µs、变参 7.81→~1.8µs、内建 1.06→~0.2µs（-44%~-81%）。
> ② 引擎三项：Env GC 注册表惰性化（注册移到闭包捕获点；析构按需擦除——
>    途中修复悬垂注册表项→地址复用→清扫器清活帧的 0xC0000005）、
>    调用/块帧槽化+字节预算缓冲池（推入时拒绝，无淘汰重入；uiprobe13
>    池驻留 206MB→54MB）、变参 mrets 别名化（表视图=展开视图恒一致）
>    + 快路径 evalFastArgs（多返回值实参保持首值）+ evalCallArgs reserve。
> ③ 回归：VM 90/90 双架构、gcstress ×3、gcprobe、ide_plugin/ide_command
>    双架构、iofile_text/builtin_api/stdlib/win_clip/publish/ide_nav 全绿。
>    详见 `docs/capability-gap.md` §1.41（含并行会话两次回退的冲突记录）。

> **第三十六轮（IDE 设置面 + 回归跑批正式化 + 宿主拦截假红定性）**。
> ① 「设置」面落地：主题（重启生效，与官方一致）、编辑器字号（6–48pt
>    即时生效，走量行高→行号栏→浮层→重排整条链）、外部工具（最多 9 个，
>    `%f`/`%p` 占位，Ctrl+1..9，与官方约定一致）；配置落
>    `%APPDATA%\Nodio\ide.cfg` 并与官方 `getConfig/setConfig` **共用同一
>    文件**，`NODIO_IDE_CFG` 可覆盖路径（回归隔离）；新增 36 项设置回归锁。
> ② 全量回归跑批脚本正式化为 `tests/run_regression.ps1`：15 项**全部显式
>    传 `-Exe`**（旧脚本漏传 vm/builtin/import 三项，会测到别的二进制，
>    A/B 对比结论直接作废）。
> ③ **宿主陷阱定性**：宿主跑着火绒，它按**可执行文件路径**拦截写入 ——
>    从正规构建目录之外的目录启动 `nodio.exe` 时，写 `%TEMP%`/`%USERPROFILE%`
>    被拒（`ERROR_ACCESS_DENIED`），读却正常，表现为 t30/t59/iofile/
>    ide_smoke/ide_settings 整片假红。定位链：同二进制逐字节比对仅差
>    4 字节时间戳 → 行为却随**目录**翻转 → 插桩确认路径与属性完全相同 →
>    `HipsDaemon` 在跑。**引擎无责，不要为此改引擎**；回归须用
>    `build-x64`/`build-x86` 正规目录。详见 `docs/capability-gap.md` §1.42。
> ④ 回归：**x64 15/15、x86 15/15 全绿**（VM 双架构 90/90，设置 36 项全过）；
>    前几轮 x86 的 0xC0000005 与 x64 冒烟不完成，随 98e5f94（GC 注册表项
>    析构擦除）一并消失。

> **第三十七轮（IDE「AI」聊天面板 + 引擎 GC 悬垂指针修复）**。
> ① 「AI」页签从一块只读 richedit 换成真正的聊天面板（`ide/nodio/app/ai.nodio`）：
>    顶栏自绘板（发送/清空/设置）、对话日志、输入框、配置区（接口/模型/密钥，
>    密钥 `password=true`）；Enter 发送 / Ctrl+Enter 换行；请求走 **worker
>    线程**（WinINet 阻塞调用放主线程会把 IDE 卡死），`winform.setInterval`
>    轮询回包。配置分工：`base_url`/`model` 落 `ide.cfg`，**API Key 只落
>    Windows 凭据管理器**（不进明文文件）。底层三层库全部无头可测：
>    `nodio.ai.http`（WinINet 带头请求）、`nodio.ai.cred`（凭据管理器）、
>    `nodio.ai.chat`（OpenAI 兼容的会话/请求体/响应解析）。
> ② **四个 Win32/官方库坑**（详见 `docs/capability-gap.md` §1.43.2–§1.43.6）：
>    `CREDENTIALW.LastWritten` 是 **FILETIME（8 字节）**不是 SYSTEMTIME（16
>    字节）（错了 → `CredWriteW` 恒报 87）；`HttpSendRequestW` 是 **5 参数**
>    原型（按 7 参声明 → **x86 stdcall 栈冲歪 → 0xC0000005**）；
>    `HttpQueryInfoW` 返回 **UTF-16**（按 UTF-8 解 → 状态码恒为 "2"）；
>    官方 richedit **没有 `append`** 方法（只有 `appendText`）—— 面板的聊天
>    日志曾因此**永远空白**；`thread.set` 的共享表**不是 GC 的根** → 跨线程
>    回包必须传字符串。
> ③ **引擎缺陷修复**：worker 线程退出后，**主线程的 GC 会摸到已释放的线程栈**
>    → 0xC0000005（10 行确定性复现，`tests/ide_thread_gc.nodio`）。根因是
>    worker 的解释器是**线程栈上的局部对象**，而 `Interp::globals` 是它的
>    成员 → 线程一退出，栈被回收，GC 仍可能顺着 `envUp`/`parent` 摸到它。
>    修法：解释器改堆分配 + 挂在 `ThreadState` 上保活。**与 AI 面板无关**，
>    任何 `thread.create` 都会踩到。
> ④ 回归：新增 `tests/ide_ai_check.ps1`（**83 项**）与
>    `tests/ide_thread_gc_check.ps1`，跑批脚本 15 项 → **17 项**；
>    `tests/ide_smoke.ps1` 增 AI 面板端到端断言（轮询到请求结束再断言，
>    不用固定时间点 —— worker 首次要自己解析模块，x86 比 900ms 还慢）。
>    **x64 17/17、x86 17/17 全绿**。

> **第三十八轮（AI 面板的「成功回包」也变成无头断言 + mock 孤儿进程治理）**。
> ① **补上盲区**：上一轮冒烟只断言 AI 面板的**失败**路径，**成功**路径
>    （AI 的回答进日志 + 进会话）从来没被断言过 —— 面板收包那半边是裸奔的。
>    补法：`tests/ai_mock_server.py` —— 只依赖 Python 标准库的**本地 mock
>    OpenAI 接口**（`http.server`）：绑 `127.0.0.1:0`、stdout 打 `READY <port>`、
>    POST 任意路径回 200 + OpenAI 形状 JSON（内容固定为「MOCK-OK 你好…」）、
>    路径含 `/fail` 回 500、`GET /ping` 回 200。端口经 `NODIO_IDE_AI_MOCK_PORT`
>    传给 IDE；**不依赖外网、不碰用户真实 Key**（凭据仍走 `NodioIDE/test/...`
>    隔离名），没装 Python 就降级成失败路径-only（冒烟不报错）。
> ② `ide/nodio/main.nodio` 的 AI 段改成**两段状态机**：阶段 0 成功（打 mock）
>    → 断言日志里有 `AI: MOCK-OK`、会话 2 条消息、`busy` 回 false；阶段 1
>    失败（打 `127.0.0.1:1`）→ 断言 `[错误]`、会话 3 条消息（失败那次
>    **用户消息仍留在会话里**）。新增 8 个断言键（`AI-SEND-MOCK/USER/REPLY/
>    MSGS/BUSY` + `AI-SEND-ERRUSER/ERR/ERRMSGS`）。
> ③ **三个进程/时序坑**（详见 `docs/capability-gap.md` §1.43.8）：
>    (a) mock 的 stdin 在 `Start-Process -NoNewWindow` 下**立刻 EOF** →
>    `shutdown()` 在 `serve_forever` 真正开始前就置位 `__shutdown_request`
>    → 服务器秒退，症状是「READY 打出来了，一连就 Connection refused」；
>    改成 **`--parent-pid` 看门狗**（`OpenProcess(SYNCHRONIZE)` +
>    `WaitForSingleObject`，内核在父进程终止时唤醒，不用轮询）。
>    ⚠ 探活**不能**用 `os.kill(pid,0)` —— Windows 上它会 `TerminateProcess`，
>    那是把父进程杀了。
>    (b) `BaseServer.shutdown()` 只在 `serve_forever()` 里能被叫停 —— 手写
>    `handle_request()` 循环时调用它会**永久阻塞**（`__is_shut_down` 只有
>    `serve_forever` 的 finally 才置位）。
>    (c) **孤儿 mock 进程会让整条调用「看起来卡住」**：它与父进程共用控制台
>    → 占着控制台句柄 → 调用方 stdout 管道等不到 EOF → 命令不返回
>    （实测：冒烟早打印了 `SMOKE OK`，调用方仍在等）。三道保险：mock 侧
>    看门狗 + `tests/ide_smoke.ps1` 侧把主体整段包进 `try/finally` 兜底
>    `taskkill /T /F`（PowerShell 的 `exit` 在 `try` 内也会执行 `finally`，
>    十几条 `exit 1` 的失败路径同样收得干净）+ 按命令行扫一遍回收
>    「上次被中断」留下的孤儿（沿用仓库既有的 `run_vm_tests.ps1` 对
>    `webroot\_srv.py` 的同一套做法）。收尾实测 **leftover mock procs = 0**。
>    另：Python 现在是 IDE 冒烟的**硬依赖**（起不来 mock → 成功路径没法断言
>    → 这条回归等于白跑），所以脚本**明确报错**而不是静默降级；
>    找 Python 的方式与 `run_vm_tests.ps1` 一致（PATH → `C:\Python311\python.exe`），
>    仓库里本来就有 5 个测试脚本依赖 Python，不是新引入的负担。
> ④ 回归：引擎本轮**未再改动**（`common.h` + `builtins.cpp` 沿用上一轮的
>    GC 悬垂指针修复），两个架构都按当前工作树重建；
>    **x64 17/17、x86 17/17 全绿**。

> **第三十九轮（import / 加载路径分配治理——普查驱动）**。
> ① 普查工具（allocstat.cpp 常驻）：全局 new/delete override + 阶段归账
>    + 尺寸直方图 + 阶段×尺寸交叉 + >64KB 大块栈位点（dbghelp 符号化），
>    NODIO_ALLOC_TRACE=1 门控；定位不再靠调试堆放大倍数猜。
> ② 优化五项：PNode 节点池（allocate_shared+自由链表，256B 桶 13.0万→6.5k）、
>    Lex 预留 + Parser move（token 倍增 churn 消失）、VarAt 几何增长、
>    **槽空间封顶 kSlotMax=512 + 整帧退化 map**（槽下标=全局 intern id 的
>    尺度病：晚定义帧为几个局部撑 max-id×328B 槽向量；exec 字节 -74%）、
>    BootTrace 计时器全门控（Ident 无条件 QPC 每语句白付 ~60ns）。
> ③ 数字（同日同机 A/B，HEAD=8175aba）：import win.ui 家族 281→250ms(-11%)，
>    exec 分配 105.7→27.3MB，总分配 358k→220k 次 / 185→61MB，callbench
>    变参 -2.5~3×、其余打平，冷启动打平。
> ④ 结构挂账：Value=328B（7 shared_ptr + std::function）→ FuncData 侧表
>    重构挂下轮；round-35 台账 µs 表今日 HEAD 无法复现，以同日 A/B 为准。
> ⑤ 回归：x64 17/17、x86 17/17（槽封顶后、门控后各一轮），gcstress ×4。
>    并行 IDE 会话的第三十七轮引擎修复（thread.worker GC 悬垂）按 hunk
>    拆分先行单独提交（694a399）。

> **第四十轮（编辑器字体「时大时小」—— 一个键名写错，Consolas 静默变成了 Segoe UI）**。
>    （详见 `docs/capability-gap.md` §1.45）
> ① **现象**：代码编辑框里同一行的中文和英文**大小明显不一致**、风格也不同，
>    整片代码看上去「时大时小」（截图 `build-x64/Release/shot_font_ide.png`，
>    150% DPI 下第 8 行「主脚本必须在」明显大于同行的「app.shell」）。
> ② **根因**：官方 `LOGFONT` 类的字体名字段叫 **`name`**（`WORD name[32]`），
>    构造函数是 `for(k,v in arg){ this[k] = v; }` —— **逐键原样拷贝**，
>    认不得的键被当垃圾塞进结构体、**不报错**。而 `lib/nodio/ui/editor.nodio`
>    写的是 `ctrl.setFont({faceName="Consolas"; point=…})` →
>    `name` 保持默认值 → 触发「Tahoma → **Segoe UI**」兜底 →
>    **Consolas 从来没生效过**。于是代码是**比例字体**（列对不齐），
>    汉字又走 GDI 字体回退（另一套字体、字面更大）→ 「时大时小」。
>    这一行还包在 `try{…}catch(e3){}` 里，异常被吞，所以既没报错也没日志。
>    实测对照：`::LOGFONT({faceName=…}).name == "Segoe UI"` vs
>    `::LOGFONT({name=…}).name == "Consolas"`。
> ③ **修复**：两处 `setFont` 改用 `name=`（`point` 保持不变 —— 官方
>    `setFont` 自己会 `stripPoint` 并除以 DPI 缩放，不能再乘 `dpiScale`）；
>    顺带修三处**口径不一致**：行号栏画板字号把 **pt** 当**逻辑像素**用
>    （→ `uiFontSize`）、**分栏对比编辑器硬编码 `fontSize = 11`** 不跟设置走
>    （→ `app.settings.fontSizeGet()`）、改字号时没通知分栏那个编辑器
>    （→ 同时 `shell.editor2.setFontSize(n)`）。
> ④ **回归锁**：新增 `tests/ide_font/font.nodio` + `tests/ide_font_check.ps1`
>    （**9 项**），跑批 17 项 → **18 项**。断言不读 HFONT（我们引擎的
>    `::SendMessage` 回不了指针型返回值，`WM_GETFONT` 恒为 0），
>    改用**等宽性**：等宽字体里 `"iiiiiiiiii"` 与 `"WWWWWWWWWW"` 等宽，
>    比例字体不等宽（实测 81/81 vs 31/131）。末尾一条**反证**：
>    故意用错键名再设一次，断言字体确实变回比例 —— 保证不是假绿。
>    冒烟另加 `DOC-FONT:主编辑器/设置` 与 `DOC-FONT-DIFF:分栏编辑器` 两条。
> ⑤ 回归：**x64 18/18、x86 18/18 全绿**。
>
> ⑤′ **补盲区：给字体锁加「字号真的变了」的判据**（详见 §1.45.8）
>    上一轮那条锁有洞：字号相关的三条断言**全都只读返回值**
>    （`setFontSize` 返回 true / `getFontSize` 回显 14 / 仍然等宽），
>    `setFont` 一旦**静默失败**，三条照样全过 → **假绿**。
>    补法：字号变了，**排版必须跟着变** —— 字宽（`EM_POSFROMCHAR` 低 16 位）
>    与行高（高 16 位作差）都从控件里直接量，**不依赖任何返回值**。
>    实测（96 DPI）：11pt→81px/17px，14pt→101px/22px，18pt→131px/28px，
>    调回 11pt 精确回到 81px/17px。断言用**比值**（与 DPI 无关，实测 1.247，
>    失败点 1.00），阈值 `w14*100 > w11*115`。
>    **变异测试证明判别力**：把 `setFontSize` 的 point 写死成 11 后，
>    旧的 8 项**全 PASS**（假绿坐实），新增的 4 项**全 FAIL**。
>    字体锁 9 项 → **17 项**，双架构 17/17；跑批仍 **18/18**。
>
> ⑥ **顺带修的回归夹具**：`ide_ctx` 在批量跑批里**间歇性假红**、单独跑恒绿
>    —— 与字体无关，是**共享桌面的剪贴板争用**（本机剪贴板被别的进程**高频
>    短暂占用**：`GetOpenClipboardWindow` 连采 5 次都看不到持有者，但 .NET 的
>    `Clipboard::SetText` 稳定报「所请求的剪贴板操作失败」；仓库自己的
>    `win_clip/clip.nopio` 也打印 `CLIP-OK 0`）。根因是夹具**复制后直接回读、
>    不校验**：写失败时回读拿到的是**上一次遗留的内容** → 单独跑时遗留正好是
>    `"hello"` → **碰巧通过**（假绿），跟着别的测试跑 → **间歇性假红**。
>    改法沿用仓库既有思路（`win_clip/clip.nopio` 的「每步重新探测 + 按门选
>    期望值」）：复制/剪切/写入/粘贴**每步都「操作 → 回读校验 → 不对就重试」**，
>    任何一步对不上就整组退成 `EDIT-CLIP-SKIP`（右键链路的断言一条不放松）。
>    两条分支都验过：成功语义用内存假剪贴板跑出**逐行对齐 `ctx.expect`**，
>    失败语义用实机（剪贴板正被锁）验过。



> **第四十一轮（Value 侧表化——调用路径近乎腰斩）**。
> ① FuncData 侧表：9 个函数专属字段收进 shared_ptr，sizeof(Value)
>    328→136B（-58%），拷贝 7→3 次原子操作；232 处访问点编译错误驱动
>    修补。② native 裸指针化：std::function(64B 内联) → 裸指针+可选
>    上下文，407 个 NF 注册点零改动，无捕获 lambda 零分配。
> ③ 三类真坑：创建点漏分配 fnData（builtin 注册期崩）、GcMarkValue
>    无条件解引用（首次 GC 崩）、共享侧表写透（跨线程 DeepCopy 原地
>    reset 清掉原函数闭包——t28/t29 抓获，先深拷贝再清）。
> ④ 数字：全局 2 参调用 4µs→1.8µs(-55%)，方法 4µs，变参 2.7µs，
>    native 0.14µs；import win.ui 250→172-188ms(-30%)；exec 分配字节
>    27.3→11.6MB；uiprobe13 WS 增量 20-54→11.7-14.9MB。
> ⑤ 回归：x64 18/18、x86 18/18，gcstress ×2。第四十轮（编辑器字体，
>    并行 IDE 会话）未提交内容经核对后随行落账。

> **第四十二轮（发布链路补齐——IDE 发布入口 + 发布器认 .nproj）**。
>    （详见 `docs/capability-gap.md` §1.47）
> ① 盘点：菜单/钩子面基本齐（唯一占位「工程属性」），真缺口是发布链路
>    ——发布器引擎与 CLI 早已存在且有回归锁，但 IDE 无任何发布入口
>    （能写/跑/调试，出不了货），且发布器只探测 default.aproj 不认
>    本仓库原生的 .nproj。「最近文件」列表（需设置持久化+菜单重建）推迟。
> ② 发布器：清单解析顺序 = 显式 `--proj <manifest>`（不存在则报错，
>    防拼错静默退化）→ default.aproj → default.nproj；ParseAproj 原样
>    复用（embed 缺省 false，数据文件要 embed="true"，脚本走 import
>    闭包）；CLI/env 各增一项（--proj / NODIO_PUBPROJ）。发布回归
>    7→10 项（default.nproj 自动探测 + custom.nproj 显式清单）双架构绿。
> ③ IDE 侧：runner 增 runArgs（复用流式轮询，run() 行为不变）；
>    main.nodio 增 publishRun/publishExe/真「工程属性」对话框；工程菜单
>    增「发布为独立 exe（控制台/界面程序）」两条显式入口（不猜 --gui）。
> ④ 冒烟锁：NODIO_IDE_SMOKE 增发布段端到端三断言 PUB-START/EXE/RUN
>    （token=time.tick() 每次不同，防旧产物假绿；runner 单槽 → 先发布
>    后 F5 回声）。坑：popen startInfo 塞 environment 表实测必报
>    （popen.aardio:63）→ 改「spawn 前后临时清父进程 env」（空串≡删除
>    变量，main.cpp `>0` 判定等于未设置）。
> ⑤ 回归：发布 10 项 ×2 架构、冒烟 ×2 架构全绿；全量 **x64 18/18、
>    x86 18/18**。

> **第四十三轮（最近文件——「文件」菜单动态段 + cfg 持久化）**。
>    （详见 `docs/capability-gap.md` §1.48）
> ① 缺口清单 §5.2「最近使用的文档 ❌」收掉（上轮挂账）。方案：cfg 一行
>    `recent.files=路径1|路径2|...`（Windows 路径不可能含 |，分隔符安全）；
>    自绘菜单打开下拉时**现读** items 表 → 「文件」条目每次重建 +
>    setMenus 即生效，无需二级子菜单机制；openFile 一处入队（工程/搜索
>    跳转/钩子打开的文档都收，工程本身不收——已有 lastProject 恢复）。
> ② 实现：settings.recentGet/recentPush（去重置顶、截 10 条）；
>    shell.buildFileMenuItems（固定段+动态段，条目 N.  完整路径）+
>    refreshRecentMenu/fileMenuLabels 导出；main.openFile 成功分支入队。
>    闭包工厂 mkOpen 把路径绑进参数——循环里直接写闭包会共享循环变量。
> ③ 坑：单变量 for in 对**数组**拿到的不是元素值（第一版 recentGet
>    四条断言全 0 但无异常）；split 结果一律计数循环+下标（仓库惯例）。
> ④ 回归锁：冒烟四断言 RECENT-AUTO/TOP-OK/DUP-OK/MENU-HIT；x64/x86
>    冒烟全绿；全量 **x64 18/18、x86 18/18**（x64 首轮 vm/winclip 失败
>    为剪贴板争用环境假红，重跑即绿）。

> **第四十四轮（全面审计四连修——递归失控全链清剿）**。
> ① 审计挖出 4 个绕过 500 层守卫的段错误向量：GcMark 深链递归
>    （10-20 万层崩，P0）、线程 DeepCopy 深链（≤5 万层崩）、解析器
>    深括号（纯解析 3 万层崩）、native→脚本重入三入口无计数。
> ② 修复：mark/拷贝迭代化（显式工作栈，语义逐路保持，顺修 fromIsEnv
>    诊断错表）；重入计数器上移共享（Call 节点/CallThisOwner/CallVmFunc，
>    纯脚本上限 500 不变）；解析 1000 层封顶 + 词法器 EOF 字符串报错。
> ③ 回归锁 t93-t97 五件：深表+GC 波、线程深拷贝、深括号、未终止字符
>    串、守卫边界不误伤。④ 四个原崩溃探针全部转绿；vm 94/0/1；
>    双架构全量回归绿。§1.47/1.48（并行会话发布链路、最近文件）随行落账。

> **第四十五轮（审计 P2 尾巴——投毒短路 + EOF 行号）**。
> ① 算术投毒短路：err 全 42 写入点核实均为硬错误后，EvalBin/一元在
>    操作数求值后查 err 非空即透传 null——递归守卫展开不再捏造
>    「500」类垃圾数字，try 捕获后无残留；NUMASSIGN A/B 零成本。
>    毒语句的其余副作用（先打印后中止）保留为 S-2 已知偏差。
> ② EOF 哨兵取最后内容行：单行文件语法错误报 行 1（原 行 2）。
> ③ 回归锁 t98；vm 96/0/0；双架构 18/18。并行会话的 sysapi 剪贴板
>    注册不随行。

> **第四十七轮（以 RTF / HTML 格式复制——编辑菜单彩色复制两条）**。
>    （详见 `docs/capability-gap.md` §1.52）
> ① 缺口清单 §5.1「以 RTF / HTML 格式复制 ❌」收掉。语义=复制**整篇**
>    （带色贴进 Word/邮件/博客），复制后选区还原。RTF 走 richedit 原生
>    copy（真 CHARFORMAT 序列化），复制前强制全文着色补偿（平时只着色
>    可见范围；显式命令可接受数秒，copyHtml 无此代价）；HTML 由本地
>    词法器 token 流直出（<pre> 包裹 + token 级 span，颜色同 self.colors
>    主题跟随；&<> 转义；MAX_TOKENS 截断降级纯文本），CF_HTML +
>    CF_UNICODETEXT 同会话写入兜底。
> ② 字节保真配方（探针 2b/2c/2d 钉死）：raw.buffer 落字节 +
>    RtlMoveMemory void(addr,addr,int) 纯指针拷贝 + GMEM_ZEROINIT 兜
>    终止符；CloseClipboard 后 Sleep(30)（win.clip.write 同款）——缺这拍
>    本机剪贴板管理器在 Close 瞬间清板。
> ③ 坑：string.unpack 走 BStr 对 bRawBytes 非 ASCII 双重编码 → 中文断言
>    用「同管线重建期望字节 + 全量 ==」（== 走 StrByteForm，同通道可靠）；
>    __buildHtml 转义实体曾写成无操作 + <pre> 漏实现（ASCII 断言查不出，
>    全量比对抓出）；CF_HTML pad=1 终止符计入期望字节。
> ④ 回归锁：editor_clip_probe 挂进 run_editor_tests（RTF 头 + 文本一致；
>    HTML 期望字节全量相等 + 文本一致；整写验循环重试抗清板）。编辑器
>    4 探针 ×2 全绿；全量 **x64 18/18**（vm 首轮假红）、**x86 18/18**。
>    附带修 ide_nav 硬编码 NAV-JUMP-LINE:44 → 动态取 NAV-DEF-LINE
>    （门面插行实测假红）；修 .gitignore 误盖四个编辑器回归锁探针
>    （前三个官方用例从未入库）。⑤ sysapi 剪贴板 API 注册（§1.50/1.51
>    挂账 WIP）随本轮落账。
