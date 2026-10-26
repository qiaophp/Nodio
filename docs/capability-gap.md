# Nodio 能力缺口清单（对标官方 aardio）
测量日期 **2026-10-20**。对照基准：`D:\aardio`（aardio.exe 文件版本 43.0.0.0 / 产品版本
46.8.3，6,902,784 字节，官方 lib 869 个 `.aardio` 源码文件，2,136 个非空 `.aardio`）。

本文只写**实测到的缺口**，每条给出复现命令或证据位置；不写估计值。
仓库既有台账 `docs/roadmap.md` 的若干分数已过期，本文末尾列出需要更新的行。

> **修复轮次说明（2026-10-20 第二轮）**：本文第一轮记录的是「审计时点」的状态。第二轮按本清单
> 逐项修复后，**P0-1 / P0-2 / P0-3、E-1 / E-5 / E-6 / E-7 已修并复测通过**，E-3 / E-4 判定为
> **测试期望写错**（官方契约另有规定，见 §2），E-8 判定为**官方同款限制**（不需要修）。
> 本轮还新发现并修掉 3 个 P0/P1 级缺陷（字符串表示归一、GC 跨线程堆损坏、调试器断点越界），
> 详见 §1.4。**当前实测：VM 回归 85 passed / 0 failed / 1 skipped（x64 与 x86 同；skip 为
> t23 的环境门控，本机无 OpenGL），发布回归全绿，命名空间 54/54，969 模块 925 通过。**
> 下面各节保留第一轮的原始记录（未标注"已修"的段落仍是现状缺口）。
>
> **第三轮（内存泄漏根因 + 设计器组件层）**：用户指出"有很严重的内存泄漏问题"，并要求
> **不靠频繁 GC 掩盖**。本轮定位并修掉 4 个独立根因（不是 GC 频率问题）：自动 GC 从未启用
> （P0-7）、正在执行的帧不在 GC 根集（P0-8）、**fiber 的执行上下文不随 fiber 迁移**（P0-9
> 系列，见 §1.7）、以及被遗弃的 fiber 永久钉住挂起局部变量（P0-9c）。附带修掉
> `win.form` 第二次创建必然失败（P1-8）、`tabstrip` 首次绘制崩溃（P1-9）、设计画布鼠标交互
> 整个没生效（P1-10）与其背后的**每控件一份回调 = 永久泄漏**（P1-11）、引擎控件缺
> `setPos`/`show`（P1-12）、**裸 `::` 系统 API 解析不了导致官方 `win.ui` 事件路由全哑**
> （P1-13）、官方控件级路由装不上窗口过程（P1-14）、以及工具箱/属性格用原生
> treeview/listview 根本跑不起来（P1-15，已改为自绘）。
> **当前实测：VM 回归 86 passed / 0 failed / 1 skipped（新增 t89 固化 fiber 语义），
> IDE 冒烟 `SMOKE OK`（exit 0），GUI 压力探针工作集收敛且不再挂死。**
> 未修的一项（引擎侧 `raw` 回调释放接口）已记为 E-11；E-10 已部分修复（签名表）。
>
> **第四轮（右键 · 官方源码级 `win.ui` 兼容路线）**：用户诉求是「右键」，选定路线是
> **不造自有的右键 API**，而是让**官方 aardio 源码级 `win.ui` 在 Nodio VM 上真跑起来**，
> 右键事件由官方 `win.ui.tracker` 提供。依赖扫描结论：官方 `import win` / `import win.ui`
> **早已能跑**（官方 `class win.form`、`win.ui.menu`、官方 `util.metaProperty` 全部可用），
> 唯一真实缺口是**官方 tracker 的右键事件不触发**。根因在引擎语义：ctor 里
> `this = obj` 重绑定之后，成员赋值仍被当成「成员初始化」而**绕过元表 `_set`**，
> 于是 tracker 的消息表从未装上。修法是让 ctor 帧记住**正在构造的实例**并按实例身份判定，
> 同时保住官方 `gdip.family`（ctor 写只读属性）语义。详见 **§1.9**。
> **当前实测：VM 回归 88 项全过（x64/x86），发布回归全绿，命名空间 54/54，
> IDE 冒烟 SMOKE OK，969 模块 sweep **957 通过 / 12 残差**（残差全部是 32 位 DLL 物理边界
> 与官方自身缺陷，见 §7；上一轮记的 44 里有 32 项是 sweep 工具链的 exe 路径缺陷造成的
> 假失败，本轮修正后消失）。新增 t90 固化右键链路
> （控件层 + 窗体层 `onContextMenu` / `onRightMouse*` + 官方菜单 API）。**
>
> **第五轮（IDE 层右键菜单）**：把第四轮打通的官方右键事件接到 IDE 的三个真实落点
> ——**编辑器 / 工程树 / 设计画布**，组件层在 `lib/nodio`（`board` / `projectTree` /
> `canvas`），IDE 层在 `ide/nodio/app`（`editor.contextMenu` 用官方 `win.ui.tracker` +
> 官方 `win.ui.popmenu`；工程树与画布菜单用 `popId` 分发）。接线过程中挖出一个
> **此前从未暴露的引擎级缺陷**：官方 `lib/win/api.aardio:11` 把 `::CallWindowProc` /
> `::DefWindowProc` 声明成 **int 返回**（32 位进程的写法），在 Nodio x64 下会把 64 位
> `LRESULT` 截成 32 位 → 官方 `treeview` 装上 wndproc 后 `HTREEITEM` 只剩低 32 位 →
> **插入子节点直接失败**。已修（`engine/builtins.cpp` 的 `api()` 在 x64 下把这两个 API
> 的返回类型提升为 `int64`）。另修 `win.getMessagePos()` 无参时返回真实光标位置
> （官方 tracker 的 `lParam == -1` 回退依赖它）。新增回归夹具
> `tests/ide_ctx/ctx.nodio` + `tests/ide_ctx_smoke.ps1`（x64/x86 双 OK，18 行断言）。
> 详见 **§1.11**。
>
> **本轮实测**：x64/x86 VM 回归各 **88 passed / 0 failed / 0 skipped**，发布回归全绿，
> 命名空间 **54/54**，IDE 冒烟 **SMOKE OK**，新增 IDE 右键回归 **CTX-SMOKE OK**（x64+x86），
> 969 模块 sweep **953 通过 / 12 FAIL / 4 长时项**（12 项 FAIL 与第四轮残差完全一致、
> 无新增功能失败；4 项长时项是下载/安装型模块，已用不含本轮 x64 改动的 x86 二进制
> 做同结果对照，确认与本轮改动无关）。
>
> **第五轮（IDE chrome 自绘化 + 设计器闭环）**：把 IDE 的页签 / 工具条 / 面板 / 画布改成自绘。
> 推进中定位到一整族**「官方 `win.ui` 控件方言」与「引擎原生控件方言」互不兼容**的缺陷——
> 同一份代码里 `setPos` 一处是 `x,y,宽,高`、另一处是 `l,t,r,b`，`text` 一处是属性一处是函数，
> 混用正是「布局错乱」的根因。修法是在标准库加方言无关封装 `nodio.ui.rect`（统一走原始
> `win.setPos`、物理像素），并踩实了官方 `form.add` 的两个隐藏机制（`autoResize` 会把控件
> **弹回设计时矩形**、创建矩形按 DPI 比例缩放），另外实测**三条官方能力在本机走不通**
> （`ctrl.setPos` 触发引擎断言崩溃、`plus` 会把父背景整块拷进自己的画板所以透明 overlay
> 不可行、`static` 的 `bgcolor` 依赖 `WM_CTLCOLORSTATIC` 画不出来）——分别绕开。
> 同时落地：dock 去子窗体、文档页签自绘、画布 8 手柄、属性格真正回写（含几何）、
> DSG 往返（类型保持、输出稳定）、工具条顺序固定。详见 **§1.10**。
> **当前实测：VM 回归 88 passed / 0 failed / 0 skipped（x64），IDE 冒烟 `SMOKE OK`（exit 0，
> 新增 PROP-*/DSG-*/TABSTRIP-*/VIS-* 硬断言），截图复核通过。**
>
> **第六轮（外观基准换成现代深色 IDE + 主题体系）**：用户指定新的外观参照
> `aar.chengxu.online/thread-903.htm`（aardioX / QuickEditor4，现代深色 VS Code 风格），
> 不再照官方老 IDE 的 MFC 浅色界面做。参考图留档 `docs/ref/aar-ide/`（10 张），
> 逐条抄下来的规格见 `docs/official-ide-reference.md` §0。
> 新增标准库 `nodio.ui.theme`（深色/浅色两套配色 + 切换 + ARGB→COLORREF）；
> 页签条扁平化、**工具条与状态栏整条改为自绘**、工具箱/属性格改主题配色、
> 窗体底色用 z=0 自绘底板、编辑区用官方 richedit 的 `setCharformat` 染深色。
> 顺带修掉两个真缺陷：设计画布不随 DPI 缩放铺满编辑区（官方 `onSize` 回调跨作用域，
> 模块钩子读出来是 null → 改由心跳驱动重排）、主窗体偶发「建出来但不显示」
> （约 1/4 概率 → 加可见性自检 + 兜底 `ShowWindow`）。
> 另记一个**会崩进程**的坑：原生 treeview/listview 的配色消息在本机 comctl32 里
> 会把 lParam 当指针解引用 → `0xC0000005`。详见 **§1.11**。

---

## 0. 结论速览

| 面 | 官方 aardio 能力面 | Nodio 实测 | 状态 |
|---|---|---|---|
| 语言/引擎 | 完整 aardio 语言 + 原生 FFI | VM 回归 **88 passed / 0 failed (x64/x86)** | 达标（第一轮为 80/85） |
| 命名空间导入 | 54 个 | **54/54 (x64)** | 已达标（台账写 45/54，过期） |
| 官方源码级 `win.ui` | 官方 `lib/win` 源码实现 | **官方 `import win` / `import win.ui` 可跑**；右键事件（`onContextMenu`/`onRightMouse*`）由官方 `win.ui.tracker` 提供；**IDE 三处右键菜单已接线**（编辑器 / 工程树 / 设计画布） | 第四轮打通、第五轮接线（见 §1.9 / §1.11） |
| 模块行为 | 969 个公开模块 | **953/969** 导入+初始化通过（第五轮实测） | 12 项 FAIL 与第四轮残差完全一致（物理边界/官方缺陷，见 §7）；另 4 项下载/安装型模块长时项，与本轮改动无因果关系 |
| 发布器 | 资源/库/内存 DLL/PE 戳记 | 发布回归 **绿**（`res:hello from embedded resource`、`blob:776`） | 已达标（第一轮为红） |
| IDE | 官方 IDE 38 条命令 + 28 菜单 + 28 对话框 + 99 字符串表 | 自托管 IDE 约 1/4 能力面；**chrome 已按现代深色 IDE 重做**（见 §1.11） | 大面积缺 |
| 窗体设计器 | 完整可视化设计器 | canvas 8 手柄 / 属性格回写 / DSG 往返 / 自绘工具箱已通（见 §1.10） | 部分通 |
| 源码可编译性 | — | x64 / x86 均构建通过 | 已修（第一轮编译失败） |

---

## 1. 阻断级缺陷（P0）

### P0-1 发布产物读不到自己的嵌入资源

> ✅ **已修（第二轮）**：把「载荷内嵌文件系统」查找**下沉到 `io.file`**（`engine/builtins.cpp`
> 新增文件级 `struct IoFileState` + `ioFileOpen/ioFilePos/ioFileSetPos`，以及
> `EmbeddedPayloadKey(path)`）：`io.file` 构造器先查内嵌 map，命中则建只读内存态
> （写类操作返回「载荷资源为只读」），`read/readAll/readBuffer/readback/seek/tell/size/
> eof/close/flush/mode` 全部支持内存态；`io.readFile` 内建与 `raw.loadDll` 也改用
> `EmbeddedPayloadKey` 查表。同时 `io.file.read` 增加编码名分支
> （`raw/utf8/gbk/gb2312/gb18030/ansi/acp`），`apps/nodio/CMakeLists.txt` 增加
> `add_dependencies(nodio nodio-stub)`。
> **实测**：`tests/run_publish_tests.ps1` 全绿 —— `res:hello from embedded resource`、
> `blob:776`。
> （不选"修 `io.readFile` 内建"这条路的原因：官方库必然遮蔽 `io.readFile`，功能挂在那里不可达。）

`.aproj` 里 `embed="true"` 的文件**确实进了载荷**（清单可验证），但运行时读不出来：

```
$ python  # 读 tests/m7/proj/dist/main.exe 的 manifest
main.aardio        6377
mylib.aardio        88
res\blob.bin       776
res\hello.txt       29
res\test64.dll    2048
res\test86.dll    2560
```

运行已发布 exe：

```
res:null          ← io.readFile("~\res\hello.txt") 读不到
blob:0            ← io.readFile("~\res\blob.bin","raw") 读不到
```

复现（`tests/m7/proj/probe2.aardio` 无任何 import，排除 import 干扰）：
`P1 entry script :null` / `P2 res\hello.txt :null` / `P3 ~\res\hello :null` —— 四种路径写法
（`~\x`、`x`、`~/x`、`\x`）全部落空。

**根因（已定位到代码 + 实测判定）**：引擎把"载荷内嵌文件系统"查找和 `raw`/`gbk` 编码模式实现为
`io.readFile` **内建函数**（`engine/builtins.cpp:3128-3169`），但官方 `builtin` 库在导入时
用 `namespace builtin; self.io = global.io;` 把官方 `io` 命名空间**合并进** `global.io`，
官方那边 `readFile = file.read`（`D:\aardio\lib\builtin\io.aardio:162`）是"已废弃别名"，
**覆盖掉了引擎那个内建函数**；别名最终落到引擎的 `io.file` 类
（`engine/builtins.cpp:2729`），而 `io.file.read` **只认磁盘 FILE\***，没有内嵌载荷查找。

实测判定（`tests/vmtests/ioprobe.aardio`，磁盘上确实有 3 字节文件）：

```
disk file size  : 3
io.file.read()  : ABC          ← 引擎 io.file 类，正常
io.readFile(raw): null         ← 引擎内建会给 3 字节；null 说明走的是官方别名
io.readFile()   : ABC
io.readFile(1)  : A            ← 别名把第 2 参当字节数，正是官方契约
io.joinpath2    : function     ← 只有官方库有 → 官方 io 命名空间确实在起作用
io.localpath    : native function  ← 引擎内建仍在 → 两者是"合并"而非"替换"
```

即：`global.io` = 引擎内建 io ∪ 官方库 io，**官方库那一侧赢了**。

所以引擎的 `io.readFile` 内建函数实际是**死代码**：被官方库遮蔽。后果有三：

1. 发布产物无法读取自身资源（官方 IDE 有完整"资源视图"，这是核心能力）；
2. 文档承诺的 `io.readFile(path,"raw"|"gbk"|...)` 编码模式完全不存在
   （实测 `io.readFile(p,"raw")` 返回 null）；
3. 修复引擎后发布产物不会自动受益——**发布器用的 `nodio-stub.exe` 是一个独立目标，
   必须在每次引擎改动后同步重建**，否则发布产物跑的是旧引擎。

**修法**：把内嵌载荷查找 + 编码模式下沉到 `io.file` 类（`ioFileCtor` / `io.file.read`），
而不是挂在会被官方库遮蔽的 `io.readFile` 上；并在构建流程里把 `nodio-stub` 设为
`nodio` 的依赖目标。

### P0-2 当前源码树编译不过

> ✅ **已修（第二轮）**：`engine/gc.cpp` 的类型错误由并发开发会话补完，
> `cmake --build build-x64 --config Release` 与 `build-x86` 均构建通过。
> （本轮另修掉一处同源编译错：`EnvFlag(...) != nullptr` → `EnvFlag(...)`，`EnvFlag` 返回 `bool`。）

```
engine\gc.cpp(123,42): error C2664: 无法将参数 1 从"_Ty2"转换为"const NodioVM::Value &"
```

`engine/gc.cpp` 是**未入库的新文件**（`git status` 显示 `?? engine/gc.cpp`），是一份写到一半的
标记-清除 GC 实现；`v.pdefaults` 的实际类型是 `shared_ptr<vector<PNode>>`（AST 节点），
不是 `vector<Value>`。同时 `engine/import.cpp`、`engine/common.h`、`engine/interp.h`、
`engine/runtime.cpp` 也在改动中。

后果：`cmake --build build-x64 --config Release` 失败 → 现在的 `build-x64/Release/*.exe`
是**改动前**的产物，所有实测数字都对应旧二进制；`nodio-stub.exe`（2026-10-19 23:41）
比 `nodio.exe`（00:16→00:48）还旧一个多小时。

### P0-3 错误路径把宿主打崩（可复现）

> ✅ **已修（第二轮）**：由并发开发会话修复错误路径收尾。
> **实测**：`tests/vmtests/crashcall.aardio` 正确输出
> `运行错误: 尝试调用非函数值 (f) @ 行 5` 并以 exit=1 干净退出，无 FATAL CRASH。
> （第十三轮 S-2 之后，位置会带上文件名：`@ <脚本>:<行号>`。）

```aardio
// tests/vmtests/crashcall.aardio
io.print("start");
var f = 123;
f();
```

```
start
运行错误: 尝试调用非函数值 (f) @ 行 5
[GC] live tables=45 envs=26 alloc=171
[FATAL CRASH] code=0xC0000005 at addr=00007FF6AC29ACAF
exit = -1073741819
```

错误文案是对的，但**错误处理之后的收尾阶段访问违规**，进程以 0xC0000005 退出而不是干净
返回非 0 退出码。官方语义里这是可捕获的普通运行时错误，用户脚本里极常见
（拼错函数名、误用变量）。崩溃点在 `engine/entry.cpp` 的退出清理路径（`GcDumpLive()`
之后），大概率是错误展开时留下的悬垂帧在静态析构期被再次释放。

**影响面**：IDE 的 F5 跑用户脚本时，脚本里一个笔误就能让整个 IDE 宿主进程崩掉。

### 1.4 第二轮新发现并已修的缺陷

这三条不在第一轮清单里，是修复过程中暴露/触发的，均已修并有实测。

| # | 缺陷 | 根因 | 修法 | 实测 |
|---|---|---|---|---|
| P0-4 | **多线程脚本堆损坏**：t29（两个 `thread.create`）随机崩在官方库初始化处，退出码 `0xC0000374`（STATUS_HEAP_CORRUPTION）或 `0xC0000005`，**崩溃点每次都不同** | ①`LiveTableSet()`/`LiveEnvSet()` 是全局 `unordered_set`，主线程与 worker 线程无锁并发 insert/erase → 堆损坏；②GC 的 mark 阶段只覆盖**当前 Interp** 的根，worker 线程的对象会被主线程当垃圾扫掉（提前释放 → use-after-free）；③`g_gcRoots`/`g_gcEpochStack` 是全局 vector，push/pop 的"栈"语义跨线程互踩 | 注册表加 `std::recursive_mutex g_gcRegistryMutex`；`g_activeVmThreads` 计数 >1 时**暂停回收**（`GcCollect` 直接返回）；`g_gcRoots`/`g_gcEpochStack` 改 `thread_local`；`g_liveTables/g_liveEnvs/g_gcAllocTick/g_gcEpoch` 改 `std::atomic`；`Interp` 构造/析构里增减活动解释器计数 | t29 连跑 **12/12** 输出 `11` + `true`，无崩溃；VM 回归 85 passed / 0 failed |
| P0-5 | **字符串 `==` / `<` / `++` 不做表示归一**：源码字面量（宽字符文本）与从文件读回的字节串内容相同却判定不等 | Nodio 内部字符串有两种表示：宽字符文本 与 一字节一字符的字节串（`bRawBytes`）；官方 aardio 字符串**本质是字节数组**（`docs/language-reference/datatype/string.md`），`==`/`++` 按字面比较 | `engine/common.h` 新增 `Utf8Bytes`/`StrByteForm`/`ValueByteForm`；`CmpLess`/`ValuesEqual`/`..`/`++`/`+`/print 全部走字节形式；`bUtf16`（ustring）按 **UTF-8 解码内容**比较（官方代码一律先 `string.fromUtf16` 再比较），不是按 UTF-16LE 字节 | t40 由 `equal:false` → `equal:true`（`size:9/bytes:9/gbksize:7/gbkequal:true`）；探针 `tests/vmtests/eqprobe.aardio` 由 `equal:false` → `equal:true` |
| P1-6 | **调试器断点越界**：断点按**行号全局匹配**，加载官方库（`builtin/_.aardio` 等）时其第 3 行也命中断点 → 停 6 次、变量列表错乱、脚本被"调试中断" | `ExecBlock` 里 `breakpoints.count(st->line)` 不区分文件 | 断点/单步只在**主脚本文件**内生效（`g_curModuleFile` 为空即主程序；导入的库模块执行时它持有模块路径） | t26 转绿：只停 2 次（`v` 命令重打印一次横幅），变量 `a=1 b=2`，随后输出 `c=3` |

---

### 1.5 第三轮：GC 根可见性缺陷（"内存泄漏"的真正根因）

用户报「有很严重的内存泄漏问题」「内存泄漏问题不是频繁GC来解决的吧」。**结论：不靠调勤 GC，而是定位到
"对象为什么不可达 / 为什么该回收的没回收"。** 最终定位到 **两个独立缺陷**，都不是 GC 频率问题。

| # | 缺陷 | 根因 | 修法 | 实测 |
|---|---|---|---|---|
| P0-7 | **自动 GC 从来没跑过**：分配物永不回收，GUI 每帧累积 gdip 对象（2000 次重绘 +104MB 线性增长） | `Interp::MaybeCollectGC` 用 `0` 同时表示"未初始化"和"关闭自动 GC"，**未设环境变量时 `s_fixed==0` → `if (s_fixed == 0) return;` 把自动回收全程关掉**。此前所有"191MB→16.7MB"测量都靠探针显式调 `collectgarbage()`，掩盖了该缺陷 | 改成三态哨兵（`-2` 未读 / `-1` 自动 / `0` 关闭 / `>0` 固定阈值）；`getenv` 统一换成 `EnvFlag()`（`GetEnvironmentVariableW` 带缓冲区形式，`getenv` 与 `GetEnvironmentVariableW(name,NULL,0)` 在本进程都读不到桌面运行时注入的变量） | GUI 重绘 2000 次工作集稳定在 **44–46MB**（此前 +104MB 线性增长），活对象数稳定在 ~730 不再增长 |
| P0-8 | **正在执行的帧被 GC 回收**：GC 一旦真正启用，官方 `win.loopMessage` 的 MSG 结构被释放 → 消息循环对着已释放内存空转 → 窗口再也收不到消息（定时器停、重绘停、`close()` 无效、进程挂死） | **`Env` 的 `parent` 是强引用（指向上层），但没有任何东西从上层指回子帧** → 块作用域（`if`/`while`/`for` 体）、`with` 体、模块体、命名空间体、**主脚本帧**这些"正在执行但 VM 根图不可达"的帧，GC 一律判为死对象，清扫时把它们的 `vars` 整个清空。官方 `win.loopMessage` 的 `var msg = ::MSG()` 写在 `if(!messageLoopRunning){...}` 块内 → MSG 结构被释放 | 新增 `Interp::GcFrameScope` RAII 守卫，把 10 处 `Env` 创建点登记为 GC 根：`ExecBlockScoped` / `with` / `ForNum` / `ForIn`（3 处）/ `try-catch` / `Node::Block` / 类体 / 命名空间体 / 模块 env（2 处）/ 主脚本帧 / 线程帧 | `mAN` 探针：GC 之后 `TICK 4,5,6…` 持续触发（修复前停在 TICK 3）；`mAR` 双定时器分流实验：第二个定时器也继续跳 ⇒ 窗口活着、派发正常；工作集在 20000 次重绘/100 秒内稳定在 48–60MB 锯齿震荡（收敛），不再线性增长 |

配套的健壮性改动：终结器逐个 try/catch 且保存/恢复 VM 状态（`ret`/`nReturn`/`nBreak`/`nContinue`/`err`）；
`GcInScope` RAII 保证 `bInGC` 一定复位；原生侧持有的 VM 回调（`g_cbs` 的 `fn`/`owner`）登记为 GC 根；
可达性路径追踪（`tPath`/`ePath`）改为**仅诊断开关开启时**才记录（原先每轮 GC 都为每个对象写一条 map 项）；
`engine/common.h` 新增 `GcFrameCheck()` 不变式断言（帧栈用 `pop_back()` 弹出，任何漏 push / 多 pop 都会弹掉
**别人的**帧，使那个 env 变成"不可达但仍在使用"的对象——这正是 P0-8 的机理）。

> **教训**：VM 的根集合必须覆盖"当前正在执行的所有帧"。父子帧之间只有向上的引用，收集器无法向下发现子帧，
> 所以**每一个在 C++ 作用域里活着的 `Env` 都必须显式登记为根**。

---

### 1.6 第三轮：IDE 启动阻塞与 for 步进解析

| # | 缺陷 | 根因 | 修法 | 实测 |
|---|---|---|---|---|
| P1-7 | **IDE 冒烟测试假通过 / 启动即挂**：官方 IDE 目标界面起不来 | ①`ide/nodio/main.nodio` 用了 `app.library.scanLib()` 却**从未 `import app.library;`** → `尝试调用非函数值 (.scanLib)`；②`ide/nodio/app/library.nodio` 的两个 for 循环步进写成 `for(r=1;#roots;1++)` / `for(i=1;#files;1++)`——**对字面量 `1` 自增**，解析器把它变成"对 Num 目标赋值"，步进变成空操作 → 循环变量永不推进 → **死循环**；③`tests/ide_smoke.ps1` 在 stdout 为空时把 `$null` 当字符串处理，且用 ANSI 读 UTF-8 文件，中文全成乱码 | 补 `import app.library;`；`library.nodio` 的步进改回 `1`；解析器**拒绝给常量自增**（`parser.h` 的 postfix 分支检查目标必须是 Ident/Member/Index，否则报 `语法错误: 不能对常量自增/自减`）——把"静默死循环"变成明确报错；冒烟脚本改用 `[IO.File]::ReadAllText(..., UTF8)` 并在 stdout 为空时**明确判失败** | `tests/ide_smoke.ps1` → `SMOKE OK`（exit 0），且输出为正确中文：`text="窗体"`、`[运行]`、`[运行结束]`；`DOCK-*`/`LAYOUT-OK:1281`/`DESIGNER-ITEM:button1`/`DESIGNER-DSG`/`TOOLBOX-COUNT:14`/`PROJ-FILES:9` 全部命中 |

---

### 1.7 第三轮续：fiber 执行上下文未随 fiber 切换（t60 失败 / GC 根集错乱）

修完 P0-8 后 VM 回归只剩 t60（fiber 协程）红。**失败原因不是逻辑错误**，而是每切一次 fiber
就打出一行 `GCFRAME ...: popping X but top is Y (size=4)` 污染了测试输出——而这行断言本身
暴露了一个**真缺陷**，和 P0-8 同族。

| # | 缺陷 | 根因 | 修法 | 实测 |
|---|---|---|---|---|
| P0-9 | **fiber 的执行上下文没有随 fiber 迁移**：两个执行流共用一个帧栈，`pop_back()` 会弹掉**别人**的帧 → GC 根集错乱（"不可达但仍在使用"的对象重新出现）；控制流标志也被串味 | fiber 用 Win32 fiber 实现（**每个 fiber 一条独立 C++ 栈**），而 `Interp::gcFrames` 与 thread_local 的 `g_gcRoots`/`g_gcEpochStack` 是**线程级共享**的。fiber 在一次调用中间 `yield` 时，它的帧还留在恢复者的帧栈顶上，恢复者返回时 `pop_back()` 弹掉的是 fiber 的帧 | 新增 `FiberCtx`（`engine/common.h`）承载全部**栈式**状态：GC 帧栈 / 显式根栈 / epoch 栈 / 控制流标志（return·break·continue）/ 待返回值 / meta 派发 owner。`Interp` 持有 `mainCtx`（主流程上下文）、`curCtx`（当前上下文）、`fiberCtxs`（挂起中的 fiber 上下文）。`engine/builtins.cpp` 新增 `FiberCtxSwitch()`：切换时把活状态存回源上下文、从目标上下文载入；`FiberResume` / `FiberYield` / `VMFiberEntry` 三个切换点全部走它 | t60 转绿；VM 回归 **86 passed / 0 failed / 1 skipped** |
| P0-9b | **挂起中的 fiber 不是 GC 根**：fiber 的帧 / 显式根 / 入口函数在原生侧，VM 根图够不着 → 挂起期间被回收 | fiber 的脚本值只带一个原生指针（`__fiber` → `VMFiber*`），收集器顺着根图看不到 `VMFiber` 里的帧栈 | `engine/gc.cpp` 的 `GcMarkNative` 遍历 `I.fiberCtxs`，标记每个上下文的帧、显式根、入口函数与待返回值 | t89：`log:payload:alive:42` —— 挂起 fiber 的局部变量在强制 GC 之后仍然完好 |
| P0-9c | **被遗弃的 fiber 永久钉住挂起的局部变量** | 裁剪逻辑要用"fiber 的脚本值是否可达"判断，但上下文里存的是**强引用** → 自己把表钉住，形成自环，裁剪永远不触发 | 上下文里改存**弱引用**（`std::weak_ptr<Table>`）；值不可达时裁掉该上下文、清空入口函数，挂起的局部变量随之回收 | t89：`freed:1`（遗弃的 fiber 释放挂起局部变量）对照 `freed2:0`（仍被引用的 fiber 保持存活） |
| P0-9d | **嵌套 resume 交错控制权**：fiber A resume fiber B 时，B 的 `yield` 会把控制权交回**主流程**而不是 A | `FiberYield` / `VMFiberEntry` 固定 `SwitchToFiber(g_mainFiber)`，恢复者是谁没有记录 | 记录真正的恢复者（`resumeFiber` = 恢复者的 OS fiber 句柄，`resumeCtx` = 恢复者的执行上下文），切换时回到它 | t89：`nested1:outer:inner1` / `nested2:outer-done`；`iter:11`（fiber 里的 `return` 不会打断恢复者的循环） |
| P1-8 | **一个脚本里第二个 `win.form` 必然创建失败** | `engine/gdi.cpp` 的 `RegisterFormClass()` 里 `bool bDone = false;` 是**局部变量**（应为 `static`），每次调用都重新 `RegisterClassExW`，第二次必然 `ERROR_CLASS_ALREADY_EXISTS` | 改成 `static bool bDone` | 设计画布（子窗体）+ 主窗体同脚本共存正常 |

> **教训（与 P0-8 同一根）**：VM 里凡是**栈式**的状态（帧栈、根栈、epoch 栈、控制流标志）都属于
> **执行流**，不属于线程。fiber 是同一线程上的另一条执行流，它的状态必须随 fiber 一起走。

---

### 1.8 第三轮续：设计器组件层（自绘控件 + 窗口子类化）

| # | 缺陷 | 根因 | 修法 | 实测 |
|---|---|---|---|---|
| P1-9 | `nodio.ui.tabstrip` **首次绘制就崩**（exit 1） | overlay 里两处 for 步进误写成 `for(i=1;#self.tabs;1++)`——**对字面量 `1` 自增**。这是 P1-7 同一类笔误（解析器已改为拒绝常量自增，于是从"静默死循环"变成明确报错） | 步进改回 `1`；并对 `lib/`、`ide/` 全量扫描 `++` 步进，确认无第二处 | 标签条能正常创建、绘制、点击切换（exit 0） |
| P1-10 | **设计画布的鼠标交互根本没生效**（选中/拖动都无反应） | 画布用官方 `win.subclass` 做子类化，而官方实现调 `::SetWindowPointer`/`::CallWindowProc`；Nodio 的 `::` 只解析引擎内置符号 → 抛「尝试调用非函数值 (.SetWindowPointer)」，被 `try/catch` 吞掉 | 新增标准库组件 `nodio.ui.subclass`（`lib/nodio/ui/subclass.aardio`）：用 `raw.loadDll("user32.dll").api("SetWindowLongPtrW"/"CallWindowProcW", 显式签名)` 自实现，**所有窗口共用一个** `raw.tostdcall` trampoline 按 hwnd 分发，窗口销毁时（WM_DESTROY）自动还原窗口过程并删记录 | 点击组件能选中（`selected=true`）；移除组件后子类化已解除（`after-remove subclassed=false`） |
| P1-11 | **每个控件一份回调 = 永久泄漏** | 官方 `win.subclass` 每个窗口建一个 trampoline，而 `raw.tostdcall` 建的 `CbCtx` 进了引擎的 `g_cbs` **永不回收**，其 `fn` 还被当成 GC 根 → 回调闭包连同它捕获的一切永久钉住（画布、组件、闭包全被钉住） | `nodio.ui.subclass` 只有一个 trampoline；每个窗口只在模块表里留一条记录，销毁即删；画布 `remove`/`clear`/`dispose` 都显式 `free()` | 组件加多少都不再泄漏（见 P1-10 实测） |
| P1-12 | 引擎原生控件**缺 `setPos`/`show`**，官方 `win.setPos` 也不可用 | 官方 `win.setPos` 调 `::SetWindowPos`（同样受 P1-10 限制）；引擎自建的控件表只有 `hwnd`/`text`/`setText`/`click` | 引擎补 `win.setPos`/`win.getPos`（原生实现，语义与官方一致：`null` 表示"不移动"/"不改大小"）与控件表的 `setPos(l,t,r,b)`/`show(flag)`；窗体 `show(flag)` 支持隐藏 | 画布拖动/移除组件正常 |
| P1-13 | **裸 `::` 系统 API 解析不了** → 官方 `win.ui` 整个框架的事件路由是哑的 | 官方 aardio 的 `::Func` 会在系统 DLL 列表里查找；Nodio 的 `::` 只查引擎内置根表，找不到就把名字当 **DLL 名**去加载 → `::SetWindowPointer` 报「尝试调用非函数值」。官方 `win/ui/_.aardio` 正是用 `::SetWindowPointer` 给窗体和每个控件装窗口过程 —— 装不上，控件就一个鼠标事件都收不到 | 新增 `engine/sysapi.cpp`：一张**系统 API 签名表**（user32/gdi32/kernel32 约 90 个 API，含 `::SetWindowPointer`→`SetWindowLongPtrW` 这类别名），`::` 查表命中后按签名解析并缓存（`Interp::sysApis`） | 官方窗体级鼠标事件**已通**（探针：表单 `onMouseDown/onMouseUp` 命中 2 次）；`::SetWindowPointer`/`::CallWindowProc`/`::CreateWindowEx` 等均可直接调用 |
| P1-14 | **官方控件级事件路由仍装不上窗口过程**（P1-13 修完后剩下的部分） | 官方控件基类的构造函数里 `::SetWindowPointer(this.hwnd, GWL_WNDPROC, thunk)` 之后，控件窗口过程仍是系统类默认的地址（探针：`GetWindowPointer(btn.hwnd,-4)` 落在 user32 区间，而窗体的落在引擎 exe 区间） | 自绘组件**不依赖官方路由**：`nodio.ui.board` 用自己的窗口子类化（`nodio.ui.subclass`）接管 `WM_LBUTTONDOWN/UP/MOVE/DBLCLK` 并换算成客户区坐标 | 自绘工具箱点击分类可折叠 ✓；自绘属性格点「值」列可就地编辑 ✓（`edit-visible=true`） |
| P1-15 | **工具箱/属性格用原生 treeview/listview 根本跑不起来** | 引擎的原生控件表没有 treeview 的 `insertItem/setItemData/expand`，也没有 listview 的 `addItem/setItem` | 两个面板都改成**自绘**（`nodio.ui.board` + gdip）：工具箱按分类分组绘制（可折叠、可选中），属性格两列自绘 + 就地编辑（原生 `edit` 控件盖在单元格上，输入即时回写 `onChange`） | `toolbox-rows=14`、`pg-items=2`、`cat-collapsed=true`、`edit-visible=true` |

---

### 1.9 第四轮：官方源码级 `win.ui` 兼容路线 —— 右键链路打通

**用户诉求**：「右键」。**选定路线**：**不在 Nodio 里自造右键 API**，而是让官方 aardio 的
**源码级 `win.ui`** 在 Nodio VM 上真跑起来，右键事件由官方 `win.ui.tracker` 提供
（官方契约见 `D:\aardio\lib\win\ui\tracker.aardio:150-166`）。

#### 1.9.1 依赖扫描结论（实测，与预期相反）

先做依赖全集扫描，结论是**官方 win 源码层早已能在 Nodio 上跑**，缺口比预想小得多：

| 探测项 | 实测结果 |
|---|---|
| `import win` | ✅ 官方 `lib/win/_.aardio`（1135 行）整体加载；`win.loopMessage` / `win.resetMessage` / `win.hookMessage` 均为**官方源码函数**；`win.getMessagePos` 为官方 FFI 绑定 |
| `import win.ui` | ✅ 官方 `lib/win/ui/_.aardio`（2171 行）加载；`win.form` 被官方 `class win.form` **覆盖**掉引擎自带的最小实现；`win.ui.menu` 为官方 class |
| 官方 `class win.form` 实例化 | ✅ 真实窗口（hwnd 有效），`f.add{cls="button"}` 建出真实子控件 |
| `::User32.api("TrackPopupMenu", ...)` 动态绑定 | ✅ `win.ui.popmenu.TrackPopupMenu` / `CreatePopupMenu` 均为可调原生函数（官方 `lib/win/ui/menu.aardio:517-521`） |
| 官方 `util.metaProperty`（`lib/util/metaProperty.aardio`，纯源码） | ✅ 可加载；`@_metaProperty` 类注解、属性元表、原型层（`_metaPrototype`）、`_get`/`_set` 派发**机制本身全部正确** |
| 官方 `win.ui.tracker` 的右键事件 | ❌ **不触发**（唯一真实缺口） |

#### 1.9.2 根因：ctor 内 `this = obj` 重绑定后，成员赋值仍绕开元表 `_set`

官方 `win.ui.tracker` 的实现（`lib/win/ui/tracker.aardio:5-10`、`:23-173`）：

```aardio
class tracker{
	ctor( ctrl,hoverTime=... ){
		this = ctrl;                                   // ← 官方实例替换
		this.wndproc = { [0x7B/*_WM_CONTEXTMENU*/] = function(hwnd,message,wParam,lParam){ ... } ... }
	}
}
```

`this = ctrl` 之后，`this.wndproc = {...}` 必须作用在 **ctrl** 上，并经由
`win.ui.ctrl.metaProperty` 的 `wndproc` 属性 `_set`（`metaProperty/_.aardio:333-400`）
落到 `owner["_defaultProc"]` / `owner["wndproc__"]`，再由 `_wndproc_c` 跳板 +
`::SetWindowPointer(GWL_WNDPROC)` 装到窗口上 —— 右键事件才有路由。

**Nodio 修复前的行为**：`engine/runtime.cpp` 用 `bInstInit` 标记「ctor 帧里的
`this.x = v` 属于成员初始化，需绕过元表 `_set`」（官方 `gdip.family` 语义）。该判定的
比较对象是 **env 里当前的 `this`**；`this = ctrl` 重绑定后 env 的 `this` 已经是 ctrl，
与被写对象相同 → **误判为成员初始化** → 直接写实例表，**绕过 `_set`** →
tracker's 消息表从未装上 → 右键事件全哑。

实测证据（最小用例）：`this = ctrl` 后 `ctrl.MARK` 仍为 `null`（`_set` 未被调用），
而直接写 `ctrl.wndproc = {...}` 则 `MARK` 变为 `SET-CALLED`。

#### 1.9.3 修法：ctor 帧记住「正在构造的实例」，按实例身份判定

- `engine/common.h`：`Env` 新增 `Table* instTable`（**仅身份、不持所有权**，避免给 GC
  根集引入新的强引用环），记录该 ctor 帧正在构造的实例。
- `engine/runtime.cpp` `InstantiateClass`：`local->instTable = inst.get();`（ctor 结束时清空）。
- `engine/runtime.cpp` `SetMember`：`bInit` 判定由「env 的 `this` == 被写对象」改为
  「**正在构造的实例** == 被写对象」：`e->bInstInit && e->instTable && obj.tab.get() == e->instTable`。

**两条官方语义同时满足**（这是本次修复的边界条件，不是取舍）：

| 官方场景 | 写入对象 | 判定 | 结果 |
|---|---|---|---|
| `gdip.family`：ctor 里 `this.name = v`，元表把 `name` 声明为只读 `_get` | 正在构造的实例 | 成员初始化 | **仍绕过 `_set`**（不报「只读属性」） |
| `win.ui.tracker`：`this = ctrl` 后 `this.wndproc = {...}` | 另一个**已构造完成**的对象 | 普通赋值 | **走元表 `_set`** → 官方消息表装上 |

#### 1.9.4 实测结果（全部用官方契约，无自造 API）

新增永久回归用例 `tests/vmtests/t90.aardio`（+`t90.expect`）固化：

| 断言 | 实测输出 | 对应官方契约 |
|---|---|---|
| 控件层：tracker + `WM_CONTEXTMENU` → `onContextMenu(x,y)` | `CTRL-CTX 321,654` | `tracker.aardio:162-166`，x,y 为**屏幕坐标** |
| 控件层：右键按下/弹起 | `CTRL-RMBDOWN` / `CTRL-RMBUP` | `tracker.aardio:150-155` |
| 窗体层：tracker 装到窗体 | `FORM-CTX 321,654` / `FORM-RMBUP` | 同上，官方允许「控件或窗口对象」 |
| 官方 `win.ui.menu(f)` 建菜单 + 加项 | `MENU true true` | `lib/win/ui/menu.aardio`（菜单栏类） |
| 官方 `win.ui.popmenu(f)` 建**弹出菜单** + 加项 | `POPMENU true true` | `menu.aardio:457-521`（弹出菜单类，`popup`/`popId` 在此类） |
| 官方 `win.ui.popmenu.CreatePopupMenu()` | `POPMENU-API true` | `menu.aardio:517-521` |
| `this = obj` 重绑定后赋值落在新对象 | `REBIND true REBOUND` | 官方实例替换语义 |
| ctor 成员初始化仍绕过只读 `_set` | `CTORINIT true` | 官方 `gdip.family` 语义 |

**真实鼠标路径同样验证过**（探针，非回归夹具）：对控件/窗体投递 `WM_RBUTTONUP`，
系统按标准 Win32 行为自动补发 `WM_CONTEXTMENU`（`lParam = -1`），官方
`getMessagePos` 随即退回真实光标位置 —— 实测 `BTN-CTX x=1650 y=1042`，
说明**从系统右键到官方回调的整条链路**在 Nodio 上是通的。

**弹出菜单最后一环也实测过**（探针，非回归夹具，因为 `TrackPopupMenu` 是模态的）：
脚本建官方 `win.ui.popmenu(f)` → `add` 两个菜单项 → `pm.popup(300,300)`；
进程 4 秒后仍存活、stdout 只有 `BEFORE-POPUP` 而没有 `AFTER-POPUP`
→ **确实阻塞在 `TrackPopupMenu` 里，菜单真的弹到了屏幕上**（随后由外部 kill 关闭）。

> **操作提醒（sweep 工具链）**：`tests/module_sweep.ps1` 必须用
> `build-x64\Release\nodio.exe`（或**同目录**改名副本）跑。overlay 里的资源按
> `~` 相对路径找（`lib/zlib/_.aardio` 的 `~/lib/zlib/.res/zlibwapi.dll`），
> 把 exe 拷到别处会让 `~` 指向错误目录 → 连带 27 个 zlib 模块假失败。
> 同目录改名副本（`nodio-sweep.exe`）既保持 `~` 语义，又能避开并发会话重链
> `nodio.exe` 造成的 "不是内部或外部命令" 假失败。

### 1.11 第五轮（右键 · IDE 层接线）：组件层接线，并修掉 64 位 LRESULT 截断

第四轮打通的是「官方 `win.ui.tracker` 能派发右键事件」这一层。第五轮把它接到 IDE 的
三个真实落点（**编辑器 / 工程树 / 设计画布**），并在接线过程中挖出一个此前从未暴露的
**引擎级缺陷**：装上官方窗口过程之后，官方控件的**指针型返回值**只剩低 32 位。

#### 1.11.1 组件层（overlay `lib/nodio`）

| 文件 | 改动 | 官方契约依据 |
|---|---|---|
| `lib/nodio/ui/board.aardio` | 自绘板新增 `onContextMenu(x,y,self)`（**屏幕坐标**）、`onRightMouseDown/Up(x,y,self)`（客户区坐标）；子类化里处理 `0x7B`/`0x204`/`0x205` | `tracker.aardio:150-166` |
| `lib/nodio/ui/projectTree.aardio` | 挂官方 `win.ui.tracker(self.ctrl)` 并透传 `onContextMenu`；新增 `nodeAt(x,y)`（官方 `hitTest(x,y,true)`）、`selectedNode()`（官方 `getSelection()`） | `treeview.aardio:453, 517-527`；官方 `onRightClick` **只判句柄**（`:618-625`），`nodeAt` 照此实现 |
| `lib/nodio/designer/canvas.aardio` | 组件控件的子类化里 `0x7B` → `self.onContextMenu(item, x, y)` | `tracker.aardio:162-166` |
| `lib/nodio/ui/subclass.aardio` | 32 位下改用 `SetWindowLongW`（32 位 user32 **不导出** `SetWindowLongPtrW`，微软把它定义成 `SetWindowLong` 的宏） | Win32 API 事实 |

`0x7B` 的坐标语义统一照官方 tracker：先 `win.getMessagePos(lParam)` 取 lParam 里的屏幕坐标，
`lParam == -1`（菜单键 / Shift+F10 触发）时退回**真实光标位置**。

#### 1.11.2 IDE 侧接线

| 文件 | 改动 |
|---|---|
| `ide/nodio/app/editor.nodio` | 新增 `cut/copy/paste/selectAll`（官方 richedit 公开 API `getSel/selText/setSel/getLength` 组合 + 官方 `win.clip` 契约）；新增 `contextMenu(winform,ctrl)`：官方 `win.ui.tracker` + 官方 `win.ui.popmenu`，**菜单对象建一次复用**，命令 id 固定，按 `popId()` 返回值分发 |
| `ide/nodio/app/shell.nodio` | 接线 `app.editor.contextMenu(winform, winform.ed)`；工程树右键菜单（打开 / 复制路径，`nodeAt` 定位 + `popId` 分发）；原有编辑菜单改走 `app.editor.*`（官方 richedit 并没有 `copy/paste/selectAll`，原调用本就调不通） |
| `ide/nodio/app/designer.nodio` | 画布右键菜单（复制 / 删除 / 属性） |

菜单弹出统一用官方 `win.ui.popmenu` 的 `popId()`：**不带坐标调用**时官方取光标的屏幕坐标
（正是右键菜单的语义），`popId` 带 `TPM_RETURNCMD`，同步返回命令 id。

#### 1.11.3 本轮修掉的引擎缺陷：64 位 `LRESULT` 被截断

**现象**：官方 `treeview` 单独用时 `insertItem` 返回完整的 64 位 `HTREEITEM`；**一旦装上官方
`win.ui.tracker`（也就是装上官方 wndproc）**，返回值只剩低 32 位 → 父节点句柄损坏 →
**插入子节点直接失败** → IDE 工程树只剩根节点，右键菜单拿不到任何路径。

**逐层实测的定位链**：

| # | 探测 | 结果 |
|---|---|---|
| 1 | 官方 `treeview.insertItem({text=…}, null)`（未装 tracker） | 完整 64 位 ✅ |
| 2 | 同上，但先 `win.ui.tracker(tv)` | `-468224640`（低 32 位）❌ |
| 3 | 手工 `raw.tostdcall` 装 WNDPROC，函数返回 64 位常量 | 完整 64 位 ✅ |
| 4 | 官方 wndproc 的回落路径 `::CallWindowProc(prev,…)` | 只剩低 32 位 ❌ |

**根因**：官方 `lib/win/api.aardio:11` 把两个 API 声明成 **int 返回**（32 位进程的写法）：

```aardio
::CallWindowProc = ::User32.api("CallWindowProc","int(ptr lpPrevWndFunc,addr hwnd,INT Msg,ADDR wParam,addr lParam)" )
::DefWindowProc  = ::User32.api("DefWindowProc", "int(addr hwnd,INT msg,ADDR wParam,addr lParam) " );
```

而 Nodio 引擎的 `api()` 返回值解码是 `case ARInt: vRet = Value::Num_((double)(int)r);`
（`engine/builtins.cpp:5715-5717`）——**64 位 `LRESULT` 被截成 32 位**。官方 aardio 本体是
32 位进程（`LRESULT` 就是 32 位）所以无碍；Nodio x64 下，凡是经官方窗口过程回落的
**指针型返回值**全坏（`HTREEITEM`、`HMENU`、对话框句柄……）。

**修法**（`engine/builtins.cpp` 的 `api()`，沿用同文件既有的「官方 32 位假设修正」先例
——`FindFirstFile` 的 `ARInt → ARAddr`）：

```cpp
else if (strName.rfind(L"CallWindowProc", 0) == 0 ||
         strName.rfind(L"DefWindowProc", 0) == 0)
{
    if (sig.ret == ARInt) sig.ret = ARI64;   // x64: LRESULT 是指针宽的
}
```

**附带修**（`engine/builtins.cpp` 的 `win.getMessagePos`）：官方语义是**无参调用返回当前光标位置**
（官方 tracker 靠它在 `lParam == -1` 时回退），Nodio 之前无参返回 `(0,0)`。

#### 1.11.4 回归夹具

新增 `tests/ide_ctx/ctx.nodio` + `tests/ide_ctx/ctx.expect` + `tests/ide_ctx_smoke.ps1`
（x64 与 x86 双跑，`CTX-SMOKE OK`），18 行断言覆盖四层：

| 断言 | 实测 | 契约 |
|---|---|---|
| 编辑器选区 / 剪贴板 / 剪切 / 粘贴 / 全选 | `EDIT-SEL hello` … `EDIT-ALL XYZ world` | 官方 richedit 公开 API |
| 编辑器右键接线（tracker 装上后 `onContextMenu` 是函数） | `EDIT-CTXWIRE 1` | `tracker.aardio` |
| 自绘板右键：屏幕坐标 + 右键按下/弹起 | `BOARD-CTX 321,654` / `BOARD-RMBDOWN` / `BOARD-RMBUP` | `tracker.aardio:150-166` |
| **引擎级**：装上官方 wndproc 后父子节点句柄仍完整 | `TREE-INSERT 1`、`TREE-PATH D:\x\main.nodio` | 本轮修复点 |
| 工程树右键事件 / `nodeAt` 命中 / 未命中 / `selectedNode` | `TREE-CTX 321,654`、`TREE-HIT 1`、`TREE-MISS 0`、`TREE-SEL 0` | `treeview.aardio:453,517-527` |
| 设计画布右键（带组件名与屏幕坐标） | `CANVAS-CTX button1@321,654` | `tracker.aardio:162-166` |

同时把**引擎修复本身**也固化进永久 VM 回归 `tests/vmtests/t90.aardio`（不依赖 IDE 层夹具）：
给官方 `treeview` 装上 `win.ui.tracker`（= 官方窗口过程）后再 `insertItem` 父子两级，
断言 `TREE-INSERT 1` —— 这条在修复前必然是 `0`（父节点句柄被截断，插入直接失败）。

> `t90` 的窗体层断言同步改为 `FORM-CTX-OK`（只断言"被触发"）：窗体在 `WM_RBUTTONUP`
> 之后会收到系统补发的 `WM_CONTEXTMENU`（`lParam == -1`），官方 tracker 按官方契约
> 回退到**真实光标位置**，那条的坐标不是确定值。确定性的坐标断言改由
> `tests/ide_ctx/ctx.nodio` 承担（只投递 `0x7B` 一条）。

#### 1.11.5 端到端可见证据（官方右键菜单真的弹在屏幕上）

探针（`tests/ide_ctx/popup_probe.nodio`，跑完即删）：官方 `win.ui.tracker` 装到官方
`richedit` → 投递 `WM_CONTEXTMENU` → 官方 `win.ui.popmenu` 弹出菜单。

```
READY
CTX 100,200        ← 官方 tracker 派发的屏幕坐标
BEFORE-POPUP       ← 进 TrackPopupMenu
（进程 3 秒后仍存活 = 阻塞在模态弹出里）
```

桌面截图（仓库根目录 `.popmenu-shot2.png`）里菜单**真的渲染在窗口上**：
「复制 / ─── / 全选」三项，正是 `pm.add("复制")` + `pm.addSeparator()` + `pm.add("全选")`。

> 官方 `popId()` **不带坐标调用**时按官方语义取**光标**的屏幕坐标弹出
> （`lib/win/ui/menu.aardio:457-521`）——这正是右键菜单该有的行为；截图取证时
> 改用 `popup(x,y)` 是为了让菜单落在窗口内可见。

#### 1.11.6 本轮残留的已知缺口（非本轮引入，不阻断右键链路）

1. **官方 `treeview.getItemRect` 在 Nodio 返回 `null`**：`TVM_GETITEMRECT` 需要可写的输出
   缓冲区，Nodio 的 `struct &` 形参目前不回填。右键链路不受影响 —— `nodeAt` 走官方
   `hitTest`（与官方 `onRightClick` 一致）。
2. **十六进制字面量在 Nodio 只支持 32 位**：`0x1122334455667788` → `4294967295`；
   十进制字面量（`1234605616436508552`）正常。官方 aardio 同为 32 位进程，不构成与官方的
   差异，但写 64 位常量时要注意。
3. **两个夹具时序敏感**（`tests/vmtests/t35.aardio` 定时器计数、`t38.aardio` 公共对话框
   自动化）：t35 靠 `setInterval` 累加计数（套件负载高时可能少一拍 `end3`），
   t38 靠 `win.findWindow("#32770", 标题)` + `postMessage(0x111)` 关掉模态对话框
   （桌面负载高时会卡在 `win.dlg.open`）。两者**单跑都稳定通过**（t35 4/4、t38 5/5），
   `t01..t38` 连跑 38/38 通过，x64/x86 完整套件也曾各 88/88 全过，但会在不同轮次
   偶发一个。**与本轮改动无因果关系**（改动只涉及窗口过程返回值宽度与
   `win.getMessagePos`），判定为夹具自身的时序脆弱，后续可给它们加环境门控
   （与 t23 同类做法）。

#### 1.11.7 本轮全量回归（实测）

| 回归项 | 结果 |
|---|---|
| x64 VM 回归 | **88 passed / 0 failed / 0 skipped** |
| x86 VM 回归 | **88 passed / 0 failed / 0 skipped** |
| 发布回归 | **绿**（entry / lib / res / blob / memdll 五组） |
| 命名空间 sweep | **54/54** |
| IDE 冒烟 | **SMOKE OK** |
| IDE 右键回归（新增 `tests/ide_ctx_smoke.ps1`） | **CTX-SMOKE OK**（x64 + x86） |
| 969 模块 sweep | **953 通过 / 12 FAIL / 4 长时项** |

**12 项 FAIL 与第四轮残差完全一致**（32 位专用 DLL 8 项 + 32 位 DLL 路径 1 项 +
`=aardio` 字节码 2 项 + 官方缺 BouncyCastle 1 项），**无新增功能失败**。

**4 项「长时项」**（`fastcgi.iisInstall`、`java.jdk.v6ora`、`java.jdk.v8ora`、
`java.jre.v8ora`）是**下载/安装型**模块：10 秒上限内跑不完，45 秒上限下暴露为
`[GCFRAME] popping … but top is …` 的 GC 帧断言（长时运行中触发）。

> **与本轮改动无因果关系的对照证据**：本轮的引擎改动（`CallWindowProc` / `DefWindowProc`
> 返回宽度）**只在 x64 生效**。用**不含该改动的 x86 二进制**跑同样这 4 个模块，
> 结果完全相同（3 个 TIMEOUT + 1 个 GCFRAME）→ 失败来自「网络下载 + 长时运行下的
> GC 帧断言」，与本轮改动无关。相对第四轮的 957/969，本轮 953/969 的差异**全部集中
> 在这 4 项**上。

---

### 1.10 第五轮：IDE chrome 自绘化（两套控件方言的和解 + 设计器闭环）

本轮目标是把 IDE 的 chrome（页签 / 工具条 / 面板 / 画布）改成**自绘**，外观对齐官方 IDE 截图。
推进过程中撞到一整族**「官方 `win.ui` 控件方言」与「引擎原生控件方言」互不兼容**的缺陷——
它们才是「布局看起来乱七八糟」的真正根因，不是配色或绘制代码的问题。

#### 1.10.1 两套 `setPos` 语义（布局错乱的真正根因）

| 来源 | `setPos` 参数语义 | `text` 形态 |
|---|---|---|
| 官方 `win.ui.ctrl`（`D:\aardio\lib\win\ui\ctrl\metaProperty\_.aardio:512-529`） | `x, y, 宽, 高` | **属性**（`ctrl.text`） |
| 引擎原生控件表（`engine/builtins.cpp` ~8818） | `l, t, r, b` | **函数**（`ctrl.text()`） |

两套混用 → 同一个矩形被解释成两种意思，控件飞到屏幕外或叠在一起。**修法不是改引擎**
（引擎原生表要兼容既有用法），而是**在标准库加一层方言无关的封装**：

- 新增 `D:\Nodio\lib\nodio\ui\rect.aardio`（namespace `nodio.ui`）：
  `setRect(ctrl,l,t,r,b)`（走**原始 `win.setPos`**，物理像素，不缩放）、`moveTo`、
  `getCtrlText`/`setCtrlText`、`getCtrlProp`/`setCtrlProp`、
  `addCtrl(form,name,tParam)`、`mouseScreen(hwnd,lParam)`。
- 把 overlay / IDE 里所有 `ctrl.setPos(l,t,r,b)` 误用替换掉。

#### 1.10.2 官方 `form.add` 的两个隐藏机制（自绘组件会被"弹回原处"）

1. **`autoResize` 会按 `_defRect` 重排控件**：官方 `add` 默认把控件登记进 `_ctrlAutoResize`，
   窗体 show / DPI 初始化时按**设计时矩形**重新摆放 → 任何用 `win.setPos` 移动过的自绘组件
   会被**弹回**原位。**修法**：所有 `add` 一律传 `autoResize=false`（由 `nodio.ui.addCtrl` 统一注入）。
2. **创建矩形按窗体 DPI 比例缩放**：canvas 底板设计成 400×300，实际物理尺寸是 600×450；
   而 `win.setPos` 是**物理像素不缩放**。**修法**：IDE 组件层**统一用物理像素**。

#### 1.10.3 三条走不通的官方能力（各有实测证据，已绕开）

| 官方能力 | 实测现象 | 处置 |
|---|---|---|
| `ctrl.setPos(x,y,宽,高)` | 内部 `saveScale()+adjust()` 在 Nodio 里触发引擎断言崩溃（`STATUS_BREAKPOINT`） | **已解禁**（§1.39 复验）：最新引擎上全链双架构可用，崩溃系后续轮次顺带修复；`nodio.ui.setRect` 封装保留 |
| `plus` 控件做透明 overlay | `plus` 会把**父窗口背景整块 BitBlt 进自己的内存画板**再绘制（`lib/win/ui/ctrl/background.aardio:196-215`）→ 覆盖层必然盖住子控件 | 放弃透明 overlay；选中框/手柄改在**底板**上自绘 |
| `static` 控件画手柄 | `bgcolor` 依赖窗体 `WM_CTLCOLORSTATIC`，Nodio 里**画不出来**（手柄不可见） | 手柄一律在底板上用 gdip 自绘 |

#### 1.10.4 dock 去子窗体（页内容直接建在主窗体上）

官方插件面板是「子窗体 + `border="none"`」，但 Nodio 里：`win.form(mode="child")` **带标题栏**，
而 `border="none"` 会把 `WS_CHILD` **覆盖成 `WS_POPUP`**（变成弹出窗口，不是停靠面板）。
**修法**：`nodio.ui.dock` 重写为**无子窗体**——页签条与页内容都建在主窗体上，
`addTab(caption)` 返回页描述对象（`add` 转发到主窗体），`addTab(caption, cls, tParam)` 建控件，
`addTab(caption, 组件对象)` 关联自绘组件并调它的 `layout(cr)`。

配套修掉一个隐蔽 bug：**页内容可见性必须重算**（`__applyVisibility()`）——注册后新建的页
会盖住当前选中页（Z 序在后），表现为「首次不点页签就全空」。

#### 1.10.5 设计器闭环（8 手柄 / 属性回写 / DSG 往返）

| 能力 | 实现 | 冒烟断言 |
|---|---|---|
| 8 手柄缩放 | 选中框与 8 个手柄画在**底板**上；手柄一律放组件**外侧**、选中框**外扩 1px**（否则被组件盖住）；拖拽增量用**屏幕坐标**（`win.getRect(hwnd,true)` + lParam）避免控件跟随导致的漂移 | `GEO-*` |
| 属性格回写 | 编辑框用 `addCtrl`（z=40 压在自绘板上），事件同时挂 `onChange`/`onchange`；`canvas.applyProps` 真正写回控件属性（text/bool/number/通用）+ **几何属性**（left/top/width/height → 改组件矩形并重排） | `PROP-WRITEBACK:item=160 ctrl=160 prop=160`、`PROP-TEXT:回写文本` |
| DSG 往返 | 解析：按 `;` 切分并用**奇数引号合并**处理串内分号；引号串保持字符串、裸值转 number/boolean。生成：字符串加引号（`"` → `""`）、数字/布尔裸写；按 key 排序保证输出稳定 | `DSG-ROUNDTRIP:true items=1`、`DSG-TYPES:left=number text=string` |
| 工具条顺序 | 官方 `for(name in self.items)` 遍历哈希表**顺序不确定** → 按钮随机排列。`nodio.ui.toolbar` 增 `self.order` 数组按加入顺序排布 | 截图复核 |

#### 1.10.6 截图回归的一个坑（"底栏不见了"是假象）

`tests/ide_shot.ps1` 之前用**虚拟化的逻辑尺寸**建位图，而 `PrintWindow` 输出的是**物理像素**
→ 位图被裁掉右下角，表现为「底栏不见了」。**修法**：按 `GetDpiForWindow` 还原物理像素尺寸建位图，
并同时打印逻辑/物理尺寸与 DPI。脚本现会捕获 stdout/stderr 并回显 `DIAG*` 布局诊断。

> **本轮实测**：IDE 冒烟 `SMOKE OK`（exit 0，含新增的 PROP-*/DSG-*/TABSTRIP-*/VIS-* **硬断言**）；
> VM 回归 **88 passed / 0 failed / 0 skipped**（x64）；截图 `shot_demo.png` 1942×1287 物理像素，
> 显示完整 IDE（工具条顺序正确、三栏页签、自绘工具箱分类+14 控件、底栏问题列表、状态栏），
> 放大图可见**蓝色 8 手柄 + 选中框**，与官方设计器一致。

---

### 1.11 第六轮：外观基准换成现代深色 IDE + 主题体系

**用户改口径**：「IDE 应该抄这个」→ 指定 `https://aar.chengxu.online/thread-903.htm`
上那套**现代 aardio IDE**（aardioX / QuickEditor4，2026-10-19 发布）。
之前是照官方老 IDE（MFC 浅色界面）做外观，现在改为照这套深色 VS Code 风格做。
参考图已留档 `docs/ref/aar-ide/`（10 张），逐条抄下来的外观规格见
`docs/official-ide-reference.md` §0。

#### 1.11.1 新增标准库 `nodio.ui.theme`（主题层）

`lib/nodio/ui/theme.aardio`（namespace `nodio.ui`）：一套配色表 + 切换接口。

| API | 说明 |
|---|---|
| `nodio.ui.theme()` | 取当前主题（深色/浅色两套） |
| `nodio.ui.themeSet("light")` | 切换主题 |
| `nodio.ui.themeRef(argb)` | ARGB(0xAARRGGBB) → COLORREF(0x00BBGGRR)（原生控件要 COLORREF） |
| `nodio.ui.themeNative(ctrl, kind, bg, text)` | 给原生 common control 上深色 —— **当前禁用，见 §1.11.4** |

所有自绘组件的默认配色都从主题取（`tabstrip` / `toolbox` / `propertyGrid` /
`toolbar` / `statusbar` / `board`），换主题只改这一处。

#### 1.11.2 chrome 深色化（已落地）

| 组件 | 改动 |
|---|---|
| `nodio.ui.tabstrip` | 扁平化：**去掉底部分隔线**，选中项 = 稍亮底 + **顶部 2px 强调色描边**（原来是底部下划线） |
| `nodio.ui.toolbar` | **整条重写为自绘**（原来是原生 `button`，浅灰 3D 边框做不出扁平图标条）；保留 `order` 数组保证顺序；`buttons[name]` 换成带 `getRect()` 的轻量包装以兼容旧调用 |
| `nodio.ui.statusbar` | **整条重写为自绘**（原来用 `static` 分段，而 static 的 `bgcolor` 在本机画不出来，深色下会留一排白块） |
| `nodio.designer.toolbox` | 主题配色；分类折叠标记由 `▶/▼` 换成细箭头 `›/⌄` |
| `nodio.designer.propertyGrid` | 主题配色 |
| 窗体底色 | 官方 `win.form` 的底色是浅色系统背景 → 加一块 **z=0 的全客户区自绘底板**刷主题色 |
| 编辑区 / 输出 / 调试 / 终端 | 官方 richedit 支持 `setCharformat({textColor;backColor})`（`lib/win/ui/ctrl/richedit.aardio:186-249`）→ 文字色/文字背景色；**控件底色**用 `::SendMessage(hwnd, 0x443/*EM_SETBKGNDCOLOR*/, 0, COLORREF)`。注意：官方 `bgcolor` 属性走 `::SendMessageInt`，在本机**不生效**，必须用引擎 sysapi 表里的 `::SendMessage`（有显式签名，安全） |

#### 1.11.3 修：设计画布不随 DPI 缩放铺满编辑区

窗口显示后官方 DPI 缩放把客户区从 1281×821 改成 1921×1231（物理像素），
设计画布却还停在显示前的矩形 → 只盖住编辑区左上一小块。

**根因不是布局算错，是「钩子取不到」**：官方 `winform.onSize` 回调是在
**跨作用域**的上下文里被调用的，那条路径上 `app.shell` 的模块级钩子
（`onAfterLayout`）读出来是 `null`（实测 `type(onAfterLayout)` = `null`），
于是覆盖层永远收不到重排通知。**修法**：不依赖跨模块钩子，改由
`main.nodio` 的 300ms 心跳比较客户区尺寸，变了就整体重排
（`shell.layout()` + `designer.layout()`）。实测画布从 `240,56,981,575`
变成 `240,56,1621,985`（铺满编辑区）。

#### 1.11.4 坑：comctl32 的配色消息在本机直接崩进程

想把原生 treeview / listview 也染深色，按标准做法发
`TVM_SETTEXTCOLOR(0x1132)` / `LVM_SETTEXTCOLOR(0x1134)`（lParam = COLORREF）。
**实测崩溃**：`[FATAL CRASH] code=0xC0000005 … COMCTL32.dll`，
`AccessViolation: op=0 target=0000000000D4D4E4`（= COLORREF `0xD4D4D4` + `0x10`）
—— 本机的 comctl32 把 lParam 当**指针**解引用了。

顺带确认：**`::User32.SendMessageW` 不能用**（那是引擎的**动态绑定**路径，
签名靠猜，wParam/lParam 会被当指针传）；要用必须像 `nodio.ui.subclass` 那样
`raw.loadDll().api()` 写显式签名（`"int64(addr hwnd,int msg,addr wparam,addr lparam)"`，
FFI 本身实测可用）。

**结论**：`nodio.ui.themeNative()` 先置为**禁用**（直接 return false），
原生 treeview / listview 保持系统浅色；这两类面板要深色只能**改成自绘**
（下一步）。

#### 1.11.5 修：主窗体偶发「建出来但不显示」

实测 **约 1/4 概率** IDE 进程活着、消息循环在跑、但**屏幕上没有窗口**：
窗口建出来了（`AARDIO_FORM` 类、`AARDIO_FORM[TID:…]`），可 `WS_VISIBLE`
始终没被置上（style `0x4CF0000`，正常应为 `0x14CF0000`）。

- 现象是**官方 `win.ui` 的 `show()` 路径**偶发没走到 `DefWindowProc`
  （`WM_SHOWWINDOW` 没被默认处理 → `WS_VISIBLE` 不置位）。
- 独立探针（官方 `win.form` + richedit + tracker）**8/8 稳定**，所以不是官方库本身，
  是 IDE 这条链路上偶发。
- 修法（`main.nodio`）：`show()` 包 try/catch + 调用后用
  `::User32.IsWindowVisible` **自检一次**，不可见就退回最朴素的
  `ShowWindow(hwnd, SW_SHOW)` + `SetWindowPos(...SWP_SHOWWINDOW)` 兜底，
  并把状态打印出来（宁可吵，也不要静默地没有窗口）。
- 加固后连跑 **20 次 0 失败**（加固前 8 次里失败 2 次）。

> 这条同时解释了早期一次「截图脚本报 no main window」的失败。

#### 1.11.6 修：自绘层没有跟随系统 DPI 缩放（150% 下字体/图标比原生控件小一圈）

**现象**：系统 DPI 150%（`GetDpiForWindow` = 144）时，IDE 里自绘的
工具条图标、页签文字、工具箱缩略图、状态栏文字**明显比原生控件小**，
整窗看上去「缩放不对」。

**根因：三套单位混用**

| 路径 | 单位 | 证据 |
|---|---|---|
| 官方 `win.ui` 控件矩形 | **逻辑像素** | `form.add{left=240;top=58;right=980;bottom=560}` 实际落在 `359,86,1469,839`（= 逻辑 × 1.5）；官方 `lib/win/ui/ctrl/metaProperty/_.aardio:647` `reloadScale` 用 `dpiScaleX` 重排 |
| 我们的自绘组件 | **物理像素** | `win.setPos` 不做 DPI 缩放（官方 `ctrl.setPos` 当年触发引擎断言已禁用，§1.39 复验已解禁可用） |
| GDI+ 字体 | **像素**（`_GdipUnitPixel`，不随 DPI 变） | 官方 `lib/gdip/family.aardio:54` `createFont(...,unit=2)`；`lib/gdi/_.aardio:650` `getDpiScale` |

于是原生控件被官方放大 1.5 倍，自绘的却纹丝不动。

**修法**（新增标准库模块 `lib/nodio/ui/dpi.aardio`）

1. `nodio.ui.dpiSetScaleFrom(winform)` 取缩放系数（官方 `winform.dpiScaleX`
   在 `show()` 之前是 `null`，退回 `gdi.getDpiScale` —— 官方库同一个来源）；
2. 组件层统一口径：**绘制/摆放用物理像素**（GDI+ 的 `measureString` 只在
   像素单位下与字体自洽），**设计常量按逻辑像素写、用 `nodio.ui.px()` 换算**；
3. 字体按物理像素创建（`boardFont` 缓存键改成「字体名 + 物理字号」——
   原来只有一个全局字体，`fontSize` 选项形同虚设）；
4. 文字统一用 `bd.format`（垂直居中 + 超长省略号截断）；
5. **设计画布的组件矩形改成逻辑像素**（与官方 DSG 约定一致）：
   DSG 里写 `left=10;width=160`，运行期由官方窗体按 DPI 缩放，
   设计器所见即所得（改之前 150% 下设计器画 120px 宽、运行时却是 180px）；
6. `main.nodio` 的心跳额外盯着缩放系数，窗口拖到不同缩放的显示器时重排。

**顺带修掉的三个真 bug**

- `nodio.ui.dock.addTab` 对**已存在**的页签走早返回分支，把 `tParam.icon` 丢了
  → 右栏「语法/标准库/范例/工具/AI」的页签图标一直不显示（只有
  `setIcon(1,...)` 的那一个显示）。
- 状态栏分段用**序号**定位（`setText(2, …)`），工程路径被写进了「行,列」
  那一格（宽度只有 135px）→ 折成两行。改成分段**名称**定位。
- `nodio.ui.board` 的字体缓存只有一份，第一个画板创建后其它画板拿到的都是
  同一个字号。

**验证**：`tests/ide_smoke.ps1` 的 `PROP-WRITEBACK` 断言改成随缩放系数计算
（`ctrl = 160 × scale`，150% 下 = 240）；IDE 冒烟 **SMOKE OK**，
VM 回归 **89 passed / 0 failed / 0 skipped**；截图
`build-x64/Release/shot_dpi_final.png`（1942×1287 物理）复核自绘层与原生
控件同字号。

---

#### 1.11.7 第八轮：`nodio.gui` 控件体系（移植 Zan.Gui）+ 三个语义级 bug

**新增**：`lib/nodio/gui/` —— 移植 `D:\project\zan-lang\packages\Zan.Gui`
的 **Theme → Style → StyleBox → Widget** 架构（详见
`docs/ide-components.md` §0.8）：

```
token.aardio  theme.aardio  style.aardio  ctx.aardio  widget.aardio
button.aardio  check.aardio  label.aardio  input.aardio  panel.aardio  gallery.aardio
```

- `nodio.ui.theme` 改成**兼容层**：把 `nodio.gui.theme()` 的语义角色映射成
  旧字段名，旧组件一行不用改。
- IDE 右栏新增「组件」页（`nodio.gui.gallery`），把整套控件画出来当活文档。
- 左栏加宽到 272、右栏加宽到 352 逻辑像素，右栏页签去图标 ——
  否则 7 个页签放不下，后面的（工具/AI/组件）会被裁掉。

**本轮定位并修掉的三个真 bug**（都不是「引擎 bug」，是 aardio 语义踩坑）：

1. **`string.find` 是模式匹配（正则），不是字面查找**

   官方 `string.find(s, pattern)` 按**正则**语义工作
   （`engine/builtins.cpp:1608` → `RECompile`；`tests/vmtests/t71.aardio`
   用的是 `"b[a-z]r"` 这种正则）。拿它当「有没有这个字符」用会连环出错：

   | 调用 | 实际结果 | 想要的意思 |
   |---|---|---|
   | `string.find("myFunc", ".")` | `1`（`.` 是正则「任意字符」，永远命中） | 「不含句点」 |
   | `string.find("myFunc", "(")` | `1`（`(` 是不完整的分组） | 「不含左括号」 |
   | `string.find("myFunc", " ")` | `null` | 「不含空格」 |

   官方文档明说：字面值原样查找用 `string.indexOf`
   （`lib/builtin/string.aardio:358` `cmpMatch` 的注释也提到
   「可改用 string.indexOf 函数可禁用模式匹配查找子串」）。

   **影响**：IDE 左栏「大纲」的标识符校验 `!string.find(pre, ".")`
   永远为假 → **大纲列表恒空**。改用 `string.indexOf` 后正常。

2. **aardio 里空串是「真值」**

   `if(!s) continue` 拦不住 `""`（只有 null/false 为假）。
   大纲解析里 `name` 初始为 `""`，注释行匹配不上任何定义分支时 `name` 保持 `""`，
   却因为「空串为真」而通过了 `if(!name) continue` —— 结果**每一行注释
   都变成一个空节点**，大纲里塞满没有文字的条目。
   改用 `if(!name || #name == 0) continue` 后正常。

   > 注意：`!0` 在本引擎里是 `true`（0 也算假），但 `!""` 是 `false`。
   > 判定「有没有内容」一律用 `#s == 0` 或 `s === null`。

3. **`io.readFile(path)` 返回的是原始字节，中文全是乱码**

   官方 `builtin/io.aardio:162` 把 `readFile` 指向 `file.read`，
   读回来的是**每字节一个字符**的字节串（实测：8346 字节的文件读成 8346 字符）；
   官方 `io.writeFile` 却按 **UTF-8** 写 —— 读写不对称，中文源码打开就是乱码。

   正确姿势是 `string.load(path)`：引擎侧 `engine/builtins.cpp:2222`
   按 UTF-8 解码、认 BOM、二进制自动嗅探。

   **影响**：IDE 打开任何含中文的源码文件都整篇乱码（标准库页的中文说明同理）。
   已把 `main.nodio`（打开文件 / 演示文件）、`app/library.nodio`（lib.txt）、
   `app/project.nodio`（工程文件）三处 `io.readFile` 改成 `string.load`。

**顺带修掉**

- 自绘文字折行：GDI+ 默认会把长文本折成多行，固定行高的列表/树里上下串行
  （标准库页的「说明」列糊成一团）。`nodio.ui.boardFormat()` 与
  `nodio.gui.ctx` 的文字格式都加上 `_StringFormatFlagsNoWrap`（0x1000），
  超长文本按 `trimming` 截成一行 + 省略号。
- `nodio.ui.board` 增加 `onMouseLeave`：用 `TrackMouseEvent`（`raw.loadDll()
  .api("TrackMouseEvent","bool(struct &tme)")`）跟踪鼠标移出，
  否则鼠标离开画板后悬停态一直挂着（按钮一直高亮）。

**引擎侧改动**（`engine/runtime.cpp`）

`ForNum` 原来把循环变量的槽位指针**缓存**在循环外
（`Value* pLoopVar = &loopEnv->vars[loopVarName]`）。`vars` 是
`unordered_map`，循环体里的 `var` 声明会触发 rehash；虽然
`unordered_map` 的节点式实现不会移动已有节点（引用仍有效），
但依赖「rehash 不移动节点」这种实现细节本身就很脆。改成**每次按 key 重查**
（`findLoopVar()`），行为不变但不再依赖容器实现细节。

**验证**：VM 回归 **90 passed / 0 failed / 0 skipped**（新增 `t92`：
循环体 `var` / 嵌套循环 / `string.find` vs `string.indexOf`）；
IDE 冒烟 **SMOKE OK**；截图
`build-x64/Release/shot_gallery.png`（组件展厅）、
`shot_outline3.png`（大纲有文字了）、
`shot_lib3.png`（标准库中文说明正常）、
`shot_source.png`（编辑器里中文源码正常）。

#### 1.11.8 第九轮：自绘标题栏（去掉原生标题栏 + 菜单栏并入一行）

**用户诉求**：「这样 IDE 不适合原生窗口标题栏了哦」——
IDE 的 chrome 已经整体深色自绘，Windows 那条**浅色**原生标题栏
（以及它下面的系统菜单栏）跟整个界面根本不是一套东西。

**新增**：`lib/nodio/ui/titlebar.aardio` —— 自绘标题栏，
**菜单栏合并进标题栏一行**（左：应用标记 + 菜单；中：当前文档 - 应用名；
右：最小化/最大化/关闭），与参考 IDE（aar.chengxu.online/thread-903.htm 的
aardioX / QuickEditor4）一致。详见 `docs/ide-components.md` §0.9。

**做法要点**

| 环节 | 做法 |
|---|---|
| 去掉原生标题栏 | `win.modifyStyle(hwnd, 0xC00000/*_WS_CAPTION*/, 0, 0x20/*_SWP_FRAMECHANGED*/)`，保留 `WS_THICKFRAME`（仍可拖边改大小）。**必须在 show 之后**（官方 `win.form` 在 show 时才做 DPI 初始化并重排控件） |
| 满幅客户区 | 子类化 `WM_NCCALCSIZE`，把客户区扩到**整个窗口矩形**：Windows 10+ 的可缩放边框有 10 物理像素，默认由系统画刷 `COLOR_3DFACE`（浅灰）绘制，深色界面里一条浅灰边框比原生标题栏还扎眼。**最大化时不接管**（系统此时提议的矩形就是工作区，接管会把内容顶出屏幕） |
| 拖动窗口 | 标题栏整条返回 `HTCLIENT`，拖动**自己实现**（记录光标屏幕位置 + 窗口位置，按增量 `SetWindowPos`） |
| 菜单 | 全自绘：菜单标题画在标题栏上，下拉菜单是独立画板（打开时捕获鼠标，点外面 = 取消） |
| 窗口按钮 | `WM_SYSCOMMAND`：`SC_MINIMIZE` 0xF020 / `SC_MAXIMIZE` 0xF030 / `SC_CLOSE` 0xF060 |
| 居中标题 | 「当前文档 - 应用名」，居中于**菜单与窗口按钮之间**（参考 IDE 就是这么排的，不是整窗居中） |

**本轮定位到的真 bug：自绘控件的 z 序把鼠标事件全吃了**

官方 `win.form.add` 一次只加一个控件，控件之间的层次就等于**添加顺序**
（谁先加谁在上面）—— IDE 那个铺满客户区的**背景板是最早加的**，
于是它压在所有自绘板（页签条、工程树、列表、标题栏…）**上面**，
把所有鼠标事件都吃掉：点页签不切换、点树不选中、点标题栏没反应。

用 `ChildWindowFromPointEx` 定位（只看本窗体子窗口，不受桌面上别的窗口干扰）：

```
CHILD-HIT titlebar(750,30): hwnd=140380 class=AAPicturePlus2
  -> child index 0: 0 hwnd=140380 ... win=(631,270)-(1926,302) cli=1295x32
```

修法：背景板显式 `SetWindowPos(HWND_BOTTOM)`，标题栏与下拉菜单显式提到
`HWND_TOP`。这是一条**通用约定**（已写进 `docs/ide-components.md` §10 第 11 条）。

**顺带修掉 / 补上**

- `nodio.ui.board` 新增 `z` 选项（画板层级）与 `setCapture(on)`（显式捕获/释放
  鼠标）、`autoReleaseCapture` 选项、`dblClick` 标记。
- 标题栏的 `WM_NCHITTEST` 用 `win.toClient(hwnd, sx, sy)` 做屏幕→客户区换算
  （官方 `win.toClient` = `ScreenToClient`），不靠猜边框内缩。

**验证**：`tests/titlebar_probe.aardio`（进程内直接给画板发
`WM_LBUTTONDOWN/WM_MOUSEMOVE/WM_LBUTTONUP`，走同一条消息链）
+ `tests/ide_titlebar.ps1`：

```
DRAG-OK          窗口跟着光标精确移动 (+60,+50)
MENU-OPEN        点菜单标题弹出下拉菜单
MENU-FIRED       点菜单项触发命令并关闭菜单
PROBE-OK
TITLEBAR OK
```

> 为什么不用合成鼠标事件（`mouse_event` / `SendInput`）点真窗口：
> 本机桌面会话里合成事件**送不进目标进程**（点页签、点标题栏都收不到），
> 测出来的结果不可信。进程内发消息走的是同一条消息链，结果确定可复现。

VM 回归 **90 passed / 0 failed / 0 skipped**（未新增用例）、
IDE 冒烟 **SMOKE OK**；截图 `shot_tb_restored.png`（普通态）、
`shot_tb_max.png`（最大化态，布局铺满工作区、内容没有被顶出屏幕）。

---

## 1.18 项目级代码导航（转到定义 / 查找引用 / 跨文件重命名 / 符号搜索）

> 目标：把 IDE 的「代码掌控」从**当前文件**扩到**整个工程** ——
> F12 跳到别的文件里的定义、Shift+F12 列出全工程引用、F2 跨文件改名、
> Ctrl+Shift+T 按名字搜符号。

### 1.18.1 为什么索引建在 LSP 服务端

aardio 侧**无法**正确判断 aardio 源码里的注释/字符串边界：

- `//` 右邻 `=` 是 raw string（`//= ... =//`）；
- 块注释按**星号数量**配对（`/* ... **/ ... */`）；
- 反引号是原样串，双引号不转义。

在 IDE 侧重写一套词法扫描，必然误判 —— 注释里的 `function` 会被当成定义，
字符串里的引号会把行数算错。所以索引建在 `apps/nodio-lsp`：
**复用引擎真正的 Lexer/Parser**（`engine/lexer.cpp` + `engine/parser.h`），
IDE 侧只负责发请求和画界面。

### 1.18.2 引擎侧的前置改造：token 要有列号

原来的 `Tok` 只有 `line`，够诊断但**不够跳转** —— 一行里可能有多个同名标识符，
列号只能由 token 的**绝对偏移**推出。为此给 `Tok` 末尾加了 `pos` 字段
（`size_t`，wchar 单位 == UTF-16 码元，与 LSP `character` 同单位）：

```cpp
// engine/common.h
struct Tok {
    ...
    size_t pos = 0;   // token 首字符在源串中的绝对偏移
};
```

`engine/lexer.cpp` 的 `Lex()` 里每个 pushTok 站点都记下 `tokStart`。
字段放在**结构体末尾**，`pushTok({...})` 这种聚合初始化不受影响。

AST（`Node`）仍然只有行号 —— 定义点的列号靠**token 流**反查：

- 函数：名字 token 后面紧跟 `(`（函数声明）或 `=`（函数字面量赋值）；
- 其它：同一行上第一个同名的标识符 token。

### 1.18.3 服务端能力

| 方法 | 作用 |
|---|---|
| `nodio/setProject` | 收到工程根目录 + 源文件清单，建立**项目级符号索引**（工程文件 + overlay 标准库） |
| `textDocument/definition` | 转到定义（跨文件） |
| `textDocument/references` | 查找引用（跨文件） |
| `textDocument/rename` | 跨文件重命名（返回 WorkspaceEdit） |
| `workspace/symbol` | 项目符号搜索 |

**定义的选择顺序**（和人的直觉一致）：

1. 同文件 —— 光标**上方**最近的一个（上方没有就取下方第一个）；
2. 本文件 `import` 到的模块（工程内模块或标准库）；
3. 任意同名定义。

**引用要认得出「同一个符号」**：名字相同不等于同一个符号，
`obj.foo` 和 `bar.foo` 是两个 `foo`。判定规则：

- 独立标识符（前面不是 `.`）—— 直接算引用；
- 成员访问 `a.b.c` —— 限定链必须正好指向**定义所在的模块**，
  或者是定义所在**命名空间路径的前缀**。

这样 `app.editor.contextMenu` 会被算成 `editor.nodio` 里 `contextMenu` 的引用，
而某个对象上的 `.contextMenu` 不会被误算。

### 1.18.4 IDE 侧接线

- **多文档**：`lspClient` 之前只认一个 uri，切标签页后补全/诊断都落在旧文件上。
  现在按 uri 分别记 `didOpen`/`didChange`，`openDoc(path,text)` 切换当前文档。
- **按键不能同步问 LSP**：`lspClient` 的请求是同步阻塞的（最多 1 秒），
  在窗口过程里做会卡住整个窗口过程。编辑器组件用 `winform.setTimeout`
  把 F12 / Shift+F12 / F2 / Ctrl+点击**推迟**出去（和代码补全一个套路）。
- **落点精度**：LSP 的列是 **0 基 UTF-16 码元**，编辑器内部是 1 基，
  中间还差一次「UTF-16 码元 → UTF-8 字节」的换算
  （aardio 字符串是 UTF-8 字节）。编辑器组件提供 `replaceTextAt()` 做这件事。
- **引用面板**：底部 dock 新增「引用」页（`nodio.ui.list`），
  双击跳到那一处；项目符号搜索的结果也复用这个面板。

### 1.18.5 踩到的坑

1. **`string.left(s,n)` 是「前 n 个字符」，不是「去掉前 n 个字符」**。
   去掉 URI 的 `file:///` 前缀必须用 `string.slice(s,9)`（`slice` 就是 `sub`，
   第三参省略时到串尾）。用 `left(s,#s-8)` 会把**尾巴**切掉。
2. **`string.replace` 返回两个值**（结果 + 替换次数），
   直接 `return string.replace(...)` 会把次数一起漏出去。
3. **双引号是原样串**：`"\\a.nodio"` 里是**两个**反斜杠。
   路径要写 `"D:\dir\file.nodio"`（双引号、一个）或 `'D:\\dir\\file.nodio'`（单引号、两个）。
   写混了同一个文件会有两个索引键 —— 查找引用重复计数、重命名把同一处改两遍。
   服务端因此加了 `NormalizePath()` 折叠重复分隔符。
4. **工程根可能自带尾部分隔符**（IDE 的 `proj.root` 就是 `D:\...\`），
   服务端再拼一个 `\` 会变成双反斜杠 —— `ModuleKeyOf` 算出的模块名会多一个点
   （`.app.editor`），import 匹配全废。

### 1.18.6 实测

- `tests/lsp_project_smoke.py`：真实工程（`ide/nodio`）上
  `contextMenu` 的定义跨文件解析到 `app/editor.nodio`、
  引用跨两个文件、重命名产出跨文件编辑、符号搜索有结果 —— **LSP-PROJECT-SMOKE-OK**；
- `tests/ide_nav_smoke.ps1`：无头跑完整 IDE 链路 ——
  打开 `app/shell.nodio` → F12 → 打开 `app/editor.nodio` 且光标落到第 44 行；
  临时工程里跨文件改名（一个开着的文件 + 一个没开着的文件）两处都改对、
  旧标识符消失 —— **NAV SMOKE OK**。

### 1.18.7 顺带修掉的测试探针 bug

`tests/editor_comp_probe.aardio` 拼键盘状态数组时多写了一个字节：

```aardio
kb = ..string.sub(kb, 1, 0x11) ++ '\x80' ++ ..string.sub(kb, 0x12, 256);
```

17 + 1 + 239 = **257 字节**，塞进 256 字节的 `raw.buffer` → **堆溢出**。
x64 上侥幸没炸，x86 上 5 次里崩 4 次（退出码 `0xC0000374` = STATUS_HEAP_CORRUPTION）。
改成 `sub(kb,1,0x10)`（16+1+239 = 256）后，x64 / x86 都 5/5 通过。

> **教训**：随机出现的堆损坏，第一反应应该是**测试代码本身**越界写，
> 而不是怀疑引擎 —— 先算清楚「拼出来的串到底多长」。

---

## 1.19 IDE 自己的工程文件报「假语法错误」（JSON 转义解码漏了 `\r`）

上一轮把项目级导航做完之后，发现 IDE 打开自己的 `app/shell.nodio` 时，
「问题」列表里挂着一条**根本不存在的语法错误**：

```
语法错误: 期望 ) (from NodioVM.cpp:2859, context: [1280] [;] [bottom] [=] [820] [)]
    [r] [] [winform] [.] [add] [(] [r] [] >>> [ed]<<< [=] [{] [cls] [=] [richedit]) @ 行 65
```

### 1.19.1 定位过程

1. 错误上下文里每个 `)` 后面都多了一个 **`[r]`** —— 而源码那一行**根本没有 `r`**。
   上下文打印的是 token 原文，所以那是一个**字面量 "r" 的 token**。
2. 对比同一文件的 LF / CRLF 两种形态：**LF 干净，CRLF 报错**（`tests/dsg_parse_probe.py`）。
3. IDE 的编辑器缓冲是 **CRLF**（richedit 只认 CRLF），磁盘文件是 LF ——
   于是「IDE 里报错、磁盘上不报错」。

### 1.19.2 根因：LSP 服务端的 JSON 解码器

问题在传输层，不在词法器。`apps/jsonrpc_stdio.h` 的字符串解码只认两种转义：

```cpp
out.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);   // 旧代码
```

其余转义一律**退化成字面字符**。而 aardio 的 `JSON.stringify` 完全按 JSON 规范转义：

| 输入 | `JSON.stringify` 输出 | 旧解码器得到 | 后果 |
|---|---|---|---|
| `CR` | `\r` | **`r`** | 每个 CRLF 变成一个 `r` 标识符 → 假语法错误 |
| `TAB` | `\t` | TAB | 正常 |
| `\` | `\\` | `\` | 正常 |
| `"` | `\"` | `"` | 正常 |
| `中文` | 原样 UTF-8 | 原样 UTF-8 | 正常（aardio 不转义非 ASCII） |
| `\uXXXX` | — | **`uXXXX`** | 非 ASCII 客户端全废 |

`\r` 这条最阴险：它**不报错**，只是把换行悄悄换成了一个字母。
`\r\n` → `r\n` → 词法器把 `r` 当成标识符 → 后面期待 `)` 的地方当然对不上。

### 1.19.3 修法

`apps/jsonrpc_stdio.h` 增加 `JsonUnescape()`，按 JSON 规范完整解码：

- 简单转义：`\n \t \r \b \f \" \\ \/`；
- `\uXXXX`：转成 **UTF-8 字节**（服务端内部一律 UTF-8 字节流）；
- **代理对** `\uD83D\uDE00` → 一个码点 → 4 字节 UTF-8；
- 非法转义保持原样（不吞字符）。

`JsonString()` 与 `JsonStringArray()` 都改走它。

### 1.19.4 为什么不能靠「客户端统一发 LF」绕过去

IDE 缓冲是 CRLF（richedit 的硬要求），要发 LF 就得在客户端另存一份 LF 副本，
于是**编辑器里的文本、服务端索引的文本、诊断里的行列**要各维护一份，
换行一多就必然对不上。正确做法是**传输层老实解码**，
让服务端看到的文本和 IDE 看到的一模一样。

### 1.19.5 实测

- `tests/dsg_parse_probe.py`：全部用例**同时跑 LF 与 CRLF 两个变体**，
  外加真实 `app/shell.nodio` 的 LF / CRLF 两份 —— **DSG-PARSE-PROBE-OK**；
- `tests/ide_nav_smoke.ps1`：新增断言 `NAV-DIAGS:0` ——
  **IDE 对自己的（合法）源码必须零诊断** —— **NAV SMOKE OK**；
- x64 / x86 的 `nodio-lsp.exe` 都已重建并复测。

> **教训**：传输层的「尽力而为」解码器最危险 —— 它不会失败，
> 只会把对的东西悄悄变成错的东西。凡是要还原**源码**，
> 转义就必须**完整且精确**。

---

## 1.20 导航栏（面包屑）+ 一个整片死区

本轮做 VS 编辑器体感时，先做了一个 **VS 的导航栏**（编辑区正上方那条
「工程 › 目录 › 文件 › 命名空间 › 函数」，点哪段跳哪段），
结果在接线阶段挖出一个**从没被发现过的整片死区**。

### 1.20.1 导航栏：层级链从哪来

面包屑的符号段要的是**层级**（文件 → 命名空间 → 函数），不是平铺列表。
数据源是 LSP 的 `textDocument/documentSymbol`。

**服务端改造**（`apps/nodio-lsp/main.cpp`）：

| 改动 | 为什么 |
|---|---|
| `DocSymbol` 增加 `endLine` | AST 只记节点的**起始行**。判「光标在不在这个符号里」需要符号的**范围** —— 只看起始行的话，光标停在一个**刚结束的函数之后**的顶层语句上，会被算成「还在那个函数里」（实测：光标在 195 行的顶层语句，链里却冒出 114 行结束的 `darkenEdit`） |
| `MaxLineOf(PNode)` 递归求子树最大行号 | 符号的结束行 = 它整棵子树里最大的行号 |
| `var f = function(){}` 识别为 **function** 而不是 variable | aardio 里绝大多数函数是 `var 名 = function(){}`（`VarDecl`），不特判的话它们全是「变量」，而变量**不是作用域**，不进面包屑 |
| JSON 增加 `"line"` / `"endLine"` 字段 | 老代码读 `v.location`（服务端根本没这个字段）→ 行号恒为 0 |

**客户端改造**（`ide/nodio/app/lspClient.nodio`）：`symbols()` 之前把 `children`
丢掉了、位置也读错字段。现在返回**层级树**
`[{name; kind; line; endLine; col; children}]`。

**IDE 侧**（`ide/nodio/main.nodio`）：

- 符号树**只在文档变化时**向 LSP 拉一次（`symTreeDirty` 标记 + 心跳里拉）；
  光标移动只做**纯内存遍历** —— 每动一下都同步问一次 LSP 会把编辑器卡死；
- 面包屑 = `工程名 › 目录段… › 文件名 › 符号链`；
- 点符号段 → `gotoLineCol` 跳过去。

### 1.20.2 挖出来的死区：模块钩子被局部变量遮蔽

导航栏要「光标移动 → 刷新面包屑」，这条链路要经过
`shell.editor.onCursor` → `shell` 的钩子 → `main.nodio` 的实现。
**结果钩子根本没被调用**（加打印才发现 `onCursorMove` 读到的是 `null`）。

顺着查下去，发现**全部 24 个模块级钩子都是死的**：

```
main 侧：HOOKS: openFile=function docChange=function cursorMove=function ...
shell 侧（模块内部）：hook=null openfile=null docchange=null
```

### 1.20.3 根因：引擎的「模块局部变量遮蔽模块表成员」

Nodio 的模块加载（`engine/import.cpp`）里，模块体跑在自己的帧
（`modEnv`）上；`var` 声明会**同时**写进 `modEnv->vars`（局部）
和模块表 `modTable->map`（成员）。而**裸赋值**（`onX = null`）
按 `runtime.cpp` 的规则**只**写局部（模块边界不外传）。

模块内部读裸名字时，`Env::Find` 沿着作用域链先查 `vars` ——
**局部副本把模块表的成员遮住了**。

于是：

```
app.shell.onOpenFile = openFile;   // 写的是模块表 map
// 模块内部读 onOpenFile → 命中局部副本（初值 null）→ 永远是 null
```

**后果**：工具条（新建/打开/保存/运行/调试/设计器）、菜单（文件/编辑/转到/查找/运行/视图）、
快捷键（Ctrl+N/O/S/W/F/H/G/Shift+F、F5/F10、Ctrl+F9）、工程树双击打开文件、
文档变化刷新大纲 —— **全部是死的**（代码写得很完整，但从来没被执行过）。

### 1.20.4 修法：钩子放进表里

模块级的裸变量有遮蔽问题，但**表**没有：表是同一个对象，
谁改都看得见。于是把 24 个钩子收进一张 `hooks` 表：

```aardio
// app/shell.nodio
var hooks = {
	onOpenProject = null;
	onOpenFile    = null;
	...
};
// 用法：if(hooks.onSave) hooks.onSave()
// main.nodio 侧：shell.hooks.onSave = saveFile;
```

`var hooks = {...}` 是 `VarDecl` → 同时进 `modEnv->vars` 和 `modTable->map`，
但两边指向**同一个表对象** → `shell.hooks.onSave = f` 改的就是模块内部
那张表 → 读得到。

顺带把遗留的 `onAfterLayout` 接上（`designer.layout()`），
设计画布在布局后立刻跟随编辑区，不用等心跳兜底。

### 1.20.5 aardio 的两个字符串坑（顺手记下）

1. **`s[i]` 取出来是「字节值」（数字），不是 1 字符的串**。
   所以 `t[#t] == '\\'` 永远不成立（数字 vs 字符串）——
   去掉路径尾部分隔符的循环**一次都没跑过**，导致工程名取不到
   （`io.splitpath("D:\\a\\b\\")` 的 name 是空）。
   必须跟字节值比：`'\' = 92`、`'/' = 47`。
2. **空串是真值**（aardio 只有 `null`/`false` 为假）。
   判空要用 `#s == 0`，写 `if(s)` 会永远进去。

### 1.20.6 实测

- `tests/ide_nav_smoke.ps1` 断言：
  `NAV-SYMTREE` 非空、`NAV-CHAIN:nodio|app|shell.nodio|`、
  `NAV-FNCHAIN:…|darkenRichEdit|`、点符号段跳到第 114 行、
  以及**钩子链路端到端**（`NAV-COMPCHAIN` 里出现函数名）—— **NAV SMOKE OK**；
- 诊断模式下 `DIAG hooks: all 24 ok`（钩子自检）；
- 截图 `ide_navbar.png`：导航栏 `nodio › app › shell.nodio › darkenRichEdit`，
  当前位置用强调色；`ide_designer.png`：设计画布照常覆盖编辑区。

---

## 1.22 属性格分组带（VS WinForms 属性窗口的「分类」）

### 1.22.1 目标

官方 IDE 的属性格是**带分组带**的：属性按分类分成若干条，每条带一个折叠箭头，
点箭头折叠/展开；没有属性的分类不画带（见 `docs/official-ide-reference.md` §设计器）。
原先 Nodio 的属性格是**平铺两列**（属性 / 值），一个控件的十几个属性全挤在一起，
既不像 VS，也没法快速定位。

分类顺序按 VS 的 `Appearance / Behavior / Data / Layout / Misc` 映射成
**外观 / 样式 / 数据 / 位置 / 基本**。

### 1.22.2 关键设计：显示行 ≠ 属性序号

分组带插在属性行之间，于是同一个「行」有两个编号：

| 编号 | 含义 | 谁在用 |
|---|---|---|
| **属性序号** | 属性在 `items` 里的下标（1 基） | `onChange(index, value)` 的 `index`、`self.items[index]` |
| **显示行号** | 分组带 + 属性行排在一起后的行号（1 基，0 = 表头） | 点击命中、就地编辑框的 y 坐标 |

两处必须各用各的：

* `onChange` 回调给宿主的是**属性序号** —— 宿主拿它去 `item.props[index]` 改值；
* 就地编辑框（临时 `edit` 控件）要落在**显示行**上 —— 用属性序号算 y 会落到
  别的行上（分类带的行数被算漏了）。

属性格内部因此多了一张 `self.rows` 显示行表：

```aardio
{type="cat";  cat="位置"}          // 分组带
{type="item"; cat="位置"; index=2} // 属性行（index = 属性序号）
```

### 1.22.3 分类从哪来

* `nodio.designer.registry.categoryOf(key)` —— 按**属性键**查表：
  `text/align/color/bgcolor/font/image` → 外观，`left/top/width/height/right/bottom` → 位置，
  `checked/password/readonly/enabled/visible/hasLines` → 样式，
  `items/columns/tabs/value` → 数据，其余 → 基本。
* 属性项自带 `cat` 字段时以它为准 —— **自定义组件可以自己分组**。
* 组件注册表的属性定义从**三元组**扩成**四元组** `{key; 标签; 类型; 分类}`，
  第 4 个元素就是分类名；省略时回落到 `categoryOf(key)`。

分类顺序：规范表（外观/样式/数据/位置/基本）优先，其余按**首次出现**顺序接在后面
（自定义组件自带的分类名不该被塞掉）。

### 1.22.4 坑

1. **自绘板刷新必须用 `redraw()` 而不是 `invalidate()`** ——
   `invalidate()` 只置脏位，更新区域可能残缺，GDI+ 会把自绘内容裁掉一块
   （见 `lib/nodio/ui/board.aardio` 的 `redraw`）。
2. **折叠箭头不能用字体字形**（`▶`/`▼` 在等宽/西文字体里可能缺字形），
   用矩形自己拼三角形。
3. **分组带是通栏的**：带的底色会盖住画在它下面的**列分隔线**，
   所以分隔线要**先画**，带**后画**（或者干脆让带盖住 —— VS 就是通栏的）。

### 1.22.5 实测

* 探针 `tests/dsg_propgrid_probe.aardio`：分类映射、显示行表结构、
  折叠/展开后的行数、就地编辑框的 y 是否落在**显示行**上、点带是否折叠
  —— **PROPGRID-OK**；
* 截图 `ide_propgrid2.png`：左栏「属性」页显示 `▼ 外观 / 标题` 与
  `▼ 位置 / 左边·上边·宽度·高度`；
* `tests/ide_smoke.ps1`（SMOKE OK）、`tests/ide_nav_smoke.ps1`（NAV SMOKE OK）、
  VM / editor / input / lsp / dsg 全量回归（x64 + x86）全绿。

---

## 1.23 设计画布上的控件「看不见」（底板直绘盖掉了子控件的像素）

### 1.23.1 现象

设计器打开后，画布上只有一片白色网格和蓝色的选中框，
**组件本身完全看不见** —— 连属性格里都显示着它的「标题 / 左边 / 上边 / 宽度 / 高度」，
说明组件**确实建出来了**（`hwnd` 有值、`visible=true`、矩形也对）。

### 1.23.2 定位

三步排除：

1. **组件存在且位置正确**：`DIAG canvas-item hwnd=… rect=468,189,648,234 text=按钮 visible=true`，
   矩形和画布选中框的位置完全吻合。
2. **z 序是对的**：用 `GetWindow(hwnd, _GW_HWNDPREV=3)` 从组件往上爬，
   **一个窗口都没有** —— 组件在最上层；反过来往下爬（`_GW_HWNDNEXT=2`）35 层才碰到画布底板。
   ⇒ 不是「被压在底板下面」。
3. **藏掉底板组件就出现**：把画布底板 `show(false)` 之后，组件立刻正常显示。

第 2、3 两条放在一起，结论只有一个：**底板的重绘把组件区域的像素盖掉了，
而组件没有跟着重绘**。

### 1.23.3 根因

`nodio.ui.board` 是官方 `plus` 自绘板，它的内容是**直绘到窗体背景上**的
（`plus.directDrawBackgroundOnly()`：禁止控件在自己窗口里画，改由父窗口画）。
绘制顺序是「窗体背景 → 子控件」，但**子控件只有在自己失效时才会重绘**：

* 底板重绘 → 窗体背景重画 → 组件区域的像素被底板内容覆盖；
* 组件本身没失效 → 不会重绘 → 像素停在被覆盖的状态。

于是画布看起来「只有网格」。底板每重绘一次（拖动、选中、改属性）就再盖一次。

### 1.23.4 修法

`nodio.designer.canvas.__refresh()`：底板刷完，把**每个组件的区域再刷一遍**。

```aardio
self.__refresh = function(){
    self.bg.redraw();                       // 底板整块重绘
    for(i=1;#self.items;1){
        var it = self.items[i];
        if(!it || !it.ctrl || !it.ctrl.hwnd) continue;
        try{
            it.ctrl.invalidate();           // 让组件自己重绘一次
            ::UpdateWindow(it.ctrl.hwnd);
        }
        catch(e){}
    }
}
```

顺带两处：

* 摆放组件时用 `..win.setPos(..., 0/*_HWND_TOP*/, 0x10/*_SWP_NOACTIVATE*/)`
  把它提到画布底板之上（官方 `win.ui` 的 `add` 只在**同一次调用内部**按 `z` 排序，
  跨调用只有创建顺序，层次容易反）；
* 设计器打开时**藏掉行号栏**（`shell.editor.gutter.show(false)`）：
  设计面是一块「表单表面」，留着代码区的行号栏既没意义，又会压住画布左边。

### 1.23.5 坑

1. **官方 `win.ui` 的 `add` 里 `z` 几乎没用**：它只在**一次 `add()` 调用内部**用来排序
   （`opt.z===null → opt.z = #zorder + 1`），跨调用完全按创建顺序。
   想确定层次只能显式 `..win.setPos(..., _HWND_TOP, ...)`。
2. **`GetWindow` 的常量别搞反**：`2 = _GW_HWNDNEXT`（往下找）、
   `3 = _GW_HWNDPREV`（往上找）。第一次量的时候用了 2，结论正好反了。
3. **自绘板直绘到父窗口时，父窗口的绘制顺序在子控件「之前」**，
   所以自绘板一重绘就会盖掉子控件的像素 —— 这类板子每次重绘后
   都要把上面的动态控件再刷一遍。

### 1.23.6 实测

* 截图 `ide_propgrid10.png`：设计画布是白色表单面 + 网格，
  组件「按钮」正常显示并带选中框 + 8 个手柄；
  左栏「属性」页是分组带（▼ 外观 / 标题=按钮；▼ 位置 / 左边·上边·宽度·高度）。
* `tests/ide_smoke.ps1`（SMOKE OK，含属性格探针）、`tests/ide_nav_smoke.ps1`（NAV SMOKE OK）、
  VM 90/90、editor / input / lsp / dsg 回归全绿。

---

## 1.24 交互链路大修：菜单点不动 / 组件乱飞 / 反馈被抹掉 / 调试跑不起来

这一轮集中处理「绝大多数地方无法点击操作」和「卡顿、组件乱飞、事件区与显示区不一致」。
四个根因分属四个层面，但互相掩盖，必须一起看。

### 1.24.1 根因 A：浮层自绘板要么看不见、要么点不到

**现象**：标题栏「文件」能点（有高亮），但下拉菜单**不出现**；
而菜单一旦出现，点菜单项又**没反应**（菜单直接关掉）。

**定位**（`tests/ide_click.ps1` 把点击变成可回归的东西）：
脚本用 `ChildWindowFromPointEx` + `SendMessage(WM_LBUTTONDOWN/UP)`
**直接向命中的子窗口发窗口消息**——
桌面锁屏时合成鼠标事件送不到 IDE（锁屏窗口在 z 序最顶），
只有走消息注入才测得准。

实测三组数据放在一起才看得出来：

| 现象 | 数据 |
|---|---|
| 菜单板在、可见、还在 z 序最顶 | `ZChain` 往上爬**一个窗口都没有** |
| 菜单板确实在绘制 | `DIAG pop.draw rc=0,0,204,330 items=11 clip=0,0,204,330` |
| 但屏幕上就是没有 | 整窗 `PrintWindow` 里那块是工具条和工程树 |

**根因**：`nodio.ui.board` 基于官方 `plus` 自绘板。官方 plus 的绘制顺序规则是

* `WS_EX_TRANSPARENT` 的子窗口**最后绘制**（永远压在兄弟窗口之上）；
* 普通子窗口**按 z 序**绘制 —— z 序靠下且没有 TRANSPARENT 的，
  会被后面创建的兄弟窗口整片盖掉。

下拉菜单画板建得早（z 序靠下），于是：加 `transparent=true` → 看得见；
不加 → 被盖掉。**而 `transparent=true` 附带 `WS_EX_TRANSPARENT`，命中测试会跳过它** ——
好在菜单打开时已经 `SetCapture`，鼠标事件直接进捕获窗口，绕开了命中测试。

**修法**：`transparent = true` + `SetCapture`（`lib/nodio/ui/titlebar.aardio`）。
画板提前建好、提前隐藏、尺寸取 1×1（不挡任何东西），打开菜单时再挪位置 + 显示 + 重画。

### 1.24.2 根因 B：菜单项的命中矩形用错了坐标空间

**现象**：菜单能弹出，菜单项画得也对，但点菜单项只是把菜单关掉。

**根因**：`self.popRects` 存的是**窗体坐标**（`px2`/`py` 绝对位置），
而鼠标事件（`onMouseDown`/`onMouseUp`）给的是**画板客户区坐标**。
`__hitPop` 拿两套坐标系比对，**永远命中不了**。

**修法**：菜单项矩形一律存**画板本地坐标**（原点在菜单画板左上角）。
绘制和命中测试用同一套坐标（绘制原点本来就是画板客户区原点）。

### 1.24.3 根因 C：`board.invalidate()` 只置脏位 → 自绘内容被裁掉

**现象**：标题栏的 logo 和「文件/工程」两个菜单**忽然整块消失**，
右边「编辑/选择/格式…」却好好的 —— 像「控件没画出来」。

**根因**：官方 plus 的 `WM_PAINT` 是这样画的（`win/ui/ctrl/plus.aardio:836`）：

1. 把窗口当前画面 `BitBlt` 到内存 DC；
2. `graphics.setClipRect(rcPaint, Replace)` —— **裁剪区 = 本次失效区域**；
3. 画一遍；
4. `BitBlt` 回窗口。

于是「只失效了一小块」时，裁剪区就只有那一小块，其余部分保留旧像素。
标题栏那次就是裁剪区没盖住左上角 ⇒ logo 和前两个菜单保持旧像素 ⇒ 看起来像被擦掉了。

**修法**：`nodio.ui.board.invalidate()` 一律按**整窗**失效
（`::InvalidateRect(hwnd, 0, 1)`），保证自绘内容每次都完整重画。
`redraw()` = 整窗失效 + `UpdateWindow` 立刻重画（用在需要同步刷新的场合）。

顺带修掉一个潜伏 bug：扁平写法（`left/top/right/bottom`）建板时
`self.rect` 没落地，`layout()` 无参调用会直接返回，矩形永远停在创建时的位置。

### 1.24.4 根因 D：官方 richedit 没关自适应 → 组件乱飞 + 事件区与显示区错位

**现象**：窗体尺寸一变（最大化 / 去掉原生标题栏 / DPI 变化），
**编辑器被弹回设计时的位置**，而行号栏、导航栏这些自绘板是我们自己摆的（物理像素）。
两边对不上 ⇒ 组件「乱飞」；行号与代码错位 ⇒ **显示区在一处、事件区在另一处**。

**根因**：官方 `win.form` 默认把控件登记进**自动缩放名单**，窗体改尺寸时
官方按「设计时矩形」重新摆一遍。`nodio.ui.addCtrl` 里的 `autoResize=false`
只覆盖了走 `addCtrl` 建的控件，而 IDE 主窗体那个 `richedit`
是**直接 `winform.add(...)`** 建的，漏了这一个。

**修法**：`ide/nodio/app/shell.nodio` 里那个 richedit 补 `autoResize=false`。

### 1.24.5 顺带修掉的四个「点了没反应」

| # | 症状 | 根因 | 修法 |
|---|---|---|---|
| 1 | 所有操作反馈 300ms 后消失 | 心跳每 300ms 写一次状态栏「就绪」，把用户消息盖掉 | `setStatus` 给状态栏加**保持期**（15 拍 ≈ 4.5s），心跳期间不许覆盖（`shell.statusIdleAllowed()`） |
| 2 | 一动鼠标就卡一下 | 状态栏文字没变也整条重画（自绘板，3840px 宽） | `statusbar.setText` 判重，没变就不重画 |
| 3 | 持续卡顿 | 心跳每 300ms 把**整篇文档**发给 LSP（67KB） | `lspSync` 判重，文本没变不送 |
| 4 | F10 永远「调试适配器启动失败」 | `dapClient.start()` 结尾写 `return self` —— 模块作用域里没有 `self`（模块级代码的 `this` 才是模块表） | 改成模块式 API：返回 `"ok"` / 失败原因；调用方一律 `app.dapClient.xxx()` |

调试器另外三处对不上也一并修了：

* `pollDebug` 调的是 `dbgSession.poll(...)`，dapClient 暴露的是 `pollEvents(ev)`；
* `setBreakpoints` 的 `breakpoints` 用 `{}` + 数字键拼，JSON 出来是对象不是数组，
  服务端按数组解析 ⇒ **一个断点都建不起来** → 改成 `[ @{_type="array"} ]` + `..table.push`；
* **断点根本没有入口**：`setBreakpoint` 定义了但全仓库无人调用。
  现在行号栏点一下打/消断点（编辑器组件新增 `breakpoints` / `onToggleBreakpoint` /
  `toggleBreakpoint`，行号左侧画红点），调试会话起来后把之前打的断点补发给适配器。

### 1.24.6 坑

1. **锁屏环境下合成鼠标事件是无效的**：锁屏窗口在 z 序最顶，
   `SetCursorPos` + `mouse_event` 送不到 IDE。要测点击必须**直接发窗口消息**
   （`tests/ide_click.ps1` 就是这么做的）。
2. **`ChildWindowFromPointEx` 的 `CWP_SKIPTRANSPARENT`**：会跳过
   `WS_EX_TRANSPARENT` 的窗口。用它做命中测试会误判「浮层点不到」——
   实际上 `WS_EX_TRANSPARENT` **不影响**命中测试（`flags=0` 时能命中）。
   这个误判差点让我把「加 transparent」当成错误方案。
3. **窗口最大化是异步的**：`ShowWindow(SW_RESTORE)` 会把最大化窗口还原回正常位置，
   而且还原带动画 —— 紧接着取 `ClientToScreen` 拿到的还是最大化时的坐标，
   点击位置和实际窗口完全对不上。测试脚本里不要用 `SW_RESTORE`。
4. **`::SetWindowPos` / `::ShowWindow` 不是引擎内置符号**：
   要写 `::User32.SetWindowPos` / `::User32.ShowWindow`。
   写成裸 `::` 会在窗口过程里抛异常，异常被吞掉，症状是「函数后半段没执行」。

### 1.24.7 实测

* 截图 `ide_full.png`：标题栏 12 个菜单齐全，工程树 / 页签 / 编辑器 /
  行号 / 语法高亮 / 断点红点 / 右栏 7 个页签 / 状态栏全部就位。
* 菜单全链路：点「工程」→「工程属性」→ 状态栏显示「工程属性」。
* 行号栏点第 1 行 → 红点出现 + 状态栏「已设断点 行 1」。
* 回归：VM 89/0、EDITOR-TESTS-OK、INPUT-TESTS-OK、SMOKE OK、
  LSP-SMOKE-OK、LSP-PROJECT-SMOKE-OK、DSG-PARSE-PROBE-OK、NAV SMOKE OK。
* `tests/ide_nav_smoke.ps1` 的行号断言原来**写死 114**（被测文件一改就红），
  改成跟 IDE 自报的 `NAV-FNLINE` 比。

---

## 1.25 设计器：画布整块黑 / 开关一次设计器代码就没了

### 1.25.1 现象

* 打开设计器 → 编辑区**整块黑**，只剩行号栏的数字；
  画布底板在、可见（`vis=true`）、矩形也对（`408,129,3312,1791`），
  z 序也在最顶（`GetWindow(GW_HWNDPREV)` 往上爬一个都没有）。
* 关掉设计器 → 编辑器里**代码不见了**（行号还在，正文空）。

### 1.25.2 根因

**画布黑**：官方 plus 的 `WM_PAINT` 用「本次失效区域」当绘制裁剪区。
设计画布是**创建时隐藏、后来才 `show(true)`** 的控件 ——
Windows 给它首次显示时的更新区域经常是空的，裁剪区一空就等于什么都没画：
窗口在、可见、矩形对、z 序最顶，屏幕上却一块黑。
（提示条、查找条、补全浮层、下拉菜单全踩同一个坑。）

**代码没了**：官方 richedit 的 `show()`/`hide()` 在 Nodio 上会走它自己的
DPI 初始化路径，**重建控件窗口 → 正文被清空**；
清空又触发 richedit 的 `EN_CHANGE` → 编辑器把空文本写回缓存，
于是「开关一次设计器，代码就没了」。

### 1.25.3 修法

* `nodio.ui.board.show(flag)`：`show(true)` 之后**强制整块重画**
  （`InvalidateRect` 整窗 + `UpdateWindow`）。这一条同时救活
  设计画布、提示条、查找条、补全浮层、下拉菜单。
* `nodio.designer.canvas.show(flag)`：显示时把底板**显式提到 z 序最前**
  （官方 `form.add` 一次只加一个控件，层次只等于创建顺序）。
* `app/designer.nodio`：**不再 hide/show 官方 richedit** ——
  画布是不透明浮层且已提到最前，本来就盖得住编辑器；
  官方 richedit 的 show/hide 会清空正文，不能碰。
  行号栏是自绘板，藏它没有副作用，保留。

### 1.25.4 实测

截图 `ide_designer6c.png`：白色表单面 + 网格 + 「按钮」组件 + 选中框 + 8 个手柄。
截图 `ide_src_c.png`：关掉设计器后编辑器内容完好（语法高亮 + 面包屑 + 断点红点）。

---

## 2. 引擎 / 语言缺口

| # | 缺口 | 证据 | 状态（第二轮结束） |
|---|---|---|---|
| E-1 | `json.stringify` 键序错 + 泄漏内部元数据 | 第一轮输出 `{"arr":[1,2,3],"n":43,"name":"aardio","_meta":{...}}` | ✅ **已修**：`engine/runtime.cpp` 的 `Node::ForIn` 表成员快照跳过 `"_meta"`。实测 → `{"arr":[1,2,3],"n":43,"name":"aardio"}`（键序按官方 `table.keys` 默认字典序，与官方一致） |
| E-2 | `time()` 文本解析不符合官方契约 | `time("garbage")` 应返回 null（官方 `docs/library-guide/builtin/time/time.md`：解析未完成即返回 null）；旧实现返回 1900 年垃圾值 | ✅ **已修**：新增标记驱动解析器（`parseTimeText`）+ 格式候选序列（`timeFormatCandidates`）。实测对齐官方：`time("garbage")`→null、`time("20170822")`→`2017/08/22 00:00:00`、`time("20170822123623")`→`2017/08/22 12:36:23`、`time("2017-05","%Y-%m-%d")`→null、`time("16:56","%H:%M:%S")`→null、`time("16:56","%H:%M:%S",,true)`→`16:56:00`、`time("20170822 12")`→`2017/08/22 12:00:00`、`time("06 Nov 1994","%d %b %Y")`→`06 Nov 1994`；并补上官方 `endstr` 成员与 `%b/%B` 月名解析（`time.gmt`/`time.iso8601` 依赖它） |
| E-3 | `thread.waitOne` 返回 `1` 而非 `true` | 第一轮实测 `tostring(thread.waitOne(h,3000))` = `1` | ⚠️ **不是缺陷 —— 测试期望写错**。官方 `lib/builtin/thread.aardio:92` 是 `return ret+1;`，返回**完成句柄的 1-based 索引**（官方文档同）。夹具 t28/t29 已按官方改写为 `142` / `11true` |
| E-4 | `string.hex` 输出格式 | 第一轮实测 `"4142"`，怀疑官方为 `"41 42"` | ⚠️ **不是缺陷 —— 测试期望写错**。官方 `string.hex(data, prefix)` 第 2 参是**前缀不是分隔符**，无前缀时输出无分隔符，`string.hex("AB")` = `4142` 正确。夹具 t25 已改写 |
| E-5 | `=aardio` 字节码模块报"语法错误"而非格式错误 | 第一轮：`库 ...\aifix.aardio 编译失败: 语法错误: 意外的 = @ 行 1` | ✅ **已修**：`engine/import.cpp` 两条 import 路径在 `IsBytecodeData` 失败且 `IsCompiledModule(src)` 为真时，报 `格式错误: 不支持的字节码格式 (=aardio   )`。实测（导入官方 `lib/ide/aifix.aardio`）→ 清晰格式错误 |
| E-6 | `raw.loadDll` 失败时把整个 DLL 二进制当路径打进错误信息 | 第一轮：`无法加载 DLL: MZ   ...This program cannot be run in DOS mode.` | ✅ **已修**：失败时若为内存映像则报 `(内嵌的内存映像)`，否则路径截断到 120 字符加 `...`。实测 → `运行错误: 无法加载 DLL: no_such_dll_xyz_12345.dll @ 行 3` |
| E-7 | 缺 `metaProperty` 内建 | `win.ui.ctrl.thread`：`尝试调用非函数值 (metaProperty)` | ✅ **已修**：官方库 `win/ui/ctrl/metaProperty/_.aardio`（含完整 `_metaPrototype`，`enableDpiScaling` 等元表链）由 overlay 的同路径文件**遮蔽**才导致 `尝试调用非函数值 (metaProperty)`。已删除 overlay 里的同名桩文件 `lib/win/ui/ctrl/metaProperty.aardio`，让官方包正常解析。实测：`f.enableDpiScaling` 恢复为函数、`f.show()` 正常、`exit=0`。**注意**：overlay 优先于官方库的规则不变，但 overlay 不得再放官方已发布的包的同名文件（`name\_` 目录包候选见 `engine/import.cpp` 的 `relCands`） |
| E-8 | 缺 `win.registerClassEx` 原生面 | `win.ui.ctrl.splitter`：`尝试调用非函数值 (.registerClassEx)` | ⚠️ **官方同款限制，不改引擎**：官方库要求先 `import win;`，Nodio 行为与官方一致 |
| E-9 | `web.layout.event` 触发 `assertion failed!` | 第一轮模块 sweep | ✅ **第二轮已不复现**：`web.layout.event` 在 969 模块 sweep 中导入通过（`MOD_OK`）。仍需留意其 zlib 依赖（32 位 DLL 边界） |
| E-10 | **裸 `::` 系统 API 的签名表不完整**（官方 `::SetWindowPos` / `::CallWindowProc` 形式已可用） | 官方 aardio 的 `::Func` 会在系统 DLL 列表里查找（user32/gdi32/kernel32…）；Nodio 原先只查引擎内置根表 | ✅ **部分已修**：`engine/sysapi.cpp` 内置一张系统 API 签名表（约 90 个 API），`::` 查表命中后按签名解析并缓存。官方 `win.ui` 的**窗体级**事件路由因此恢复。**剩余**：官方**控件级**路由仍装不上窗口过程（见 §1.8 P1-14），自绘组件改用自己的 `nodio.ui.subclass`；表外的 API 仍需 `raw.loadDll(path).api(name, 显式签名)` |
| E-11 | `raw.tostdcall` 建的回调**永不释放** | `engine/ffi.cpp` 的 `g_cbs` 只增不减（`g_cbs.push_back(ctx)`），`CbCtx` 里的 `fn` 还被当成 GC 根 → 回调闭包连同捕获的一切永久钉住 | ✅ **已修**（第七轮）：官方契约见 `D:\aardio\docs\library-guide\builtin\raw\callback.md` §2/§6 —— 回调是 **cdata 对象**（带 `_topointer`、可从脚本调用），**由 GC 自动回收**。改法：`engine/ffi.cpp` 加 trampoline 内存池（64KB 页切 256B 槽，**释放的槽不复用**，防原生侧迟到调用串到别的回调）+ `ReleaseCallback()`（清 `fn/owner` 断脚本引用、把跳板改写成 `xor eax,eax; ret` 中性桩）；`engine/builtins.cpp` 的 `makeCallback` 改为返回 cdata（`Value::Pointer` + `_meta` 含 `_topointer`/`_call`）；`engine/gc.cpp` 的 `g_cbs` 根标记跳过 `rooted==false` 的 ctx；`engine/runtime.cpp` 支持从脚本调用 cdata。**比官方更安全**：官方 §6 明说「你释放了原生回调函数，可是外部组件的 C、C++ 却并不知道……将会导致不可预知的错误」——官方直接 free，Nodio 改成置中性桩，脚本侧零泄漏 + 原生侧零野指针。回归 `tests/vmtests/t91.aardio`（CDATA-TYPE/CALL/OWNER/RELEASED）；缩放实测：2k/20k/200k 回调 GC 后存活表恒为 145（基线 134），原生侧固定 256B/回调 |
| E-12 | `import` **自动预载父命名空间**，与官方契约不一致（5 个官方「禁止直导」guard 全部失效） | 第二轮发现：`engine/import.cpp` 的「Hierarchical namespace loading」会在 `import web.form.query` 之前先加载 `web.form`，官方 guard `type(web[["form"]]) != type.class` 因此永远为假 | ✅ **已修**（第七轮）：官方契约三条硬证据 —— ① `D:\aardio\docs\library-guide\import.md` 的查找顺序只有 4 条路径（`~\lib\a.b.c.aardio` / `~\lib\a\b.c.aardio` / `~\lib\a\b\c.aardio` / `~\lib\a\b\c\_.aardio`），**没有任何父命名空间预载**；② 同文档「在 aardio 中库一定要自行创建与路径相同的命名空间。如果未创建一致的命名空间，则导入库失败」→ 命名空间链由库自身的 `namespace a.b;` 建立（`NsPath()` 是 resolve-or-create）；③ 官方库显式声明依赖（`lib\win\ui\_.aardio` 开头就 `import win; import win.ui.background; import win.ui.ctrl; import gdi;`）。改法：删除 `engine/import.cpp` 的预载块。实测 5 个官方 guard 全部按**官方原文**报错：`请直接导入web.form` / `只能通过导入 web.view 导入 web.view.dev` / `请导入 web.acme 库` / `只能在 winex.accObject中 导入 winex.accObject.element` / `sqlite.base禁止直接导入`；同时 Nodio 自有多层命名空间（`nodio.ui.board` / `nodio.designer.registry` / `win.ui.ctrl.plus` / `win.ui.tracker`）绑定不变。回归 `tests/import_contract_check.ps1`（10 项，x64/x86 全绿） |
| E-13 | 引擎系统 API 表把 **gdi32 的 region API 错标成 user32** | 官方 `lib\win\region\_.aardio:6` 的 `::CreateRoundRectRgn := ::Gdi32.api("CreateRoundRectRgn", ...)` 报 `找不到 API: CreateRoundRectRgn`。根因：赋值的**左值** `::CreateRoundRectRgn` 会走引擎的 `::Name` 系统 API 解析（`engine/sysapi.cpp` 的 `FindSysApi`），而表里 `CreateRoundRectRgn` / `CreateRectRgn` / `CombineRgn` 的 dll 字段写成 `user32.dll`（实际在 gdi32.dll）→ 解析到一个不存在的导出 | ✅ **已修**（第七轮）：三个条目的 dll 字段改成 `gdi32.dll`。实测 `win.region` / `win.region.round` / `win.util.round` / `web.ui` 由 FAIL 转 OK；`win.region.bitmap` / `win.region.png` 越过这个错误后暴露为 32 位 DLL 边界（`无法加载 DLL: (内嵌的内存映像)`），属物理边界。回归 `tests/sysapi_check.ps1`（6 项，x64/x86 全绿） |
| E-14 | 官方预编译模块（`=aardio`）只报笼统的「不支持的字节码格式」 | 导入官方 `lib\ide\aifix.aardio` 只得到 `格式错误: 不支持的字节码格式 (=aardio   )`，无法判断「格式没读懂」还是「读懂了但没实现」；而容器层其实早已完全还原（`tools/aardio_bytecode.py` 字节级回归通过） | ✅ **已做（第九轮）**：把 §3 的 chunk 解析器移植进引擎（新增 `engine/aardio_chunk.cpp` / `engine/aardio_chunk.h`），`engine/import.cpp` 命中 `=aardio` 时先解析再报**结构化诊断**：`格式错误: 官方预编译模块 (=aardio) 暂不可执行 — 已解析出 97 个对象 / 8734 条指令 / 2173 个常量（最大嵌套 4 层），但 Nodio 尚未实现该字节码的解释器`；结构损坏时报「结构不完整」，非 `=aardio` 魔数仍走原分支。计数与 Python 参考实现**逐项一致**。回归 `tests/aardio_chunk_check.ps1`（10 项，x64/x86 全绿）。**可执行性仍是物理边界**：61 条记录种类语义未还原，见 `docs/aardio-bytecode.md` §6.3/§6.4（第九轮新导出节点优先级表、官方内建分派表、10/61 条记录语义、21/61 条校验约束）。<br>**第十轮续（记录结构完整导出 + 分派表证伪）**：把官方「记录种类的操作数格式描述符表」`DAT_007C80A0`（VA `0x007C80A0`，64 字节）完整解出并逐条对照官方 chunk 校验器 `FUN_004d4450` 的取操作数代码，**修正了第九轮对该表位含义的错误推断**：① 合法记录种类只有 **61 种**（`0x00`–`0x3C`，校验器 `if (0x3c < uVar6) return 0;` 直接拒绝 `0x3D`–`0x3F`），「64 种」来自种类字段占 6 位；② 描述符 bits0..1 = 操作数格式类（0=双 9 位操作数 / 1=单 18 位无符号 / 2=单 18 位有符号）、bits4..5 = 槽位索引或相对位移、bit6 = 可作跳转目标、bit7 = **必须紧跟一条 0x22 跳转记录**；③ 61 种按描述符字节归并为 **24 种记录形态**（14 种是「必须紧跟 0x22」的条件跳转类）。同时用 Ghidra `ListRefs` 反查描述符表的全部 5 个引用点，**全部在编译/校验侧、没有运行期引用** → 官方**不存在 64 路分派表**（运行期对记录种类是逐种类直接分派），这与第九轮按「目标跨度 < 8KB」筛选 64 项跳转表全部命中 MFC 消息表的结论一致。固化：`tools/aardio_vm_tables.py` 的 `OPERAND_DESC_BYTES` / `decode_operand_desc()` / `RECORD_SHAPE_GROUPS` / `check_shape_groups()`（自带 61/61 覆盖自检）；文档 `docs/aardio-bytecode.md` §5.1/§5.2/§6.9。<br>**第九轮续（VM 架构已定位）**：找到官方 VM 主分派 `FUN_004EC700` 与记录发射器 `FUN_004F5B20`，确认官方 VM 是 **AST 树遍历 + 记录流**（不是 RISC 指令流）：记录字的 C 段/B 段在运行期被回填为「表达式项数 / 语句数」；并导出 AST 节点类型 → 26 类运算符的映射（`FUN_004EE2E0`，优先级与 aardio 运算符阶梯自洽）、83 个记录发射点（10 类 kind 为常量）。**这修正了路线 3 的做法**：要还原的不是「64 条机器指令」，而是「64 种记录各自的语义 + AST 节点类型空间」。<br>**x86 专属缺陷（第九轮自测发现并当场修掉）**：越界检查写成 `pos + n > size`，在 **32 位构建**上 `size_t` 只有 32 位，`15 + 0xFFFFFFFE` 回绕成 13 → 越界读进 `std::string` 抛 `string too long`（x64 不触发）。改法：改成 `n > size - pos` 这类**不做加法**的比较。同轮还发现 `tests/import_contract_check.ps1` 的 sqlite 用例在 x86 上必然失败（overlay 的 `lib\sqlite\.res\sqlite3.dll` 是 **64 位**，32 位进程加载不了 → `无法加载 DLL: (内嵌的内存映像)`，属 32 位 DLL 物理边界），已改成「父命名空间本身能导入才断言子模块绑定，否则显式 SKIP 并写明原因」，双架构都绿<br>**第十一轮续（运行期解释器已定位 · 61 种语义已读出）**：**第十轮剩下的最后一个真缺口被正面拿下** —— 官方**运行期记录解释器本体** `FUN_004C4BC0` @ `0x004C4BC0`（函数体 **7263 字节**）是整个 `aardio.exe` 里**唯一**一处对 32 位记录做 `switch (word & 0x3F)` 的函数，case 覆盖 `0x00`–`0x3C` **全部 61 种**（与 chunk 校验器 `FUN_004D4450` 的 `if (0x3c < uVar6) return 0;` 完全吻合），两个入口是 `FUN_004D33C0`（带 C 栈溢出检查，上限 200 层）与 `FUN_004D3450`。定位手法：从官方调试器变量解析器 `FUN_004D4940` → `FUN_004D4BD0` → `FUN_004D4170` → `FUN_004D42A0`，再由 `FUN_004D50A0`（建立闭包）的第二、第三个调用者 `FUN_004D33C0` / `FUN_004D3450` 落到 `FUN_004C4BC0`。随之实测得到：① **记录字位布局**（kind = bits 0..5；A = bits 6..13；C = bits 14..22、B = bits 23..31，各为「9 位字段 = 低 8 位索引 + 最高 1 位全局作用域标志」；跳转类把 bits 14..31 当**一个 18 位有符号相对位移**用，bias `0x1FFFE`，条件跳转读**下一条 u32**）；② **16 字节槽位布局**（值 +0x00 / 类型标签 +0x08 / 附加 +0x0C / 附加 2 +0x10）与 **8 种类型标签**（0=null 1=bool 2=int 3=number 4=string 5=namespace 6=function 7=table）；③ VM 状态对象与 0x18 字节调用帧的全部字段偏移；④ **61 种记录的语义逐条读出**（算术组 `0x18`–`0x1E` = `+ - * / % ^ 一元-`，helper `FUN_004C4740` 分派参数 = opcode−0x12；位运算组 `0x35`–`0x3B` = `~ & \| ^ << >> >>>`，helper `FUN_004C49C0` 分派参数 = opcode——两组双路佐证）。**顺带修正第十轮的一处结论**：描述符表 `DAT_007C80A0` 的「18 位操作数格式」在**运行期不成立**（解释器里每个 kind 的两个操作数都只取 9 位，18 位只用于跳转位移），该表只能用于编译/校验期；校验器对 kind `0x22` 的解读（跳过 B 条记录）与解释器（18 位相对跳转）**不一致，原因待查**。固化：新增 `tools/aardio_vm_runtime.py`（解释器入口 / 位布局 / 槽位布局 / 类型标签 / 61 条语义表 / `check_opcode_table()` 61-61 自检），`tools/aardio_opcodes.py` 改用**运行期**位布局解码并输出语义名，`tools/aardio_vm_tables.py` 增加运行期小节，文档 `docs/aardio-bytecode.md` 新增 **§7**（7 小节，含 61 行语义表与 8 个未决点）。**状态从「物理边界」降级为「工程量缺口」**：语义不再是未知，剩下的是把记录流翻译回 Nodio 的 `PNode` 并消掉 8 个变体差异<br>**第十一轮续（已进产品：官方 aardio 做不到的「可解释」）**：运行期记录解码**做进了引擎**（`engine/aardio_chunk.h/.cpp`）——`ParseAardioChunkTree()` 保留整棵 chunk 树、`DecodeAardioRecord()` 按官方运行期位布局解码单条记录、`DisassembleAardioTree()` 出反汇编、统计记录种类直方图与跳转可解引用率。① **import 的结构化诊断升级**：官方只会给「bad header」/「格式错误」，Nodio 现在给「97 个对象 / 8734 条指令 / 2173 个常量（最大嵌套 4 层）；记录流已按官方运行期布局解码：8734 条记录，用到 **43/61** 种记录种类，非法种类 **0** 条，**631** 个跳转目标中 **631** 个落在指令边界上」；② **新增 CLI** `nodio.exe -disasm <file>`，打印整棵 chunk 树的反汇编 + 种类直方图 + 跳转落点——**官方 aardio 只能执行自己的预编译块，完全不能反汇编、给不出任何结构结论**（超越项，见 `docs/surpass.md` S-1）；③ 回归 `tests/aardio_chunk_check.ps1` 扩到 **26 项断言**（x64/x86 双绿），定向 sweep 确认两个 `=aardio` 模块的**失败集合未变**（只有文案更详细） |
| E-15 | 自绘工程树**右键完全无效**（工程树右键不弹菜单、点不中节点） | 两处组件层缺陷：① `lib/nodio/ui/tree.aardio` 给 board 接了 `onDraw`/`onMouse*`/`onDoubleClick`/`onMouseWheel`，**唯独没接 `onContextMenu`** —— 而 `lib/nodio/ui/projectTree.aardio:133` 把回调挂在 `self.tree.onContextMenu` 上，那只是没人读的属性（官方 `win.ui.tracker` 的 `WM_CONTEXTMENU(0x7B)` → `onContextMenu(x,y)` 契约见 `D:\aardio\lib\win\ui\tracker.aardio:150-166`）；② `nodeAtScreen` 用 `self.board.ctrl.getRect()`（**客户区**矩形，原点恒 0,0）做屏幕→客户区换算 → 屏幕坐标命中恒 miss | ✅ **已修**（第九轮，组件层，未改任何 IDE 文件）：`tree.aardio` 追加 `self.board.onContextMenu = function(x,y,bd){ if(self.onContextMenu) self.onContextMenu(x,y); }`，并把 `nodeAtScreen` 的换算改成 `..win.getRect(self.board.ctrl.hwnd, true)`（屏幕矩形，与 `nodio/ui/rect.aardio:108` 的 `mouseScreen` 同法）。实测：探针 `HIT-SCREEN 0 → 1`、`CTX-0x7B 321,654`、`CTX-0x204/205 1790,1410`；官方 `win.ui.popmenu` 真实弹窗（Win32 弹出菜单窗口 `#32768` 实测 123×45 = 两个菜单项「打开 / 复制路径」）。回归 `tests/ide_ctx_smoke.ps1`（**CTX-SMOKE OK**，x64+x86）+ `tests/ide_smoke.ps1`（SMOKE OK）+ VM 90/90 |
| E-16 | overlay 里出现**非法 for 步长** `for(i=1;#items;1++)` | 官方 aardio 的 for 步长是**常量**（`for(i = first, last [, step=1])`），`1++` 是给字面量自增；`lib/nodio/ui/titlebar.aardio:326` 触发 `tests/ide_smoke.ps1` 的 overlay 守卫，`ide_smoke` 直接 FAIL（IDE 侧编译不过） | ✅ **已修**（第九轮）：改为 `for(i=1;#items;1)`，与同文件 298/310 行的写法一致。官方 979 个 `.aardio` 文件里真 aardio 代码一律用 `1`（`for(...++...)` 的 28 处命中全在 JS/C# 嵌入源码里）。回归：`tests/ide_smoke.ps1` 恢复 **SMOKE OK**（PROJ-FILES:9 / TREE-CTRL:1） |
| E-17 | 引擎内置 `win.clip` **只有官方 12 个成员里的 2 个**（read/write），且「导入官方包」与「用内置兜底」两条路径行为不一致 | 官方 `D:\aardio\lib\win\clip\_.aardio`（`namespace win.clip;`）定义 **12** 个成员：`readUtf16/read/write/writeBitmap/readBitmap/clear/test/formats/formatNames/getFormatName/eachFormat/readUnicode`；官方文档 `docs\guide\language\syntax-quick-ref.md:1621`、`docs\guide\ide\system-prompt.md:2541` 要求显式 `import win.clip;` 才能用。而 Nodio 的 IDE（`ide/nodio/app/editor.nodio:20/25/28`）**不导入**、直接用 `win.clip.write/read`，靠引擎内置表兜底。实测（未导入时）：`type(win.clip.formats)`=null、`type(win.clip.clear)`=null、`type(win.clip.test)`=null → **官方包的 10 个成员根本没生效**；`import win.clip;` 后 12 成员齐备且官方实现完全可用（实测 `write` 返回剪贴板数据句柄、`read` 读回文本，且与内置实现互通）→ 说明官方包能跑，缺的是**内置兜底的 API 面**与**两条路径的一致性** | ✅ **已修**（第十轮，引擎层，未改任何 IDE 文件）：`engine/builtins.cpp` 把内置 `win.clip` 补齐到官方 **12 成员 API 面**，语义按官方：① `write(str, rep)` —— `rep` 控制复制前是否先 `EmptyClipboard`（官方：rep 为 null 或 true 时清空），成功返回**剪贴板数据句柄**（官方契约，不再返回 bool）；② `read()` / `readUtf16()` —— 剪贴板里没有 `CF_UNICODETEXT` 时返回 **null**（官方语义；官方 aardio 里空串是**真值**，返回 null 才能让 `if(s)` 正确地跳过粘贴）；③ `formats()` / `test()` / `getFormatName(id,friendly)` / `eachFormat()` / `clear()` / `writeBitmap()` / `readBitmap()` 全部按官方语义实现（`formatNames` 表与官方 24 个预定义友好名逐项一致）。**编码差异（已知且无害）**：官方包 `read()` 返回 UTF-8 **字节串**（官方 aardio 的字符串就是 UTF-8 字节串），Nodio 的字符串模型是**宽文本**，内置 `read()` 返回宽文本——剪贴板载荷（CF_UNICODETEXT）完全一致，且实测官方 richedit 的 `text/selText` **宽文本与 UTF-8 字节串两种形态都接受**（`中文ABC` 往返正确）。回归 **新建** `tests/win_clip_check.ps1` + `tests/win_clip/clip.nopio`：同一条脚本里先跑**内置实现**（B-*）再 `import win.clip;` 跑**官方包**（O-*），断言两路径的 12 成员面、`formatNames`、`getFormatName` 友好名、写入/读回/clear 语义**逐项一致**；剪贴板被同机其它进程占用（`OpenClipboard` 返回 0）时按官方「静默失败」语义断言 null/0——x64/x86 全绿。**同轮修掉 `tests/ide_smoke.ps1` 的守卫误报**：原正则 `for\s*\([^;]*;[^;]*;\s*[^;]*\+\+` 会把**循环体**里的 `++` 当成 for 步长（并发会话的 `lib/nodio/ui/editor.aardio:354` `for(i=1;#lines;1) out[i] = INDENT ++ lines[i];` 被误判），改为只看 for 头第三子句。<br>**sweep 证据（E-17 定向回归）**：对官方库全量检索 `win\.clip` 得 21 个引用者
（`win.clip` + 11 个子模块 + `win.ui.ctrl.richedit` / `win.ui.ctrl.pick` / `winex.editor` /
`winex.selection` / `process.adb.clipboard` / `web.rest.aiChat.settingForm` /
`autos.tools.handlers` / `autos.ui.memories` / `autos.skills.videoDownloader`），
新建清单 `tests/clip_modules.txt` 用 `tests/module_sweep.ps1 -List` 定向跑：
**x64 ok=18 / fail=3**，3 个失败**全部是既有项**且与本改动无关 ——
`autos.skills.videoDownloader`（32 位 DLL 物理边界）、`autos.tools.handlers`
（`=aardio`，E-14 物理边界）、`autos.ui.memories`（32 位 DLL `sundown.dll`，**x86 构建下导入成功**，
已逐层定位到 `string/markdown/_.aardio:85`）。基线对照：`.module-sweep-r9-x64.log` 与
`.module-sweep-r10-x64.log`（13:11，早于本轮改动）里 `autos.ui.memories` 是**同样的错误**、
13 个 `win.clip*` 全部 OK → **E-17 零回归**。<br>**全量 sweep 复核（本轮重跑 969 个公开模块，
拆 4 段并行以绕开 3 小时上限）**：**ok=897 / fail=72**，与 r10 基线**数值完全一致**，
且**失败集合逐项相同**（差集双向为空：本轮新出现 0 项、基线有而本轮恢复 0 项）→ E-17 对
969 个官方模块**零回归**。

---

## 3. 标准库行为缺口

全量 sweep：`tests/module_sweep.ps1`（第一轮新增，PowerShell 版 `tests/ns_sweep.sh`，
10s/模块、**子进程 stdout/stderr 走文件重定向**——孙进程（如 `process.adb.connecter`
拉起的常驻 adb server）继承管道会让 `ReadToEnd` 永久阻塞，第二轮已改成文件重定向）。
命令：

```
pwsh -File tests\module_sweep.ps1 -Exe D:\Nodio\build-x64\Release\nodio.exe
```

**第一轮**：969 个公开模块中 78 个未通过（分两段跑完：1–416 段 26 失败，417–969 段 52 失败）。
**第二轮复测（x64，969 个公开模块全覆盖）**：`ok=925`、非 OK **44**（第一轮 78）。
**第四轮复测（x64，同一判据）**：`ok=957`、非 OK **12** —— 第二轮那 44 项里有 **32 项是
sweep 工具链自身的 exe 路径缺陷造成的假失败**（把 `nodio.exe` 拷到 `D:\Nodio\.sweep\`
再跑，overlay 资源按 `~` 相对路径解析会落到错误目录 → `zlibwapi.dll` 加载失败 →
27 个 zlib 模块级联报错）。本轮改用**同目录改名副本**
（`build-x64\Release\nodio-sweep.exe`）后真实残差降到 12 项，与下表"物理边界"三类吻合。
判定口径：**stdout 出现 `MOD_OK` 即视为导入成功**（退出码不可靠：某个模块留下的常驻
孙进程会让后续每次运行都返回非 0 退出码，尽管模块本身导入成功）。

**第四轮实测残差 12 项（全部可枚举，无新增）**：

| 类别 | 数量 | 性质 |
|---|---|---|
| 32 位 DLL 内存映像（`无法加载 DLL: (内嵌的内存映像)`） | 8 | **物理边界**：`av.audio` / `av.film` / `av.video.flicker` / `av.video.kernel` / `gdip.feather` / `fsys.update.maker` / `autos.skills.codingFilm` / `autos.skills.videoDownloader` |
| 32 位 DLL 文件路径 | 1 | `com.autoComplete` → 官方安装目录下的 32 位 COM DLL |
| `=aardio` 字节码 | 2 | `autos.tools.handlers` / `ide.aifix` → `格式错误: 官方预编译模块 (=aardio) 暂不可执行`（E-5 + E-14 已修；**第十一轮后语义已全取得，剩实现工作量**） |
| 官方库自身引用缺失 | 1 | `crypt.jwt.algorithm.EdDSA` → `import BouncyCastle;`，官方 lib 树里没有 BouncyCastle —— 官方库缺陷 |

**第七轮复测（x64，969 个公开模块全覆盖，用**修正后**的 harness）**：`ok=897`、非 OK **72**。
这是本项目**第一次可信的**全量数字（原因见本节末「sweep 工具链缺陷」）：前四轮
「残差 12 项」实际只覆盖了前 417 个模块。72 项**全部可归因，零引擎缺陷残留**：

| 类别 | 数量 | 性质 | 代表模块 |
|---|---|---|---|
| 32 位 DLL 内存映像（`无法加载 DLL: (内嵌的内存映像)`） | 32 | **物理边界**（x86 全部通过） | `av.audio*` / `gdip.feather` / `fsys.update.maker` / `sevenZip.lzma*` / `string.markdown*` / `web.form.chat` / `win.dlg.flashDict` / `raw.argsPointer` / `raw.fasm` … |
| 32 位 DLL 文件路径 | 2 | **物理边界** | `com.autoComplete`（官方安装目录下的 32 位 COM DLL）、`web.layout`（`HTMLayout.dll`，实测 x86 通过） |
| 官方「先导入父命名空间 / 禁止直导」契约 | 25 | **契约正确**（官方同样报错） | `sqlite.base` / `sqlite.backup` / `web.form.query` / `web.view.dev` / `web.acme.httpClient` / `winex.accObject.element` / `web.layout.event`（`assert(web.layout && web.layout.behavior)`）/ `py2.*` `py3.*`（需先 `import py2` / `import py3`）/ `win.ui.ctrl.splitter`（需先 `import win`）/ `win.ui.ctrl.thread`（需先 `import win.ui.ctrl.metaProperty`） |
| 下载 / 安装 / 交互型模块（TIMEOUT） | 6 | **环境所致**，非缺陷 | `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` / `fastcgi.iisInstall` / `process.php.7.4` / `process.php.8.3` |
| 官方库自身引用缺失 | 3 | **官方库缺陷** | `crypt.jwt.algorithm.EdDSA`（`import BouncyCastle;`）/ `process.ruby` / `process.ruby.simpleHttpServer`（`sevenZip\decoder2\httpFile`） |
| `=aardio` 闭源字节码 | 2 | **工程量缺口**（格式已复原；**运行期语义已于第十一轮全部取得**，只剩「记录流→`PNode` 翻译器」的实现工作量，不再是未知） | `autos.tools.handlers` / `ide.aifix` |
| 伪模块（清单里的名字不是合法标识符） | 1 | **清单问题**，非引擎缺陷 | `win.#DEFINE`（官方文件是 `lib\win\#DEFINE.aardio`，`#` 不能出现在 import 名里） |

**第十轮补充定位（`autos.ui.memories` 的确切根因）**：该模块报
`无法加载 DLL: (内嵌的内存映像) @ 行 85`，但**行号属于被导入的官方包**，
不属于 `memories.aardio` 本身——当时引擎的运行错误只带行号、不带文件名，容易误判。
二分定位链路：`import string.markdown.toRtf;` → `string\markdown\toRtf.aardio:4`
`import string.html.toRtf;` → `string\markdown\_.aardio:85`
`var dll = ..raw.loadDll($"~/lib/string/markdown/.res/sundown.dll","sundown.dll","cdecl")`
→ `sundown.dll` 的 PE machine = `0x014C`（**x86 32 位**）。
**决定性验证**：同一脚本在 **x86 构建下导入成功**、在 x64 构建下失败
→ 归入「32 位 DLL 内存映像」物理边界类（该类 32 项，`string.markdown*` 是代表之一），
**不是引擎缺陷**。

> **第十三轮已修（S-2）**：运行期错误现在自带**文件 + 行号**
> （`@ <文件>:<行号>`，归属到**定义帧**所在的文件），上面这种「行号属于被导入的
> 官方包」的歧义不再存在——回归锁 `tests/s2_error_location_check.ps1`。

**第十九轮复测（x64，2026-10-21，969 个公开模块全覆盖，修正后 harness）**：
`ok=897`、非 OK **72** —— 与第七轮可信基线**逐位一致**（无回归、无新增失败），
日志 `\.sweep-x64-r19.log`。

**这一轮把「aardio 全部标准库可用」这件事的定义钉死了**：

| 类别 | 数量 | 性质 | Nodio 能否自行消除 |
|---|---|---|---|
| 32 位 DLL（内存映像 32 + 文件路径 2 + 以「尝试调用非函数值 (.api)」暴露的 python / HTMLayout / comctl32 一批） | 46 | **物理边界**：官方这些模块只带 32 位 PE 映像，x64 进程装不下 | ❌ 物理上不可能（**x86 构建下全部通过**） |
| 官方「先导入父命名空间 / 禁止直导」契约（`sqlite.base` / `sqlite.backup` / `web.form.query` / `web.view.dev` / `web.acme.httpClient` / `winex.accObject.element` / `win.ui.ctrl.thread` / `py3.pip`） | 8 | **契约正确**：官方同样报错 | ❌ 官方行为，不是缺口 |
| 下载 / 安装 / 交互型模块（TIMEOUT） | 6 | **环境所致** | ❌ 环境限制 |
| 官方库自身引用缺失（`crypt.jwt.algorithm.EdDSA` 依赖 BouncyCastle、`process.ruby*` 依赖 `sevenZip\decoder2\httpFile`） | 3 | **官方库缺陷** | ❌ 官方自己也不通 |
| `=aardio` 闭源字节码（`ide.aifix` / `autos.tools.handlers`） | **2** | ~~**工程量缺口**~~ ✅ **已修（第二十一轮）**：官方预编译块已能真正执行（记录 VM 61/61 + owner 调用约定 + 多返回值） | ✅ **已消除** |
| 伪模块 `win.#DEFINE` | 1 | **清单问题**：官方文件名带 `#`，不能出现在 import 名里 | ❌ 清单修正 |

> **结论**：969 个公开模块里，**Nodio 侧唯一还能靠自己解决的缺口就是那 2 个
> `=aardio` 模块**（两者指向**同一个**官方预编译块 `lib\ide\aifix.aardio`，
> 所以一次修复同时解掉两个）。其余 70 项分别是物理边界（x86 全通）、
> 官方契约、官方库自身缺陷与环境所致 —— **不是 Nodio 的缺陷**，
> 应写进交付说明而不是继续投入。
>
> 也就是说，「aardio 全部标准库可用」这条线的**收口动作非常集中**：
> 把记录 VM 从 57/61 推到 61/61，就换来 x64 `ok=897 → 899`、x86 `ok=931 → 933`；
> 剩下的就是交付说明里的三张表（物理边界 / 官方契约 / 官方库缺陷）。
> 相关取证见 `docs/aardio-bytecode.md` §7.5.8。

**第十九轮复测（x86，2026-10-21，同一 harness）**：`ok=930`、非 OK **39**，日志 `\.sweep-x86-r19.log`。
与 x64 的失败集逐项交叉比对：**x86 的失败集是 x64 的严格子集，差集正好 34 项**，
且这 34 项在 x64 上的失败原因全部是「无法加载 DLL: (内嵌的内存映像)」——
即官方这些模块只带 32 位 PE 映像，x64 进程装不下，**x86 构建下全部通过**。
除这 34 项外，两个架构的失败项一一对应且失败原因完全相同，说明
**没有位数相关的引擎缺陷**。

**第二十轮（2026-10-21）实测分类（x64 72 项 / x86 39 项，逐条归因）**：

| 归因 | x64 | x86 | 是否 Nodio 缺陷 | 说明 |
|---|---:|---:|---|---|
| 物理边界：官方只带 32 位 DLL（内嵌内存映像 33 + `HTMLayout.dll` + `std/com/autoComplete.dll`） | 34 | 0 | ❌ 物理不可能 | x86 构建下这 34 项全部通过 |
| 官方库自身：`.api` / DLL 导出符号不是可调用值（`pyN.*` 的 CPython API、`web.layout` 的 HTMLayout 导出等） | 17 | 17 | ❌ 官方库缺陷 | 官方 `.api` 绑定的是 CPython/HTMLayout 的 C 导出，Nodio 未接这些 ABI |
| 环境：下载 / 安装 / 交互型模块超时 | 6 | 7 | ❌ 环境限制 | 10 秒超时内不返回 |
| 官方契约：必须先导入父命名空间 / 禁止直接导入 | 4 | 4 | ❌ 官方行为 | `sqlite.base`、`sqlite.backup`、`web.form.query`、`web.acme.httpClient` |
| 官方契约：被导入模块自己要求先导入别的官方库 | 3 | 3 | ❌ 官方行为 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）、`process.ruby*`（`sevenZip\decoder2\httpFile`） |
| 官方契约：只能经父模块导入 | 2 | 2 | ❌ 官方行为 | `web.view.dev`、`winex.accObject.element` |
| 官方库自身：`pyN.pip` 限制开发环境 | 1 | 1 | ❌ 官方行为 | — |
| 官方库自身：断言失败 | 1 | 1 | ❌ 官方缺陷 | `web.layout.element` |
| **Nodio 自身：`=aardio` 官方预编译块** | **2** | **2** | ✅ **已修（第二十一轮）** | `ide.aifix` 与 `autos.tools.handlers` 指向**同一个** `lib\ide\aifix.aardio` —— 第二十一轮起这两项**已转 OK** |
| 清单问题：伪模块 `win.#DEFINE` | 1 | 1 | ❌ 清单修正 | 文件名带 `#`，不能出现在 import 名里 |
| **合计** | **72** | **39** | | |

> **交付结论（标准库这条线，第二十轮快照）**：官方公开库 969 个模块中，
> **x64 `ok=897`、x86 `ok=930`**；非 OK 项里**只有 2 项是 Nodio 自己的缺口**
> （那一个 `=aardio` 官方预编译块），其余 70 项（x64）/ 37 项（x86）
> 分别是**物理边界、官方契约、官方库自身缺陷、环境限制**——
> **不是 Nodio 的缺陷**，应写进交付说明，不应继续投入。
> 记录 VM 收口到 61/61 后，这 2 项的解法只剩「把 `=aardio` 块接进
> `engine/import.cpp` 并让它跑起来」这一步 —— **第二十一轮已经做完**（见下）。
>
> **第二十一轮（2026-10-21）复测：`=aardio` 官方预编译块已能真正执行**
>
> 官方公开库 **969** 个模块，用同一 harness 复测，并与第十九轮日志**逐模块对比**：
>
> | 架构 | 全部 OK | 非 OK | 相对第十九轮 |
> |---|---:|---:|---|
> | x64 | **899** | **70** | `+2` —— `ide.aifix`、`autos.tools.handlers` 转 OK |
> | x86 | **933** | **36** | `+3` —— 同上两项，外加 `winex.desktop`（上轮 TIMEOUT，本轮通过） |
>
> **逐模块对比：新增失败集为空**（x64 / x86 皆然），唯一变化就是那 2 个
> `=aardio` 模块由 FAIL 转 OK。
>
> > **本轮做完的事**：官方预编译块 `lib\ide\aifix.aardio` 不再只是「解析得出结构」，
> > 而是**真的被记录 VM 跑完** —— `engine/import.cpp` 命中 `=aardio` 时执行该块
> > 并交出模块命名空间。关键语义（owner 调用约定、多返回值、`0x2F` 返回槽与
> > 空结果跳过 `0x22`）见 `docs/aardio-bytecode.md` §7.5.10。
>
> **四类交付表（第二十一轮实测归因，x64 70 项 / x86 36 项）**：
>
> | 归因 | x64 | x86 | 是不是 Nodio 缺陷 | 说明 |
> |---|---:|---:|---|---|
> | **物理边界**：官方只带 32 位 DLL（内嵌内存映像 32 + 文件路径 2） | 34 | 0 | ❌ 物理不可能 | 官方库只提供 32 位 PE 映像，x64 进程装不下；**x86 构建下这 34 项全部通过** |
> | **官方库自身**：`.api` / DLL 导出符号不是可调用值 | 17 | 17 | ❌ 官方库缺陷 | `py2.*` / `py3.*` 的 CPython 导出、`web.layout.*` 的 HTMLayout 导出、`win.ui.ctrl.*` 的原生导出等 |
> | **环境限制**：下载 / 安装 / 交互型模块超时 | 6 | 6 | ❌ 环境限制 | `fastcgi.iisInstall`、`java.jdk.*`、`java.jre.*`、`process.php.*`（10 秒超时内不返回） |
> | **官方契约**：必须先导入父命名空间 / 禁止直接导入 | 6 | 6 | ❌ 官方行为 | `sqlite.base`、`sqlite.backup`、`web.form.query`、`web.acme.httpClient`、`web.view.dev`、`winex.accObject.element` |
> | **官方库自身**：依赖的官方库缺失 | 3 | 3 | ❌ 官方缺陷 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）、`process.ruby*`（`sevenZip\decoder2\httpFile`） |
> | **官方契约**：`py3.pip` 只允许开发环境 | 1 | 1 | ❌ 官方行为 | — |
> | **官方库自身**：断言失败 | 1 | 1 | ❌ 官方缺陷 | `web.layout.event` |
> | **官方契约**：类命名空间必须先导入 | 1 | 1 | ❌ 官方行为 | `web.layout.debug`（要求先 `import web.layout` 创建类命名空间） |
> | **清单问题**：伪模块 `win.#DEFINE` | 1 | 1 | ❌ 清单修正 | 官方文件名带 `#`，不能出现在 import 名里 |
> | **Nodio 自身缺口** | **0** | **0** | — | **已归零**：原 2 项 `=aardio` 预编译块第二十一轮修复 |
> | **合计** | **70** | **36** | | |
>
> > **交付结论（标准库这条线，最终）**：官方公开库 969 个模块中，
> > **x64 `ok=899`、x86 `ok=933`**。非 OK 项里**属于 Nodio 自身缺陷的是 0 项**；
> > 剩余 70 项（x64）/ 36 项（x86）全部落在
> > **物理边界 / 官方契约 / 官方库自身缺陷 / 环境限制** 四类 ——
> > 这四类**不是 Nodio 的缺陷**，应写进交付说明，不应继续投入。
> > 换句话说：**「可以使用 aardio 的所有标准库」这条线在 Nodio 侧已经收口。**
> > 验收口径与两套构建各自的覆盖范围见 **`docs/aardio-stdlib-delivery.md`**（交付说明）。

**第七轮 x86 复测（同一 harness）**：`ok=931`、非 OK **38**。

**x64 / x86 交叉验证（决定性证据）**：x86 的失败集是 x64 的**严格子集**，差集正好 **34 项**，
且这 34 项**全部**是 `无法加载 DLL`（32 位 DLL 物理边界）：`av.*`、`gdip.feather`、
`sevenZip.lzma*`、`string.markdown*`、`web.form.chat`、`web.view`、`win.dlg.flashDict`、
`raw.argsPointer`、`raw.fasm`、`com.autoComplete`、`web.layout`（`HTMLayout.dll`）等。
反过来，**x86 没有任何 x64 之外的新失败**。即：

> 两套架构上，**引擎侧零残留缺陷**。剩下的 38 / 72 项分别归因于
> 官方「先导入父命名空间 / 禁止直导」契约（25）、下载安装型模块超时（6）、
> 官方库自身缺依赖（3）、`=aardio` 闭源字节码（2）、伪模块 `win.#DEFINE`（1）。

**第七轮 x64 实测变化（同一 harness 下改前/改后逐模块对比）**：改前（E-12/E-13 落地前）
`ok=887`、非 OK **82**；改后 `ok=897`、非 OK **72**。差异全部是修复与偶发收敛，**零新增失败**：



- 6 项「空输出 FAIL」是偶发（单独复跑全部通过）→ harness 现在对空输出自动重试一次；
- 6 项 `找不到 API: CreateRoundRectRgn` → **真引擎缺陷已修**（§2 E-13），其中 4 项转 OK
  （`web.ui` / `win.region` / `win.region.round` / `win.util.round`），2 项
  （`win.region.bitmap` / `win.region.png`）越过这个错误后暴露为 32 位 DLL 物理边界；
- 在**旧 harness 可信的前 417 个模块**区间内，改前/改后逐模块失败集**完全一致**。



| 类别 | 数量 | 性质 |
|---|---|---|
| zlib / 32 位 DLL 级联（`尝试调用非函数值 (.api)` 模块: `lib\zlib\zip.aardio` / `unzip.aardio`） | 27 | **假失败**（第四轮证伪：exe 路径导致 `~` 解析偏差；同目录副本下 zlib 全部通过） |
| 32 位 DLL 内存映像（`无法加载 DLL: (内嵌的内存映像)`） | 8 | 同上（`av.*`、`gdip.feather`、`autos.skills.codingFilm` 等） |
| 32 位 DLL 文件路径（`zlibwapi.dll` / `autoComplete.dll`） | 4 | 同上 |
| `=aardio` 字节码 | 2 | `autos.tools.handlers` / `ide.aifix` → `格式错误: 官方预编译模块 (=aardio) 暂不可执行`（E-5 + E-14 已修；**第十一轮后语义已全取得，剩实现工作量**） |
| 官方库自身引用缺失 | 1 | `crypt.jwt.algorithm.EdDSA` → `import BouncyCastle;`，官方 lib 树里没有 BouncyCastle —— 官方库缺陷 |
| 需真实外设 / 交互 / 网络 | 4 | `fastcgi.iisInstall`（交互提示）、`java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora`（TIMEOUT） |
| 其他 | 1 | `autos.skills.blender`（无输出、无错误文本，待复现） |

**已修（第七轮）：import 曾自动预载父命名空间，与官方契约不一致。**
`engine/import.cpp` 的「Hierarchical namespace loading」会在 `import web.form.query` 之前
先加载 `web.form`。官方文档（`docs/library-guide/import.md`）的查找顺序只有
`~\lib\time.ole.aardio` / `~\lib\time\ole.aardio` / `~\lib\time\ole\_.aardio` —— **官方不会
自动导入父命名空间**。后果是官方库自己的"禁止直导"自检失效：
`sqlite.base`、`web.form.query`、`web.view.dev`、`web.acme.httpClient`、
`winex.accObject.element` 都能直接导入（官方会报 `请直接导入web.form` 之类）。
**第七轮已删除该预载块**（见 §2 E-12）：5 个官方 guard 现在全部按官方原文报错，
回归 `tests/import_contract_check.ps1`（10 项，x64/x86 全绿）。
这次修正让一批模块在**单独导入**时开始报错——**这是官方行为**：它们以前"能导入"只是因为
预载顺手把父命名空间连同它的错误一起吞掉了（典型：`av.audio.dsp` 之类子模块，父库
`av.audio` 在 x64 装不下 32 位 DLL，错误被预载的 `*err` 备份/恢复逻辑掩盖了）。

**第七轮发现（工具链缺陷）：969 模块 sweep 的判定被跨模块污染，后半程 552 个模块的结论是假的。**
`tests/module_sweep.ps1` 原先用**同一个** stdout/stderr 重定向文件跑所有模块
（`$tmpOut` / `$tmpErr` 全程复用）。列表第 418 个模块 `process.adb.connecter` 会拉起
常驻 adb server，孙进程继承并一直占着这个重定向句柄；于是**从第 418 个模块起，读到的
stdout 里混进了别的进程写的 "MOD_OK"**，判定全部变成 OK。证据三条：

1. 第 418 个模块起，每一行都带 `exit=1`（常驻进程占住控制台的签名）；
2. 5 个官方 guard 模块（`sqlite.base`=512、`web.acme.httpClient`=694、`web.form.query`=710、
   `web.view.dev`=791、`winex.accObject.element`=931）**全都落在污染区**，整轮 sweep 判 OK，
   单独跑却是正确的 FAIL；
3. r4 / r6 / r7 三份整轮日志的 FAIL **全部落在第 335 个模块之前** —— 之后的 634 个模块
   从来没有被真正验证过，历轮"残差 12 项"实际只覆盖了前 417 个。

修法：**每个模块用独立的临时文件**（`sw.<guid>.out` / `.err` / `.cmd`）。修好后
`process.adb.connecter` 之后紧跟 `sqlite.base` / `web.form.query`，两者都正确判 FAIL。

> ⚠️ 因此**前四轮 sweep 的"残差 12 项"只对第 1..417 个模块成立**，需要按新 harness 重测。

**第十一轮发现（工具链残留）：sweep 会把 adb server 与 `.old` 备份越堆越多。**
上面那条"常驻 adb server"不只是判定污染源，它还在**磁盘和进程表上留垃圾**，实测：
- `%LOCALAPPDATA%\aardio\std\adb\20240409\` 下堆了 **93 个 `adb.exe.*.old`**（每个 5.8MB，约 540MB）
  + 6 个 `AdbWinApi.dll.*.old` + 2 个 `AdbWinUsbApi.dll.*.old`；
- 同时有 **37 个孤儿 `adb.exe` 进程**在跑（命令行全是 `adb -L tcp:<port> fork-server server
  --reply-fd N`，父进程**全部已消失**）。

两个残留是**同一个因果链**：官方 `builtin/io.aardio:76-89` 在写 `io.appData` 资源时的逻辑是
「写失败 → 把旧文件改名成 `<name>.<tick>.old` → 重写 → `remove(old)`」。而
`process.adb` 导入时会①把 adb.exe 重新写进 std 目录、②拉起一个**常驻** adb server
（官方文档明说"进程退出时不会退出常驻服务端"）。常驻 server 占着 adb.exe 的映像 →
写不进去 → 改名备份 → 备份也被占着 → `remove(old)` 失败 → **`.old` 永久残留**；
server 自己则随父进程退出变成孤儿。两者互相喂养：下一次 sweep 再导入一次，又多一份
`.old` + 一个孤儿。

修法（**测试工具链层，不改官方库、不改引擎**）：
- `tests/module_sweep.ps1` 收尾：按**命令行 + 镜像路径**回收「父进程已死、且镜像位于
  aardio std 目录」的 adb server（只碰孤儿，活的常驻 server 不动），等 800ms 让文件句柄
  释放，再删掉 std 目录下的 `*.old`（**不碰**活的 `adb.exe` / `AdbWin*.dll`）；
- `tests/run_vm_tests.ps1` 收尾：原来只按 `$srvProc.Id` 杀 `_srv.py` 进程树，
  脚本自身被 Ctrl+C/崩溃打断时就会留下孤儿监听器 → 改为**再按命令行**扫一遍
  `webroot\_srv\.py` 全量回收。

实测（清理后连跑 3 轮只含 `process.adb*` 的定向 sweep）：
**adb 进程 = 0、`.old` 残留 = 0**（幂等），目录只剩 4 个正常文件
（`aardio.adb.dex` / `adb.exe` / `AdbWinApi.dll` / `AdbWinUsbApi.dll`）。
`run_vm_tests.ps1` 回归 **90 passed / 0 failed**，跑完 `_srv.py` 残留 = 0。

> 📌 另注：`%LOCALAPPDATA%\aardio\` 下另外 120 个 `*.old` 是 WebView2/Chromium 的
> `LOG.old`（浏览器日志轮转，`webview2\user-data\...`），**与本项目无关，属正常运行时产物**，
> 不应清理。

**结论**：标准库的"能不能 import"已经 100%（命名空间 54/54；第四轮 969 模块 **957 通过**，
12 个残差全部可枚举且全是 32 位 DLL 物理边界或官方自身缺陷）。

---

## 4. 发布器缺口

> ✅ **第二轮已修**：P-1 / P-2 / P-3 三条都收口了。发布回归现在**全绿**：
> `PASS entry=main` / `PASS lib:mylib-embedded` / `PASS res:hello from embedded resource` /
> `PASS blob:776` / `PASS memdll:true` / `PASS memdll.add(100,23):123` /
> `PASS memdll.hello:hello-from-mem-dll`。

| # | 缺口 | 第一轮证据 | 状态 |
|---|---|---|---|
| P-1 | 发布产物读不到嵌入资源 | `tests/run_publish_tests.ps1` 退出码 2，`res:null` / `blob:0` | ✅ 已修（见 P0-1：查找下沉到 `io.file`） |
| P-2 | 发布运行时与开发运行时不同源 | `nodio-stub.exe`（23:41）vs `nodio.exe`（00:16→00:48）；`publish/publish.cpp` 优先用 stub 模板 | ✅ 已修：`apps/nodio/CMakeLists.txt` 加 `add_dependencies(nodio nodio-stub)`，stub 永远是同一批源码的产物 |
| P-3 | 台账"publisher 100% closed"与实测不符 | `docs/roadmap.md` 第 130 行 | ✅ 与实测一致（发布回归全绿） |

仍可用的部分（两轮都验证过）：内存 DLL 自加载（`memdll:true`）、import 闭包嵌入
（`lib:mylib-embedded`）、字节安全载荷、PE 图标/版本/manifest 戳记。

---

## 5. IDE 缺口

> **口径说明（2026-10-24 第三十四轮重写）**：本节上一版写于**第五轮（2026-10-20）**，
> 此后第 12～34 轮落地了大量能力（着色 / 查找替换 / 书签 / 折叠 / 导航 / 断点调试 /
> 设计器闭环 / 批量对齐 / 差异对比 / 监视 / 格式化 / 全屏 / 插件视图…），
> 原文那份「官方 IDE 有、Nodio 还没有」的清单**已大面积过期**。
>
> 本节按**当前工作树实测**重写。证据三源：
> ① **菜单即能力面** —— `ide/nodio/app/shell.nodio:1061-1250` 的菜单定义
>    （官方 IDE 的 28 菜单 / 38 命令 / 99 字符串表逐条对过）；
> ② **协议服务端的方法分发** —— `apps/nodio-lsp/main.cpp:1490-1596`、
>    `apps/nodio-dap/main.cpp:175-354`；
> ③ **组件层实现** —— `lib/nodio/ui/editor.nodio`、`lib/nodio/designer/*`。

### 5.1 编辑能力

| 官方能力 | Nodio 现状 | 证据 |
|---|---|---|
| 语法着色 / 主题 | ✅ 已通：本地词法器着色（keyword / string / number / comment / func 五色） | `editor.nodio:246-250, 2732, 2847` |
| 智能提示 / 自动完成 | ✅ 已通：Ctrl+Space → LSP `completion` → 候选浮层 → 插入 | `shell.nodio:1113`；§1.16 |
| 查找 / 查找下一个 / 查找上一个 / 替换 / 全部替换 | ✅ 已通（Ctrl+F / Ctrl+H；另有「在工程中搜索」Ctrl+Shift+F） | `shell.nodio:1109-1110, 1160-1170` |
| 书签（插入 / 下一 / 上一 / 清除全部） | ✅ 已通（Ctrl+F2 / F2 / Shift+F2） | `shell.nodio:1152-1155`；`editor.nodio:3032-3045` |
| 代码折叠（折叠 / 展开 / 全部折叠 / 全部展开） | ✅ 已通（行号栏折叠标记 + 真正收起正文） | `shell.nodio:1134-1142`；`editor.nodio:1943-1997`；§1.15 |
| 注释切换（Ctrl+/） | ✅ 已通（单行 `//` 与块注释都处理） | `shell.nodio:1132`；`editor.nodio:962-1016` |
| 行号开关 / 缩进 / 反缩进 / Tab 智能缩进 | ✅ 已通（Tab 缩进、Shift+Tab 反缩进；「显示行号」在「视图」菜单） | `shell.nodio:1130-1131, 1202` |
| 跳转到指定行（Ctrl+G） | ✅ 已通 | `shell.nodio:1111, 1150` |
| 代码补全 / 参数提示 / 代码片段展开 | ✅ 已通（Ctrl+Space / 悬浮签名提示 / Tab 片段自动展开并定位 `$0`） | `editor.nodio:2615-2727` |
| 格式化（Shift+Alt+F） | ✅ 已通（按括号层级缩进，整次排版封成单一事务） | `shell.nodio:1125-1127` |
| 全屏模式（Shift+F11） | ✅ 已通（记录窗口几何，二次触发无损恢复） | `shell.nodio:1211` |
| 快速打开文件（Ctrl+P） | ✅ 已通（工程文件模糊匹配 + 定位载入页签） | `shell.nodio:1064` |
| **打印 / 打印预览 / 快速打印 / 打印设置** | ❌ **未做** —— 菜单无任何打印入口，`lib/nodio/` 全库无打印管线 | — |
| 以 RTF / HTML 格式复制文档 | ✅ 已通（RTF=整篇 richedit 原生 copy + 全文着色补偿；HTML=token 流直出 CF_HTML + CF_UNICODETEXT 兜底；复制后选区还原） | `shell.nodio:1146-1152`；`app/editor.nodio:21-22`；`editor.nodio:195-239, 938-1095`；§1.52 |
| **官方 intellisense 词库**（`config/intellisense/kernel.txt` 150KB + `winform.txt` 58KB + `unimportant.txt` 5KB） | ✅ 已接入并**可用**：三形态解析（带分隔线 / 无分隔线+注释头 / 重定向）941 词条；**限定符补全**（写 `winform.` → 234 条窗体成员；写 `string.` → 106 条）+ 悬停出官方中文说明 | `lib/nodio/intellisense.nodio`；`editor.nodio` 限定符前缀；§1.59 |

### 5.2 工程 / 窗口面

| 官方能力 | Nodio 现状 | 证据 |
|---|---|---|
| 工程资源管理器（工程树） | ✅ 已通（增删改、文件操作、右键菜单、真全文搜索） | `shell.nodio:1080-1090`；§1.13 |
| **类视图 / 资源视图** | ❌ **未做** —— 官方另有独立的类树与资源树 | — |
| 标准库面板 / 范例面板 | ✅ 已通（标准库扫描 + 官方 `config/lang/lib.txt` 中文说明） | `ide/nodio/app/library.nodio`；`shell.nodio:1175-1176` |
| **模板面板** | ❌ **未做** | — |
| 最近使用的文档 | ✅ 已通（「文件」菜单最近文件动态段，cfg `recent.files` 持久化；§1.48） | `app/settings.nodio`；`app/shell.nodio` |
| **开始页** | ❌ **未做** —— Nodio 直接开工程的启动文件 | `ide/nodio/main.nodio:2180` |
| 文档窗口管理（页签 / 批量关闭 / 右键菜单） | ✅ 已通 | `shell.nodio:1071-1076`；「企业生产可用轮次」 |
| **垂直-水平拆分窗口组 / 选项卡组（新建、移动）** | ❌ **未做** —— 只有「双栏对比」（diff 视图），不是可自由拆分的窗口组 | `shell.nodio:1198` |
| 外部工具自定义（命令 / 键 / 说明） | ✅ 已通（工程菜单：资源管理器定位 / 终端 / 代码行统计 / 复制路径） | `shell.nodio:1086-1088` |
| 插件视图机制（`pluginViewCreate` / `pluginViewShow`） | ✅ 已通（第三十四轮：右侧栏页签宿主 + `loadcode` + `parent=...` 注入 + 内置「助手」页） | `main.nodio:2293-2315`；`dock.nodio:164+`；`ide/nodio/plugins/tool.nodio`；回归 `tests/ide_plugin_check.ps1` |

### 5.3 协议能力

**LSP**（`apps/nodio-lsp/main.cpp:1490-1596`）

| 已实现 | 未实现 |
|---|---|
| `textDocument/didOpen` / `didChange` / `didClose`、`publishDiagnostics`、`completion`、`hover`、`documentSymbol`、`documentHighlight`、`definition`、`references`、`rename`、`workspace/symbol` | `textDocument/signatureHelp`、`formatting`、`codeAction`、`semanticTokens`、`foldingRange`、`selectionRange`、`typeDefinition`、`implementation`、`declaration`、`prepareRename` |

> 注：**着色**与**参数提示**在 IDE 侧是**本地实现**（`editor.nodio` 自带词法器 + 本地签名推断），
> 不走 LSP；补全与导航才走服务端。

**DAP**（`apps/nodio-dap/main.cpp:175-354`）

| 已实现 | 未实现 |
|---|---|
| `initialize`、`launch` / `attach`、`setBreakpoints`、`threads`、`stackTrace`、`scopes`、`variables`、**`evaluate`（表达式求值，在当前暂停帧的环境里求值 —— 能看到局部变量）**、`continue`、`next` / `stepIn` / `stepOut`、`pause`、`configurationDone`、`disconnect` / `terminate` | `setVariable`、`goto`、`exceptionInfo`、`loadedSources` |

> 注：**监视表达式**是 IDE 本地求值（`ide/nodio/main.nodio:1699-1732`），不是 DAP `evaluate`。

### 5.4 官方 IDE 命令表逐条对照

第三十二～三十四轮已把官方命令面接上：命令表**逐条注册**进共享注册表
（`ide/nodio/app/ideCommand.nodio:40-78`，39 个条目），其中 **35 条**经 `main.nodio`
注入的 impl 表接真实实现（`ide/nodio/main.nodio:2194-2315`）；跨进程经
`ide.command.join()` + `send` 驱动（回归 `tests/ide_command_check.ps1`，真实双进程）。

**仍未接真实实现的 4 条**：`processList`、`createThread`、`updateVersion`、
`getProjectIcon` —— 注册存在，调用返回 null。

### 5.5 设置面

**已落地**（第三十五轮，`ide/nodio/app/settings.nodio` + `tests/ide_settings_check.ps1` 36 项断言）：

| 设置项 | 生效方式 | 说明 |
|---|---|---|
| 主题（深色 / 浅色） | **重启后生效** | 各 chrome 组件是**构造期**读一次 `theme()` 把配色存进自己字段（`nodio/ui/theme.nodio`），运行期再切要重建整排 chrome；官方 aardio 同样是换主题重开 IDE。偏好存 `%APPDATA%\Nodio\ide.cfg`，启动时应用（`main.nodio` 在 `import app.shell` **之前**调 `themeApply()`） |
| 编辑器字体大小（6-24 pt） | **即时生效** | 新增 `nodio.ui.editor.setFontSize(pt)`：重设 richedit 字体 → 重新量行高（`EM_POSFROMCHAR`）→ 行号栏字体跟着行高走 → 浮层画板字号同步 → 重新布局 |
| 外部工具（最多 9 个） | **即时生效** | 名称 / 命令 / 参数（`%f` = 当前文档，`%p` = 工程目录）；「设置」菜单增删、右栏「工具」面板分组列出、**Ctrl+1..9** 触发（编辑器组件新增 `onHotkey(vk)` 钩子，组件本身不认识「外部工具」） |

配置与官方命令面**共用** `%APPDATA%\Nodio\ide.cfg` —— 外部工具/插件用
`ide.command.getConfig("theme")` 就能读到 IDE 的设置。

**仍是空占位的**：

| 位置 | 条目 | 现状 |
|---|---|---|
| `shell.nodio` 「工程」→ **工程属性** | `setStatus("工程属性")` 空转 |
| `shell.nodio` 右栏「AI」页签 | 只是一个空 `richedit`，**没有任何 AI 接入**（官方参考图 `docs/ref/aar-ide/06-ai-assistant-outline-output.png` 是有 AI 助手的）。接哪家模型、API key 怎么存，需要先定，不能凭空造 |

---

## 6. 窗体设计器缺口

契约见 `docs/ide-components.md`（组件层已写好）。**当前状态（第五十六轮实测）**：

| 官方设计器能力 | Nodio 现状 |
|---|---|
| 8 手柄缩放 / 拖动 | ✅ 已通：选中框 + 8 手柄画在底板上（手柄放组件外侧、框外扩 1px），拖拽用屏幕坐标 |
| DSG 解析（`fromDSG`） | ✅ 已通：按 `;` 切分 + 奇数引号合并，值类型保持（number/boolean/string） |
| 属性格双向改写 | ✅ 已通：改值 → 写回控件属性 + 几何（left/top/width/height → 重排）；属性格带**分组带**（VS WinForms 分类，见 §1.22） |
| 工具箱 | ✅ 自绘（分类可折叠 + 14 个内置控件），组件走 `registry` 注册 |
| 网格 / 边界线 / 对齐 / 缩放 / 改窗体设置 | ✅ 已通：「设计」菜单有 显示网格线 / 对齐网格 / 控件吸附对齐 / 显示边距线 / 窗体设置 / 设计器缩放 25%~400%（`shell.nodio:1216-1224`） |
| 事件代码生成 | ✅ 已通：双击控件 → 切代码视图并生成 `winform.<name>.<event> = function(id,event){}` 桩；源码里已有同名处理函数只跳转、不重复生成（`main.nodio:1855`） |
| 设计 ↔ 源码视图切换 | ✅ 已通（`switchActiveForm` 语义：开设计器 = 从当前文档载入 DSG 块；关设计器 = 把当前设计写回文档的 DSG 块，只动编辑器缓冲） |
| 自定义控件替身映射（`ide.customCtrl`） | ✅ 已通：官方 `cls→drawAs` 映射表照抄进 `nodio.designer.registry.customCtrl`（10 条），DSG 仍写真实类名 |
| 批量多选与批量对齐 | ✅ 已通（Ctrl/Shift 多选；左/右/顶/底/水平/垂直居中/等宽/等高 8 条批处理指令，`shell.nodio:1226-1233`） |
| 设计器撤销 / 重做 | ✅ 已通（画布独立 `undoStack`/`redoStack`，上限 50 步，Ctrl+Z / Ctrl+Y） |
| 缩放与控件间磁性对齐吸附线 | ✅ 已通（25%~400% 缩放 + 5 逻辑像素内智能吸附 + 橙色辅助线） |
| 工具箱拖拽落点 + 自动避让 | ✅ 已通（第五十六轮：拖到哪落在哪 + 落点预览框；单击添加自动避开已有控件，不再一律落在 (20,20)） |

---

## 7. 物理边界（不可支持，需在交付说明里写清）

1. **32 位专用原生 DLL**（`av.*`、`gdip.feather`、`com.autoComplete`、`zlibwapi.dll`、
   `python27/38.dll`（官方 lib 树里那份是 32 位；x64 用的 64 位 Python 3.8 由 overlay
   `lib/py3/.res/DLLs/` 提供）、`qrencode.dll`、`web.layout` 的 `HTMLayout.dll` 等）：
   x64 进程装不了。官方同样有 32/64 两套分发。Nodio 保留 x86 构建承接
   （`build-x86/Release/nodio.exe` 存在）。**x86 轮次的 969 模块 sweep 已跑完**
   （第七轮，用修正后的 harness，见 §3）。实测 x86 上
   `import py2; import py2.dict;` / `import py3; import py3.dict;` /
   `import web.layout; import web.layout.event;` **全部通过** —— 这些在 x64 上的失败
   **确认是 32 位物理边界，不是引擎缺陷**。
2. **原版 `=aardio` 闭源字节码**（如官方 `lib/ide/aifix.aardio`，97KB）：原版 opcode 编码
   未复原，**无法执行**。第二轮把失败原因从误导性的"语法错误"改成了清晰的
   `格式错误: 不支持的字节码格式 (=aardio   )`（E-5），第九轮再把容器解析器移植进引擎
   （E-14），诊断进一步细化为
   `格式错误: 官方预编译模块 (=aardio) 暂不可执行 — 已解析出 97 个对象 / 8734 条指令 / 2173 个常量（最大嵌套 4 层），但 Nodio 尚未实现该字节码的解释器`，
   但**可执行性这一项仍是物理边界**（第九轮已定位官方 VM 主分派 `FUN_004EC700`
   与记录发射器 `FUN_004F5B20`，确认它是「AST 树遍历 + 记录流」并导出 26 类运算符映射
   与 83 个记录发射点，仍缺 64 种记录各自的语义，见 `docs/aardio-bytecode.md` §6）。
3. **官方 IDE 的 MFC 外壳原生特性**（停靠窗口浮动、工具栏/菜单自定义、打印管线）：
   Nodio 走自托管路线，只能自研等价物（能力清单见 §5）。

---

## 8. 建议修复顺序

> **第二轮执行结果（2026-10-20）**：1～5 项**已全部完成并复测**，见下表。
> 剩下的是仍未开工的 IDE / 设计器 / 物理边界类工作。

| 顺序 | 事项 | 状态 |
|---|---|---|
| 1 | 把仓库恢复可编译（P0-2） | ✅ 已修，x64/x86 均构建通过 |
| 2 | 修 P0-3（错误路径收尾）、P0-1（内嵌载荷查找下沉到 `io.file`） | ✅ 已修，发布回归转绿 |
| 3 | 把 `nodio-stub` 纳入同一构建产物 | ✅ 已修（`add_dependencies(nodio nodio-stub)`） |
| 4 | E-1 / E-2 语言语义缺陷 | ✅ 已修（E-3 / E-4 改夹具，E-8 判为官方限制） |
| 5 | E-5 / E-6 / E-7 修原生面与错误信息 | ✅ 已修（E-7 用 overlay 补 `metaProperty`） |
| 6 | 用 x86 构建重跑模块 sweep，把 32 位 DLL 失败从真缺陷里剥离 | ✅ **已做完**（第七轮）：x64 与 x86 两轮都用**修正后的** harness 跑完 969 模块 —— **x64 `ok=897` / 非 OK 72**，**x86 `ok=931` / 非 OK 38**；x86 的失败集是 x64 的**严格子集**，差集正好 34 项且全是 `无法加载 DLL`（32 位 DLL 物理边界）。两套架构上**引擎侧零残留缺陷**，全部可归因（34 物理边界 / 25 官方「先导入父命名空间」契约 / 6 下载安装型超时 / 3 官方库自身缺依赖 / 2 `=aardio` 闭源字节码 / 1 伪模块 `win.#DEFINE`） |
| 7 | IDE：先做"编辑器三件套"（着色 / 查找替换 / 书签），再接 LSP 剩余方法；设计器按 `docs/ide-components.md` 的契约补 8 手柄 + `fromDSG` + 属性格回写 | 🟡 **设计器三项本轮已通**（8 手柄 / `fromDSG` / 属性格回写 + DSG 往返，见 §1.10）；**编辑器三件套** ✅ 着色 / 查找替换已通（§1.12），**书签**仍未做 |
| 8 | **import 自动预载父命名空间**与官方契约不一致（§3 末），官方"禁止直导"自检失效 | ✅ **已修**（第七轮，§2 E-12）：删掉 `engine/import.cpp` 的预载块，5 个官方 guard 按**官方原文**报错，回归 `tests/import_contract_check.ps1`（x64/x86 全绿） |
| 9 | **DPI 缩放对齐**（官方控件逻辑像素 vs 自绘层物理像素） | ✅ **已修**（第七轮，新增 `nodio.ui.dpi`；见 §1.11.6） |

**本轮新增的必修项**（优先级等同 P0，已插队做完）：

1. **P0-4 GC 跨线程堆损坏** —— 任何多线程脚本都会随机崩，必须先修（见 §1.4）。
2. **P0-5 字符串表示归一** —— 官方字符串即字节数组，不归一则 `==`/`++`/比较全错（见 §1.4）。
3. **P1-6 调试器断点越界** —— 断点会停在官方库里，调试器基本不可用（见 §1.4）。
4. ~~**E-9 `web.layout.event` 断言失败**~~ —— 第二轮 sweep 中已不复现。
5. **x86 轮次的 969 模块 sweep** —— 把 32 位 DLL 失败从真缺陷里剥离。
---

## 附：证据来源与工具边界

**官方 aardio 的能力面**来自三层（优先级从高到低，与 `docs/official-ide-reference.md` 一致）：

1. 官方库**源码**（`D:\aardio\lib` 869 个 `.aardio`、`D:\aardio\extensions`）；
2. 官方 `config` 数据（`themes/*.xml`、`intellisense/*.txt`、`USR.CFG`、`SYS.CFG`）；
3. `aardio.exe` **二进制**（本文用 REA 工具 + 自写 PE 解析器读资源）。

**REA（rea-agents 6.4.0，Ghidra 12.1.4）** 在本次审计中：

| REA 操作 | 目标 | 结果 |
|---|---|---|
| `inspect-pe-resources` | `aardio.exe` | ✅ 1904 条目 / 943 个资源，coverage=complete：MENU 28、DIALOG 28、STRING 99、ACCELERATOR 2、TOOLBAR 10、PNG 529、ICON 61、STYLE_XML 5、SETTINGS 1、AAUTEMPLATE 1、MANIFEST 4、VERSION 1 |
| `inspect-artifact` | `aardio.exe` | ✅ root_format=pe，sha256 `e3b6032df5bcfba52cb92633b16d2e519888ef46bdb983082c53f88bf5cc9278`，evidence `ev_e99886dd1c1b6cdfcdd6d2e2c25e691422807a54f9fbe4ce23e6d4732bbee8ea` |
| `analyze --provider ghidra` | `aardio.exe` | ❌ **两次 `provider_timeout`**：默认 330 s、以及把 `REA_GHIDRA_STARTUP_TIMEOUT_MS` 提到 1,800,000 后仍在 `list_documents` 阶段超时（6.9 MB MFC 二进制的 Ghidra 全量自动分析超出该 provider 的时限） |
| `doctor` | — | ✅ Ghidra 12.1.4 探测全绿（非安装问题，是目标体量与 provider 时限问题） |

**因此本报告没有引用任何 Ghidra 反编译结论**——那部分证据本轮没拿到，官方 IDE 的能力清单
改由「官方源码 + 官方 config + PE 资源普查 + PE 字符串表解码」三方交叉得出。
MENU 资源的解码脚本在 `tests/pe_ide_surface.py`（MENU 弹窗项的子模板是菜单索引而不是
子菜单偏移，本脚本对这部分解析不完整，**结论只取自 STRINGTABLE 与 RT_* 计数**）。

---

## 附：需要更新的台账行（`docs/roadmap.md`）

| 行 | 现值 | 实测 |
|---|---|---|
| 104-105 命名空间导入 | 45/54 (x64)、52/54 (x86) | **54/54 (x64)**（第二轮复测：命名空间 sweep `ok=54 fail=0`；第七轮复测仍 54/54） |
| 128 engine 96% | — | P0-1/P0-2/P0-3/P0-4/P0-5 均已修；**第七轮** VM 回归 **89 passed / 0 failed / 0 skipped (x64 与 x86 同)**；E-11（raw 回调释放）/ E-12（import 契约）/ E-13（系统 API 表归属）已修；**第九轮** VM 回归 **90 passed / 0 failed / 0 skipped**，新增 E-14（官方预编译块容器解析器进引擎） |
| 130 publisher 100% | closed | 与实测一致（第七轮：发布回归 **全绿**，`res:`/`blob:`/`memdll:` 全 PASS） |
| 131 IDE 20% | — | 大致相当，但 LSP/DAP 已比台账描述完整 |
| 132 designer 0% | not started | 已有 5 个组件初版；**第五轮补齐** 8 手柄缩放 / `fromDSG` 解析 / 属性格回写（含几何）/ DSG 往返 / 自绘工具箱（见 §1.10）；**第七轮** 设计坐标改逻辑像素（DPI 对齐，见 §1.11.6）；**第八轮** 新增 `nodio.gui` 控件体系（移植 Zan.Gui 的 Theme→Style→StyleBox→Widget），自定义组件既能给设计器用（`nodio.designer.registry`），也能在 IDE 面板里用（见 §1.11.7） |
| 139-141 `=aardio` 报格式错误 | 已完成 | **现已属实**（E-5 已修，第九轮细化为 E-14：报 `格式错误: 官方预编译模块 (=aardio) 暂不可执行 — 已解析出 97 个对象 / 8734 条指令 / 2173 个常量`）；**第七轮**又把 `=aardio` 的**格式**完全复原（15 字节头 / chunk 文法 / 常量池 / 32 位 RISC 指令编码，`tools/aardio_bytecode.py --verify` 字节级回归通过，见 `docs/aardio-bytecode.md`）；**第九轮**再把节点优先级表、官方内建分派表导出并还原 10/61 条 opcode 语义 + 21/61 条校验约束；**第十一轮**终于把**运行期解释器本体**定位（`FUN_004C4BC0`，7263 字节，`switch(word & 0x3F)`，61 个 case 齐全）并**逐条读出 61 种记录的语义**（含槽位布局 / 8 种类型标签 / 18 位跳转位移），语义不再是未知；剩下的是把记录流翻译回 Nodio 的 `PNode`（工程量缺口，非物理边界），见 `docs/aardio-bytecode.md` §7 |
| — | — | **第七轮已结清**：import 自动预载父命名空间（E-12，已修 + 回归 `tests/import_contract_check.ps1`）；x86 轮次的 969 模块 sweep（已跑，见 §3）；另新发现并修掉 **sweep 工具链自身的跨模块污染缺陷**（§3 末，前四轮「残差 12 项」只覆盖前 417 个模块）。剩余待办：IDE / 设计器（见 §8）、`=aardio` 记录流→`PNode` 翻译器（语义已全拿到，见 §2 E-14 第十一轮） |

### 1.12 第七轮：编辑器组件 + 文档闭环（标准可用 IDE）

目标：把 IDE 从「骨架能跑」推进到「标准可用」—— 编辑器本体、文档生命周期、
命令面全部接真实实现，消灭菜单里的「未实现」占位。

#### 1.12.1 本轮新增的标准库组件

| 组件 | 文件 | 作用 |
|---|---|---|
| `nodio.ui.editor` | `lib/nodio/ui/editor.aardio` | 编辑器本体：行号栏、aardio 语法着色、当前行高亮、行列号回调、Tab/Shift+Tab 缩进、Ctrl+/ 注释、查找替换浮条、撤销/重做、脏标记 |
| `nodio.ui.filedlg` | `lib/nodio/ui/filedlg.aardio` | 系统「打开/另存为」对话框（手写 Windows 布局的 `OPENFILENAMEW`） |
| `nodio.ui.prompt` | `lib/nodio/ui/prompt.aardio` | 通用输入提示条（转到行等） |

#### 1.12.2 本轮查清并修掉的坑（都是实测）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 官方「打开文件」对话框**秒退、返回 null** | 官方 `fsys.dlg` 的 `OPENFILENAME` 在 Nodio 上是 144 字节，Windows 的 `OPENFILENAMEW` 是 152 字节（`hwndOwner` 是 HWND=8 字节，官方声明成 INT=4 字节）→ 字段整体错位 8 字节 | `nodio.ui.filedlg` 手写按 Windows 真实偏移排好的结构体 |
| 2 | 文件对话框的过滤器下拉框**显示乱码** | aardio 的 `'\0'` 是**字节转义**，带它的字面量是字节串；跟普通字符串拼接会把整串降级成字节串（中文被 UTF-8 编码成字节），再做 UTF-16 转换就成乱码 | NUL 改用 `'\u0000'`（非字节转义） |
| 3 | 编辑器**撤销不生效** | richedit 的撤销栈只收键盘输入，程序化的 `EM_REPLACESEL`（缩进/注释/替换）不进栈；且 RichEdit 不实现 `EM_CANUNDO` | 组件自实现文本快照栈（`WM_CHAR`/Delete/粘贴/剪切前存快照，程序改动前存快照） |
| 4 | 文档「刚打开就是已修改」 | 脏标记拿 LF 原文跟编辑器里的 CRLF 文本比 → 必然不同 | 脏标记改成**推导式**（当前文本 vs 上次保存的文本），文档模型统一用 CRLF 形态，保存时再按原换行符还原 |
| 5 | 查找浮条**只剩输入框，浮条本体空白** | 官方 `plus` 控件默认把**父窗口背景**整块拷进自己的画板，而浮条浮在 richedit 上方 → 自绘内容被 richedit 盖住 | 建板时 `transparent=true` |
| 6 | 查找浮条**只画出左边的标签，按钮全没了** | ① 按钮矩形用的是**父窗口绝对坐标**，而画板绘制的原点在客户区 → 按钮被画到画板外；② `for(k,v in 表)` 只遍历表的数组部分，字符串键的表遍历不到 | 按钮矩形改客户区相对坐标；改用数组 + 索引循环 |
| 7 | 浮条重绘后**输入框被画板盖住** | 重绘会把画板提到 z 序最前 | 先重绘画板、再摆输入框（并用 `::InvalidateRect`+`::UpdateWindow` 整块重绘，官方 ctrl 没有可用的 `redraw` 成员） |
| 8 | 浮条/提示条的按钮**点不动** | 同 6（命中测试用的也是绝对坐标 + 同样的遍历坑） | 同 6 |
| 9 | 官方 `plus` 的 `redraw` 成员调用抛「非函数值」 | 该成员在 Nodio 的控件基类上不可用 | 用引擎 sysapi 的 `::InvalidateRect` + `::UpdateWindow` |

#### 1.12.3 IDE 层的接线（`ide/nodio/`）

- `app/shell.nodio`：创建 `nodio.ui.editor`（复用 DSG 里的 richedit）、导出给
  `main.nodio`；文档模型改成 `path -> {文本; 保存时的文本; 换行符}`，
  脏标记由内容推导；页签标题带 `*`；菜单与快捷键全部接真实命令
  （新建/打开/保存/另存为/全部保存/关闭标签页/剪切/复制/粘贴/全选/撤销/重做/
  查找/替换/转到行/增加缩进/减少缩进/注释切换），**「未实现」占位清零**。
- `app/editor.nodio`：从「直接操作 richedit」改成「操作编辑器组件」的门面
  （含右键菜单：剪切/复制/粘贴/查找/替换/缩进/注释/全选）。
- `main.nodio`：文档闭环（新建/打开/另存为/保存/脏标记/关闭时提示
  保存-不保存-取消）、状态栏行列号、大纲随内容刷新（按键只标脏，
  300ms 心跳里统一重扫）。

#### 1.12.4 回归

- `tests/editor_probe.aardio`（组件行为）：撤销/重做/脏标记推导/查找替换/缩进注释 全绿
- `tests/ide_smoke.ps1`：新增 `DOC-*` 断言（脏标记/行列号/行号栏/缩进/注释/撤销）
- `tests/ide_smoke.ps1` / `tests/ide_titlebar.ps1` 补 **UTF-8 BOM**
  （Windows PowerShell 5.1 按 ANSI 读无 BOM 的中文脚本 → 解析错误）
- VM 回归 **90 passed / 0 failed / 0 skipped**

### 1.13 第八轮：工程工作流闭环（工程树文件操作 + 真全文搜索）

目标：把 IDE 的「工程」这一面补成标准 IDE 的样子 —— 工程树能新建/重命名/删除，
左栏「搜索」页是真能用的全文搜索（关键字 → 结果列表 → 双击跳转）。

#### 1.13.1 本轮新增的标准库组件

| 组件 | 文件 | 作用 |
|---|---|---|
| `nodio.ui.searchPanel` | `lib/nodio/ui/searchPanel.aardio` | 左栏「搜索」页：关键字输入条 + 结果表（文件/行/内容）+ 双击跳转 |

`nodio.ui.projectTree` 补了 `markDir` 的 `isDir` 标记、`isDir(node)`、`dirOf(node)`
（节点所在目录 —— 「新建文件」默认落在父目录）。

#### 1.13.2 本轮查清并修掉的坑（都是实测）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | **Ctrl+S / Ctrl+N / Ctrl+O / Ctrl+W / Ctrl+F9 一个都没注册上** | `winform.reghotkey(func, mod, vk)` 把 `mod` 直接传给 Win32 `RegisterHotKey`，修饰键掩码是 `MOD_ALT=1 / MOD_CONTROL=2 / MOD_SHIFT=4 / MOD_WIN=8`；代码里写的是 `0x8`（Win 键）和 `0x11`（`0x10` 压根不是合法标志位）→ `RegisterHotKey` 直接失败 | 改成 `MOD_CTRL=0x2`、`MOD_CTRL+MOD_SHIFT=0x6`，并补上 Ctrl+F / Ctrl+H / Ctrl+G / Ctrl+Shift+F |
| 2 | 工程树按 `.nproj` 清单平铺，**新建的目录根本看不见** | 树的内容 = 工程清单的文件列表，清单里没有目录条目 | 树改成**实时文件系统视图**（目录+文件都扫磁盘），清单里「有清单但磁盘上没有」的文件再补进去；扫描有深度（4 层）和节点数（1200）上限 |
| 3 | 新建/重命名/删除后**发布包不会带上新文件** | 磁盘改了，`.nproj` 清单没改 | `app/project.nodio` 补清单维护（`addFile/removeFile/renameFile/renameDir/removeDir/save`），磁盘操作后同步写回（只写 `.nproj`，`.aproj` 是官方格式不动） |
| 4 | 删目录时 `io.remove` **删不掉非空目录** | `io.remove` 只删文件/空目录 | IDE 侧自己递归（`io.listFiles` + `io.remove`） |
| 5 | 搜索结果里**行号被截掉一位**（20 显示成 2） | 结果表列宽不够，`nodio.ui.list` 的单元格是「超长截断」绘制 | 「行」列给到 36 逻辑像素（放得下 3 位数） |
| 6 | 单引号串里的 `\a` 变成 **BEL 字节**，路径/文件名对不上 | aardio 单引号串按 C 规则处理转义（`\n` `\r` `\t` `\a` `\\` …），双引号串才是**原样串** | 单引号串里要一个反斜杠就写 `\\`；路径优先用双引号原样串（`"D:\Nodio"`） |
| 7 | `string.find` 当"有没有这个关键字"用 → **永远命中/永远不命中** | 官方 `string.find` 是**正则**（`"."` 是任意字符） | 一律用 `string.indexOf`（1-based，找不到返回 null） |
| 8 | 按 `\n` 切行后预览列**多一个不可见回车** | CRLF 文件每行末尾带 `\r` | `string.endsWith(line,'\r')` 判断后 `string.sub` 去掉（`#line` 是 UTF-8 字节数，与 `sub` 的位置参数一致） |

#### 1.13.3 IDE 层的接线（`ide/nodio/`）

- `app/shell.nodio`：左栏「搜索」页从「一个空的官方 edit」换成 `nodio.ui.searchPanel`；
  工程树右键菜单从「打开/复制路径」扩到「打开 / 复制路径 / 新建文件 / 新建目录 /
  重命名 / 删除 / 设为启动文件」；菜单加「工程 → 新建文件/新建目录」与
  「查找 → 在工程中搜索」（Ctrl+Shift+F）。
- `app/project.nodio`：工程清单维护（见上）+ `relPath`（绝对路径 → 工程内相对路径）。
- `main.nodio`：`projRefresh()` 重建工程树（目录+文件）并刷新搜索范围；
  五个文件操作（新建文件/新建目录/重命名/删除/设为启动文件）走
  `nodio.ui.prompt` 收名字、`win.msgbox` 确认删除（**走 `modalBusy` 闸门**，
  模态期间心跳会重入解释器）；重命名后页签跟着改（`shell.tabRename`），
  删除后关掉打开着的文档（`shell.tabCloseUnder`）。

#### 1.13.4 回归

- `tests/ide_smoke.ps1`：新增 `TREE-NODES` / `SEARCH-SCOPE` / `SEARCH-HITS` /
  `SEARCH-ROWS` / `PROJOPS-*` 断言（工程树、搜索、工程清单增删改）
- `tests/probe_iofs.aardio`：io API 探针（`createDir/exist/save/load/listFiles/
  rename/remove/getSize` + `fsys.isDir`），实测全部可用
- VM 回归 **90 passed / 0 failed / 0 skipped**；IDE 冒烟 `SMOKE OK`；标题栏 `TITLEBAR OK`
- 截图：`shot_search.png`（搜索页：关键字 + 结果表 + 状态栏"找到 309 处"）、
  `shot_tree.png`（工程树已是目录树：`app/` `components/` 折叠展开）
### 1.14 第九轮：LSP 诊断真正接进 IDE（波浪线 + 行号栏标记 + 问题列表跳转）

本轮把「LSP 诊断」从「只有一张问题表格」推进成**标准 IDE 的槽内标记**：
出错范围画波浪线、染错误色，行号栏画诊断圆点，问题列表双击跳到出错那一行。
过程中查出**两个根因级 bug**，都是「看起来像功能没做完，其实是链路根本没通」。

#### 1.14.1 根因一：官方 `richedit.setCharformat` 画不出波浪线

**现象**：`setCharformat({underline=true; underlineType=7})` 只画**单线**，
`underlineType` 写 1~8 读回恒为 1，官方 API 怎么试都出不来波浪线。

**根因**（三件事叠在一起）：

1. **结构体布局**：原生 RICHEDIT50W 用的 `CHARFORMAT2W` 真实布局是
   `cbSize@0 mask@4 effects@8 yHeight@12 yOffset@16 textColor@20 charSet@24
   pitchAndFamily@25 faceName[32]@26 pad@90 weight@92 spacing@94 backColor@96
   lcid@100 reserved@104 style@108 kerning@110 underlineType@112 animation@113
   revAuthor@114`（total=116）——**和 aardio 官方 `CHARFORMAT2` 类完全一致**
   （Wine 的 `include/richedit.h` 与 MS 头文件同构，可作权威依据）。
   此前按「MSDN 那种 crTextColor@12 / wUnderlineType@28」的布局发过去，
   整条消息无效——那是**查错了结构体**。
2. **掩码位**：要让控件认 `underlineType`，`mask` 必须带
   `CFM_UNDERLINETYPE = 0x800000`（0x00800000）。
   官方 `setCharformat` 的 `mask` **只加 `CFM_UNDERLINE(0x4)`，从不加这一位**
   → 控件按规范忽略该字段 → 永远只画单线。
   （注意 `0x8000000` 是 `CFM_CHARSET`，写错一位还会顺手把字体字符集清成 0。）
3. **枚举值**：`CFU_UNDERLINEWAVE = 8`（`CFU_UNDERLINEDOTTED=4`、
   `CFU_UNDERLINEDOUBLE=3`、`CFU_UNDERLINEHEAVYWAVE=0x0c`），
   不是 7；7 是 `CFU_UNDERLINEDASHDOTDOT`。

**另一个坑**（同源）：官方 `setCharformat` 用 `if(textColor)` 判断要不要设颜色，
而 **aardio 里 0 是真值**（只有 null/false 为假）→ 只传 `underline` 也会连带把
选区文字色刷成 0（黑）。官方 `getCharformat()` 读的还是 `SCF_DEFAULT`
（控件默认格式），**验选区格式必须用 `getSelCharformat()`**（`SCF_SELECTION`）。

**修法**：`nodio.ui.editor` 自己定义 `CHARFORMAT2` 结构体、
**直接发 `EM_SETCHARFORMAT`**，mask 完全自控：

```aardio
var cf = CHARFORMAT2();
cf.mask = CFM_UNDERLINE | CFM_UNDERLINETYPE | CFM_COLOR;
cf.effects = CFE_UNDERLINE;
cf.underlineType = 8;              // CFU_UNDERLINEWAVE
cf.textColor = 错误色;
::SendMessageByStruct(ctrl.hwnd, 0x444/*_EM_SETCHARFORMAT*/, 1/*_SCF_SELECTION*/, cf);
```

清标记时 `mask` 只带 `CFM_UNDERLINE|CFM_UNDERLINETYPE`（**不带 CFM_COLOR**），
所以清下划线不会碰到文字色 —— 官方 API 的「0 是真值」坑一并绕开。

> 自建结构体经 `::SendMessageByStruct` 发送时，**结构体必须含名为 `cbSize` 的字段**：
> `engine/ffi.cpp` 的 API 调用侧会自动把真实大小盖进 cbSize，控件也校验它；
> 不含 cbSize 的版本整条消息无效。

**实测**（`tests/probe_squiggle2.aardio` + 像素放大）：

| underlineType | 渲染 |
|---|---|
| 8 (WAVE) | 波浪线 ✅ |
| 0x0c (HEAVYWAVE) | 粗波浪线 ✅ |
| 4 (DOTTED) | 虚线 ✅ |
| 3 (DOUBLE) | 双线 ✅ |
| 官方 API | 单线（因为不加 CFM_UNDERLINETYPE）❌ |

#### 1.14.2 根因二：LSP / 调试器的可执行文件路径是错的

**现象**：IDE 里 LSP **从来没起来过**（问题列表永远是空的，状态栏不显示诊断数），
调试器也从来没起来过。

**根因**：`main.nodio` 里

```aardio
var NODIO_EXE = ..string.getenv("NODIO_EXE") : "nodio.exe";
var LSP_EXE   = NODIO_EXE ++ "-lsp";    // → "nodio.exe-lsp"  ← 不存在
var DAP_EXE   = NODIO_EXE ++ "-dap";    // → "nodio.exe-dap"  ← 不存在
```

`NODIO_EXE` 本身就带 `.exe`，拼出来是 `nodio.exe-lsp`。真正的可执行文件是
**同目录下的 `nodio-lsp.exe` / `nodio-dap.exe`**。

**修法**：从 `NODIO_EXE` 取目录再拼文件名：

```aardio
var NODIO_DIR = ..io.splitpath(NODIO_EXE).dir;
if(!NODIO_DIR) NODIO_DIR = "";
var LSP_EXE   = NODIO_DIR ++ "nodio-lsp.exe";
var DAP_EXE   = NODIO_DIR ++ "nodio-dap.exe";
```

**附带结论**：`nodio-lsp.exe` 在 IDE 进程被强杀时会**随 stdin 关闭自行退出**
（实测残留 0），不产生孤儿进程。

#### 1.14.3 本轮新增/修改

- `lib/nodio/ui/editor.aardio`
  - 新增 `CHARFORMAT2` 结构体 + `__CFM_*` / `__CFE_*` / `__CFU_*` 常量；
  - `__applyDiagMarks` 改为自建结构体直发 `EM_SETCHARFORMAT`（波浪线 + 错误色）；
  - 新增 `__setCharFmt()`（mask 自控，绕开官方 API 的两个坑）；
  - `setDiagnostics` 加**指纹判重**（诊断没变就不重刷）——
    IDE 心跳每 300ms 喂一次诊断，不判重的话编辑器每 0.3 秒全量重刷一次；
  - `__applyDiagMarks` 在「本来就没有标记」时直接返回（打字时每 250ms 都会走到这里）。
- `ide/nodio/main.nodio`
  - 修 `LSP_EXE` / `DAP_EXE` 路径（见 1.14.2）；
  - 演示模式新增 `NODIO_IDE_DEMO_FILE`（打开指定文件，走 `openFile` → LSP 会跟着起来）。
- `tests/editor_probe.aardio`：诊断断言改用 `getSelCharformat()` 口径，
  断言 `underlineType == 8` / 未标记行 `== 0` / 清除后 `== 0`。
- `tests/probe_squiggle2.aardio`：波浪线线型对照探针。
- `tests/ide_diag_sample.nodio`：端到端视觉回归用的**故意写错**的源码。

#### 1.14.4 端到端验证（截图复核）

`NODIO_IDE_DEMO_FILE=tests/ide_diag_sample.nodio` 跑起来后：

- 槽内**红色波浪线**（第 7 行 `var = = 3`）✅
- 行号栏**红色圆点**（第 7 行）✅
- 底部问题列表条目 `7 | 错误 | 语法错误: 意外的 = @ 行 7` ✅
- 状态栏 `诊断: 1 项 — 行 7: 语法错误: 意外的 = @ 行 7` ✅

#### 1.14.5 回归

- VM **90 passed / 0 failed / 0 skipped**
- IDE 冒烟 `SMOKE OK`；LSP 冒烟 `LSP-SMOKE-OK`

### 1.15 第十轮：代码折叠（行号栏折叠标记 + 真正收起正文）

标准 IDE 的基本项。实现路径一开始就走错了（先试自绘覆盖层），
最后靠**一个此前没被用过的字符格式位**做成了。

#### 1.15.1 折叠靠 `CFE_HIDDEN` 做，不用自绘覆盖层

原生 RICHEDIT50W 的 `CHARFORMAT2` 里 `dwEffects` 有个 **CFE_HIDDEN(0x100)** 位。
实测（`tests/probe_fold.aardio`）：

- 给一段文字打上 hidden 格式后，**隐藏的段落整个收起，连行高都没有**：
  6 行的文档隐藏第 3~4 行 → `lineCount` 变 4，画面上只剩 3 行；
- 隐藏的只是**显示属性**，文本本身没丢（`ctrl.text` 仍含被隐藏的行），
  保存出去的永远是完整文本；
- `EM_POSFROMCHAR` 对**可见行**返回的 y 是正确的（折叠区后面那一行的 y
  正好是它该在的那一行）→ 行号栏可以靠字符位置自己算行位置。

所以折叠 = 「给正文区打 hidden 格式」，比自绘覆盖层干净得多
（不用遮、不用处理鼠标穿透、不用管 z 序）。

#### 1.15.2 折叠后**不能再用官方的行号函数**（关键坑）

官方 `lineToChar` / `lineFromChar` / `lineRange` 在有隐藏行时**返回错的位置**：

| 折叠第 3~4 行后 | 实际含义 | 官方返回 |
|---|---|---|
| `lineCount` | 4（隐藏段不算行） | ✅ 对 |
| `lineToChar(1)` | 第 1 行 | 1 ✅ |
| `lineToChar(2)` | 第 2 行 | 9 ✅ |
| `lineToChar(3)` | **折叠后那一行**（原第 5 行） | **18 ❌**（还是隐藏行的起点） |
| `lineToChar(4)` | 末尾空行 | 44 ✅ |

`EM_LINEINDEX` 对「被隐藏的那一行」返回的是隐藏行自己的起点，
不是可见行的起点 —— 直接拿来定位就会跳到折叠区里面去。

**修法**：编辑器组件自己维护两张表：

- `lineStarts[i]` / `lineLens[i]`：**文档行 → 字符位置**（自己按 CRLF 切行算，
  CRLF 在 richedit 里只占 1 格）；
- `visLines[i]`：**可见的文档行号**（跳过被折叠隐藏的行）。

所有行定位一律走 `__lineStart` / `__lineEnd` / `__lineOfChar`，
官方那三个函数只在没有折叠时才会被调用（现在一处都不用了）。
行号栏也改成按 `visLines` 画 —— 否则折叠后行号会整体错位。

#### 1.15.3 折叠区识别

按 aardio 语法扫花括号配对：`{` 所在行是**折叠头**，
配对的 `}` 所在行是**正文末行**，中间 `[start+1, end]` 是可折叠的正文。
字符串（`"raw"` / `'esc'`）和注释（`//` / `/* */`）里的花括号不算。

#### 1.15.4 交互与视觉

- 行号栏加宽到 64 逻辑像素：左边是诊断圆点，中间是行号，右边是折叠标记；
- 折叠标记是**自己用矩形拼的实心小三角**（▸ 已折叠 / ▾ 展开），
  不依赖字体里有没有 ▶▼ 字形；
- 点标记 → 折叠/展开；光标在区块内部时，折叠的是**包含它的那一块**；
- 菜单「格式 → 折叠/展开 / 全部折叠 / 全部展开」，快捷键 `Ctrl+Shift+[`。

#### 1.15.5 本轮新增/修改

- `lib/nodio/ui/editor.aardio`
  - 新增折叠引擎：`__lineStart` / `__lineEnd` / `__lineOfChar` /
    `__buildLineStarts` / `__scanFoldRegions` / `__applyFoldFormat` /
    `__rebuildVisLines` / `__foldMaintain`；
  - 公开 API：`toggleFold(line)` / `foldAll()` / `unfoldAll()` /
    `isFoldable(line)` / `isFolded(line)`；
  - 行号栏改按 `visLines` 画 + 画折叠标记 + 点击命中；
  - 所有行定位从官方 `lineToChar/lineRange/lineFromChar` 换成自己的行表。
- `ide/nodio/app/shell.nodio`：菜单「格式」加折叠三项 + `Ctrl+Shift+[` 快捷键。
- `tests/editor_fold_probe.aardio`：折叠行为回归（识别 / 收起 / 文本不丢 /
  折叠中定位 / 展开 / 全部折叠）。
- `tests/probe_fold.aardio` / `probe_fold2` / `probe_fold3`：CFE_HIDDEN 与
  行映射的事实探针。

#### 1.15.6 踩过的坑（实测）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 折叠后正文没完全收起，**留一条空行** | 隐藏区间只盖到正文，**没盖最后一行行尾的 CRLF** → 段落标记还留着 | 区间右端取 `lineStart(end+1) - 1`，把 CRLF 一起盖住 |
| 2 | 行号栏第 1 行显示成 **`null1`** | 行号栏循环从 `i=first`（0 基）开始，但 `visLines` 是 **1 基** → `vis[0]` 是 null，`tostring(null)` = "null" 和 "1" 叠在一起 | 循环改成 `i = first + 1` |
| 3 | 折叠标记点不动 | 命中测试同样 0 基/1 基错位 | 同上，`i = first + 1` |

#### 1.15.7 回归

- `tests/editor_probe.aardio`：`PROBE-OK`（诊断波浪线断言仍过）
- `tests/editor_fold_probe.aardio`：`PROBE-OK`
  （11 行 → 折叠后 5 行、文本保留、折叠中 `gotoLine(9)` 仍定位到文档第 9 行）
- VM / IDE 冒烟见本轮记录

---

### 1.16 第十一轮：代码补全（Ctrl+Space → LSP → 候选浮层 → 插入）

标准 IDE 的第三件基本项（本轮前只有「着色 + 诊断 + 折叠」）。
组件负责**算出前缀 / 画浮层 / 键盘导航 / 插入**，IDE 负责**问 LSP 要候选**，
两边用两个回调解耦：`onComplete(line,col)` 出、`showCompletion(items)` 进。

#### 1.16.1 职责划分

| 侧 | 做什么 |
|---|---|
| `nodio.ui.editor` | 算光标处的单词前缀；画候选浮层（自绘 plus 画板，transparent=true，z=70）；↑↓ 选择、Enter/Tab 插入、Esc 取消；按前缀过滤 |
| `ide/nodio/main.nodio` | `shell.editor.onComplete = function(line,col){ ... app.lspClient.completion(line-1,col-1) ... }` → 拿到候选后 `shell.editor.showCompletion(items)` |

#### 1.16.2 前缀过滤放在**客户端**

我们的 LSP 服务器 `textDocument/completion` 返回的是**全量**候选
（文档里所有词 + 全部关键字，见 `apps/nodio-lsp/main.cpp:401 CompletionsFor`）。
真正的按前缀过滤在 `showCompletion()` 里做：

- 用 `string.startsWith(label, prefix, true)`（忽略大小写）筛；
- **一个都不匹配就退回全量** —— 关键字列表仍然有参考价值，
  总比弹一个空框强。

这样做的好处是「过滤」与「问谁要候选」彻底解耦：换后端不用改过滤逻辑。

#### 1.16.3 浮层细节

- 宽度**跟着最长的一条候选走**（170~340 逻辑像素之间夹逼），不写死；
- 最多显示 8 行，超出就**滚动**：`compScroll` 记录第一条可见项，
  选中项永远被滚进可见区（`completionMove` → `__layoutCompletion` 顺带算）；
- 命中测试用**画板本地坐标**（plus 画板的鼠标坐标是画板自己的 0 基坐标）；
- 点候选 → 直接插入。

#### 1.16.4 按键路径的两个坑

1. **`Ctrl+Space` 的请求不能放在窗口过程里**：`onComplete` 会**同步**问 LSP
   （最多等 1 秒），在按键处理里阻塞会卡住窗口过程，
   在那儿建浮层控件也容易重入 → 改成 `__scheduleComplete()`：
   按键只记意图，真正去问放到 `setTimeout(...,1)` 里。
2. **吃掉按键还不够，WM_CHAR 也要跟着吃**：`TranslateMessage` 早一步就把
   `WM_KEYDOWN` 翻译成了 `WM_CHAR` 投回控件 —— 只吃 KEYDOWN 的话，
   Tab 会「既缩进又插一个制表符」，Ctrl+Space 会「多出一个空格」。
   修法：子类化里加 `__eatChar`，KEYDOWN 吃掉某个键时记下它的字符码，
   下一个 WM_CHAR 若是这个码就一起吞掉（新一轮按键先清零，避免误吞）。

#### 1.16.5 本轮新增/修改

- `lib/nodio/ui/editor.aardio`
  - 补全引擎：`__wordAtCursor` / `requestComplete` / `showCompletion` /
    `hideCompletion` / `completionVisible` / `completionMove` /
    `completionAccept` / `__createCompletion` / `__layoutCompletion` /
    `__drawCompletion` / `__hitCompletion` / `__scheduleComplete`；
  - 命名空间级 `__compKindText(kind)`：LSP CompletionItemKind → 中文短标签；
  - 公开 API：`requestComplete()` / `showCompletion(items)` /
    `hideCompletion()` / `completionVisible()` / `completionMove(delta)` /
    `completionAccept()`；
  - 修 `__lineOfChar` 的 0/1 基混用（光标停在行首时会算成上一行）；
  - 新增 `__caretPos()`（有选区时光标在选区**末尾**）。
- `ide/nodio/main.nodio`：`shell.editor.onComplete` 接线；
  演示模式新增 `NODIO_IDE_DEMO_COMPLETE=1`（拉起补全浮层）与
  `NODIO_IDE_DEMO_TYPE=1`（模拟真实键盘输入）。
- `ide/nodio/app/shell.nodio`：菜单「编辑 → 代码补全  Ctrl+Space」。
- `tests/editor_comp_probe.aardio`：补全行为回归（前缀 / 浮层 / 导航 / 滚动 /
  插入 / 关闭 / 真实 Ctrl+Space 按键路径）。
- `tests/ide_comp_sample.nodio`：补全演示样本。

#### 1.16.6 踩过的坑（实测）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `import nodio.ui.editor` 之后 `type(nodio.ui.editor)` 是 **table**（不是 function），一调用就报「尝试调用非函数值」 | 文件里有一行 `// 注释…var __diagSig = function(list){` —— **注释把代码吞了**，后面的 `}` 变成裸块，块里第一句 `if(!list) return ""` 让**命名空间体提前 return**，`editor` 函数从未注册；import 只能回退到空表 | 注释与代码分行；`namespace` 体里**任何 `return` 都会截断整个模块**（排查模块加载问题时要先看这一条） |
| 2 | 前缀算出来是**空串** | `string.sub(s,start,end)` 的第三个参数是**结束位置（含）**，不是长度 —— 写成 `sub(s,start,len)` 直接得到空串 | `sub(lineText, start, bo-1)` |
| 3 | 插入候选时**多删了一个字符** | 前缀为空时 `setSel(a, a)` 是「选中 1 个字符」而不是「光标停这里」 | 前缀为空时用单参 `setSel(a-1)`（0 基插入点） |
| 4 | 光标在行首时状态栏行号/列号差 1 | `__lineOfChar` 拿 0 基位置去比 1 基的 `lineStarts` | 内部统一：`__lineOfChar` 收 0 基，比较时 +1 |
| 5 | 按键被吃掉后控件里仍多出一个空格 | `TranslateMessage` 生成的 WM_CHAR 投回控件 | 子类化里连 WM_CHAR 一起吃（见 §1.16.4） |

#### 1.16.7 回归

- `tests/editor_comp_probe.aardio`：`PROBE-OK`
  （前缀 `al` → 4 个候选 → ↑↓ → 15 个候选时滚动 → 插入后 `print(alphabet` →
  Ctrl+Space 真实按键路径命中 `onComplete(3,9)` 且浮层弹出）
- `tests/run_editor_tests.ps1`：EDITOR-TESTS-OK（3 个探针）

---

### 1.17 第十二轮：**敲一个键就崩**（引擎级致命缺陷）

这一轮本来是去做代码补全的，结果挖出一个**比补全重要得多**的问题：
**IDE 里敲任何键，整个进程直接崩**（`0xC0000005`，进程当场消失，
不是报错对话框，是「窗口没了」）。

#### 1.17.1 现象与定位

| 探测 | 结果 |
|---|---|
| `tests/probe_keysim3.aardio` step1：给 richedit `PostMessage(WM_KEYDOWN)` | 进程崩 |
| step2：`WM_CHAR` | 崩 |
| step3：给 **edit** 控件发 `WM_KEYDOWN` | 崩 |
| step4：给 **static** 控件发 `WM_KEYDOWN` | 崩（静态控件根本不处理按键！） |
| step5：先用 `nodio.ui.subclass` 把 WM_KEYDOWN 吃掉再发 | **照样崩** |

「连 static 控件都崩」「连被子类化吃掉的按键都崩」这两条把嫌疑从
「控件处理消息」推到了「**消息泵在派发之前**就出了事」。

崩溃地址落在 `USER32.dll+0xBA6F`，用 `dumpbin /exports USER32.dll` 对照
导出表，落在 **`IsDialogMessageW`（RVA 0xB960）** 区间内；
崩溃时的读地址是 `0x8`（MSG 结构偏移 8 = wParam），
典型的「结构体指针是 NULL」。

#### 1.17.2 根因

官方 `win` 库的消息泵（`D:\aardio\lib\win\_.aardio`）：

```aardio
isDialogMessage = u.api("IsDialogMessageW","INT(ADDR hDlg,struct msg)");
...
elseif( (message <= 0x108/*_WM_KEYLAST*/) && (message >= 0x100/*_WM_KEYFIRST*/) ){
    var root = getRoot(hwnd)
    if( (root != hwnd ) && isDialogMessage(root,msg) ) return true;
}
```

即：**任何按键消息**（WM_KEYDOWN..WM_KEYLAST）都会走到
`IsDialogMessageW(root, &msg)`，第 2 个参数按官方契约是
**`struct msg` 按值传**（传结构体的原生副本指针）。

而 Nodio 引擎这一侧：

- FFI 遇到 `struct` 按值参数时，会 `StructSerialize()` 把 aardio 的结构体表
  序列化成原生缓冲区再传指针 —— **前提是这张表带 `_struct` 结构定义**；
- 官方消息泵填消息用的表是**普通表**（`var msg = {};`），
  **没有 `_struct`** → `StructSerialize` 拿不到布局 → 返回 NULL →
  `IsDialogMessageW` 收到 NULL 的 MSG → 读地址 0 → **进程崩**。

所以：**这不是补全的问题，是「IDE 根本没法打字」**。
只是此前没有任何人往控件里发过按键消息（没有键盘输入链路），
这个洞一直没被踩到。

#### 1.17.3 修法

`engine/builtins.cpp` 的 `fillMsgTable()` 里，给消息表补上 MSG 的结构定义：

```cpp
// x64 原生 MSG 布局（48 字节）：hwnd@0 message@8 wParam@16 lParam@24 time@32 pt@36
if (tb->map.find(L"_struct") == tb->map.end())
    tb->map[L"_struct"] = Value::Str_(
        L"addr hwnd;INT message;ADDR wParam;addr lParam;INT time;int x;int y;");
```

（`addr`/`ADDR` 在 64 位下是 8 字节，引擎的结构体对齐规则算出来的
偏移与原生 MSG **逐字节一致**，`raw.len(msg)` = 48 可验证。）

顺带把崩溃处理器加了一条**模块内偏移**输出
（`module base=… offset=0xBA6F`）—— 没有偏移，崩溃点只能靠猜。

#### 1.17.4 实测

- `tests/probe_keysim.aardio`：richedit / edit / static 三类控件 +
  子类化吃掉消息的路径，按键消息**发完不崩**（`ALIVE:1`）；
- `tests/probe_type.aardio`：`keybd_event` 打真键 → 文本确实进了 richedit；
- IDE 端到端（演示模式 `NODIO_IDE_DEMO_TYPE=1`）：
  文档从 161 字符变成 162 字符，末尾多出 `x` —— **IDE 现在能打字了**；
- VM 回归 90 passed / 0 failed，编辑器 / 输入 / IDE / LSP 四套冒烟全绿。

#### 1.17.5 新增/修改

- `engine/builtins.cpp`：`fillMsgTable` 补 MSG 结构定义；
- `engine/entry.cpp`：崩溃处理器输出模块基址与模块内偏移；
- `tests/probe_keysim.aardio`、`tests/probe_type.aardio`：输入链路回归；
- `tests/run_input_tests.ps1`：INPUT-TESTS-OK。

> **教训**：官方 aardio 库是 32 位时代的产物，它对结构体按值传参的假设
> （表必须带 `_struct`）在我们这边是**隐式**的 —— 引擎的 `__messagePeek`
> 填出来的表一直没带结构定义，于是每一个按键消息都在 FFI 层变成 NULL。
> 这类「官方库假设 + 我方实现细节」的组合，只能靠**端到端真输入**测出来。

### 1.21 第二十三轮：官方内建 API 缺口收口（函数 API 层面 17 → 1）

> 标记：**第二十三轮**（本节由本会话写入，与并发会话的 IDE 条目互不重叠）

第二十二轮把「全量可用」的验收从 **import 层面**扩到 **函数 API 层面**，
量出官方内建库 **17 个真缺口**（8 个有官方库实际调用点）+ 3 处 **命名不兼容**。
本轮把这 17 个全部落地，**剩 1 个**。

#### 1.21.1 落地清单（官方声明 → 官方契约 → Nodio 实现）

| 官方 API | 官方契约（出处） | 落点 |
|---|---|---|
| `string.split` | 4 种形式：1 参逐 UTF-8 字符 / 2 参**单字节分隔符集合** / `'<整词>'` / 第 3 参 `maxCount` 是结果元素数上限 | `engine/builtins.cpp`（重写） |
| `string.splitLiteral` | 整词分隔 | `engine/builtins.cpp`（新增） |
| `io.lines(pathOrFile)` | 逐行迭代，EOF 自动关闭 | `engine/builtins.cpp`（原生迭代器） |
| `io.libpath(name)` | 返回**（库文件路径, 所在目录）**，找不到 null | `engine/builtins.cpp` + `engine/import.cpp`（复用 import 解析） |
| `math.fmod(x,y)` | C `fmod`，符号随**被除数 x** | `engine/builtins.cpp` |
| `math.lerp(from,to,t)` | `from+(to-from)*t`，`t` 不夹取 | `engine/builtins.cpp` |
| `table.setAttr(obj,key,value)` | 含该键直改，否则走 `_meta._set`（`ownerAttr=true`） | `engine/builtins.cpp` |
| `com.NewObject(tab,progId[,tlb])` | 三值返回 `(appObject, event, e)` | `engine/com.cpp` |
| `com.NewControl(tab,progId)` | 同上三值形状 | `engine/com.cpp` |
| `com.SafeArray(vt,...)` | `{_safearray=元素表, _type=vt}` | `engine/com.cpp` |
| `com.IsIUnknown` / `com.IsMember` / `com.GetPointer` | 见官方 `builtin/com.aardio:192,209` | `engine/com.cpp` |
| `com.DoObjectVerb(obj,verb)` | `IOleObject::DoVerb` | `engine/com.cpp` |
| `com.Connect` / `com.AddConnection` / `com.ReleaseConnection` | 事件接收器（见 §1.21.3） | `engine/com.cpp` |

3 处**命名不兼容**（官方名不存在）随之全部按官方名补齐：
`com.NewObject`（官方第 1 参是接收结果的表，与 Nodio 自有的 `com.CreateObject`
签名不同，两者并存）、`com.SafeArray`、`com.ReleaseConnection`。

#### 1.21.2 顺带修正的契约偏差

* **`%` 运算符**：官方是**向下取整（floor）**，余数符号跟**除数 y**
  （`D:\aardio\docs\language-reference\operator\arithmetic.md:46`）。
  Nodio 原先用 C `fmod`（符号跟被除数）—— `engine/runtime.cpp` 3 处 +
  `engine/aardio_vm.cpp` 0x1C 1 处改为 `AardioMod()`。
  实测 `-7%2=1` / `7%-2=-1` / `-7%-2=-1` / `-7.5%2=0.5`。
  官方 `math.fmod` 恰恰是 C `fmod`（符号跟被除数），两者语义**不同**，都已落地。
* **`string.split` 的 2 参语义**：此前 Nodio 按「多字节字面量」切，官方是
  **单字节分隔符集合**；多字节字面量必须用 `'<整词>'`。
  官方库 `builtin.string` 大量 1 参调用此前全是坏的，现已逐字符正确。
* **`com.NewObject` 失败路径的错误信息丢失**：多返回值的 `mrets` 只装**首值之外的
  附加值**，失败分支把首值重复塞进 `mrets`，导致 `e` 取到 null —— 已修。

#### 1.21.3 COM 事件接收器（官方 `builtin/com.aardio` 的硬依赖）

官方 `builtin/com.aardio` 的 `ConnectWeak` / `CreateEmbedEx` 是 **aardio 源码**，
但都调用**引擎原生**的 `Connect` —— 不补这个，官方这批 aardio 源码在 Nodio 里
直接「尝试调用非函数值」。

实现（`engine/com.cpp`）：

* `com.Connect(comObject, 事件监听表)` → `(sink, cookie)`；
  `com.AddConnection(comObject, 事件接收器)` → `cookie`；
  `com.ReleaseConnection(comObject[, sink, cookie])`。
* 连接点走 `IConnectionPointContainer`，**只取 dispinterface 型**
  （`IID_IDispatch`，或枚举连接点后取源接口是 `TKIND_DISPATCH` 的）；
  源接口类型信息从**类型库**反查（连接点只给 IID）。
* 接收器是一个 `IDispatch` 实现：服务端用源接口 DISPID 回调，引擎用源接口类型
  信息把 DISPID 还原成**事件名**，再到 aardio 事件表取同名成员调用
  （官方 `ConnectWeak` 的接收表正是用 `_meta._get` 转发的）。
* 连接点与 cookie 登记在 COM 对象包装表的隐藏成员上，供 `ReleaseConnection` 解绑。

**实测**（`tests/builtin_api_check.ps1` 第 5 段）：

* `MSXML2.DOMDocument.6.0`、`Shell.Explorer` 连接成功（`cookie>0`），
  `sink.onreadystatechange(1,2)` 走通
  `GetIDsOfNames → Invoke → DISPID → 事件名 → aardio 处理函数`，事件**触发 1 次**；
* 官方 `com.ConnectWeak` 同样可用；
* `com.ReleaseConnection` 解绑计数正确，重复调用静默返回 0；
* 没有连接点的对象（`Scripting.FileSystemObject`）→ **明确报错**，不静默失败。

> 踩过的坑：连接成功后**不能**再 `Release` 那个 `IConnectionPoint`
> —— 登记表里存的是它，重复释放会让 `ReleaseConnection` 拿到**悬垂指针**
> 并在 COM DLL 里触发访问违例（实测 `msxml6.dll` 0xC0000005）。

#### 1.21.4 仍未闭合（诚实记账）

> **状态更新（第二十五轮）**：本节两项缺口已闭合，详见 §1.23。
> 下表保留为当时的记账快照。

| 项 | 状态 | 原因 |
|---|---|---|
| `com.ImplInterfaceFromTypelib(implTable,typelibPath,interfaceName,coclassName)` | **未实现** | 需要「按 ITypeInfo **动态构造原始接口 vtable**」（官方 `com.interface` 的机制）。**官方库零调用点** |
| 原始（非 IDispatch）事件连接点 | **不支持** | 同上：`com.wmi` 的 `SWbemSink`（`IWbemObjectSink`）、`WMPlayer.OCX` 默认源接口都是原始接口；实测 `WMPlayer.OCX` 返回 `hr=0x80040202`（`EVENT_E_ALL_SUBSCRIBERS_FAILED`）—— 连接点找到了，但接收器不被接受 |

#### 1.21.5 回归

* `tests/builtin_api_check.ps1`（**5 段**）：`string.split`/`splitLiteral`（20 项）、
  `io.lines`/`io.libpath`（10 项）、`math.fmod`/`lerp`/`table.setAttr`/`%`（15 项）、
  `com.*` 原生 API（14 项）、`com.Connect` 事件链路（9 项）；
* `tests/builtin_api_coverage.ps1`：官方内建成员覆盖率核对
  （官方清单固化为 `tests/official_builtin_members.json`）；
* x64 / x86 **两套构建均全绿**；既有 7 套回归（`aardio_run_check` /
  `aardio_chunk_check` / `run_vm_tests` 90 passed / `run_stdlib_tests` /
  `import_contract_check` / `sysapi_check` / `s2_error_location_check`）全绿。

> 实测口径（第二十四轮）：**官方内建成员 150 个，Nodio 覆盖 149 个（99.3%）**，
> 唯一缺口 `com.ImplInterfaceFromTypelib`。
> **第二十五轮已补齐该缺口，覆盖率升至 150 / 150（100%）**，见 §1.23。
> （§1.21 与第二十二轮 §8 的分母 156 不同：后者把 `thread._serialize`、
> `thread.release`、`raw._json` 这类**嵌套表成员**也计进了成员数。）


## 1.22 第二十四轮：comTypeInfo 类型信息契约 + 杀软误报根治

### 1.22.1 官方类型信息契约（已闭合）

**原缺口**：`comTypeInfo`（`com.GetTypeInfo` / `com.LoadTypeLibrary(...).GetTypeInfo(i)`）
缺 `GetTypeAttr()` 与 `GetFuncDesc(i)`，导致三个官方标准库文件是死代码：

- `lib/com/typeAttrs.aardio` —— `attr.typekind` / `attr.Funcs` / `attr.Vars` /
  `attr.GUID` / `attr.flags.*`
- `lib/com/tlbDoc.aardio` —— `func.name` / `func.invkind` / `func.memid` /
  `func.type` / `func.vt` / `func.parameters` / `func.ParamsOpt` / `func.description`
- `lib/com/interface/_.aardio` —— `com.interface.loadTypelibrary` 的全部前置字段

**已补齐**（`engine/com.cpp`）：`GetTypeAttr()`、`GetFuncDesc(i)`、`GetVarDesc(i)`、
`GetImplType(i)`、`GetImplTypeFlags(i)`，形状严格按官方 `!comTypeAttr` /
`!comTypeFunc` / `!comTypeParameter` / `!comTypeVar`（索引从 1 起）。

**实测**（`C:\Windows\System32\stdole2.tlb`，42 个类型）：

- `GetTypeAttr()` 的 `typekind` 覆盖 record / interface / alias / enumeration /
  dispinterface / coclass / module 全部官方取值；GUID 形如
  `{00000000-0000-0000-C000-000000000046}`（IUnknown，正确）。
- `GetFuncDesc(1)`（IUnknown::QueryInterface）：`name` / `invkind=function` /
  `Params=2` / `ParamsOpt=0` / `type=int`（VT_HRESULT=25）/ `vt=25` /
  `memid=1610612736`（0x60000000）/ `restricted=true` / `parameters[1].name=riid`。
- 官方 `com.typeAttrs.summary` → 8 interface / 2 coclass / 2 enum；
  官方 `com.typeAttrs.load` → 42 个类型；
  官方 `com.tlbDoc` → 11121 字文档（此前完全不可用）；
  官方 `com.interface.loadTypelibrary(tlb,"IFont")` → 动态生成 `com.interface.IFont`
  （IID `{BEF6E002-A874-101A-8BBA-00AA00300CAB}` 正确），可实例化。

**实现备注**：本机 SDK 的 `oaidl.h` 是旧式 Automation 头（`FUNCDESC` 无 `params[]`、
无 `flags` 成员，参数描述在 `lprgelemdescParam`，`cParamsOpt` 即官方 `ParamsOpt`），
`TA_F*` / `FUNCFLAG_*` 头文件未定义、按 Automation ABI 固定取值在源码内补齐。

### 1.22.2 杀软误报（已闭合，非能力缺口）

火绒把 **32 位** `nodio.exe` 报为 `HEUR:Backdoor/Farfli.e` 并隔离，64 位同源构建
从不被拦。二分定位（独立 Win32 构建 + 逐项开关 + 读火绒隔离区）得到命中条件：
**「可写且可执行的内存」与「32 位未处理异常过滤器转储原始寄存器」两特征共现**。

修复：跳板页与内存 PE 映射改为「申请时可写 → 写入代码 → 按页/按节降权为
`PAGE_EXECUTE_READ`」，页面从不以「可写+可执行」形态存在；32 位崩溃过滤器
不再转储整组原始寄存器（保留 `EIP/ESP/EBP` + 异常码 + 出错地址 + 模块内偏移 +
访问违例信息）。能力不减，x64 / x86 复测均 CLEAN。

回归锁：`tests/av_heuristic_check.ps1`（源码级断言，不依赖杀软）。

**仍未闭合**：`com.ImplInterfaceFromTypelib` / `com.ImplInterface`（3 参形式）——
需按 ITypeInfo **动态构造原始（非 IDispatch）接口的 vtable**；与「原始接口事件
连接点」（`WMPlayer.OCX` 的 `_WMPOCXEvents`）同根因。本轮补齐的类型信息设施
是它的前置条件，vtable 生成器本身尚未实现。官方内建成员覆盖仍是
**149 / 150（99.3%）**，唯一缺口不变。

> **状态更新（第二十五轮）**：vtable 生成器已实现，该缺口闭合，
> 覆盖率升至 **150 / 150（100%）**，见 §1.23。


## 1.23 第二十五轮：com.ImplInterface / 原始接口事件连接点闭合

### 1.23.1 已闭合项

| 项 | 状态 | 实测 |
|---|---|---|
| `com.ImplInterface(implTable,progID,interfaceName)`（3 参） | **已实现** | 官方文档示例 `com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")` 端到端跑通，`com.AddConnection` 返回 cookie=1 |
| `com.ImplInterfaceFromTypelib(implTable,typelibPath,interfaceName,coclassName)` | **已实现** | `com.ImplInterfaceFromTypelib(events,"C:\\Windows\\System32\\wmp.dll","_WMPOCXEvents","WindowsMediaPlayer")` 收到 11 次真实事件 |
| 原始（非 IDispatch）事件连接点 Advise | **已实现** | WMP 事件真实送达 aardio 处理函数：`OpenStateChange(1)` / `StatusChange` / `PlayStateChange(9)` … |
| `com.Connect` 自动路径 | **已实现** | 源接口 IID 非 `IID_IDispatch` 时自动改用原始 vtable 接收器，收到 9 次事件 |

### 1.23.2 三个关键实测结论

1. **服务器按「源接口 IID」查 sink，不是按 IID_IDispatch**。
   WMP 的 `_WMPOCXEvents` 在类型库里登记为 dispinterface，但连接点的源接口 IID
   是 `{6BF52A51-394A-11D3-B153-00C04F79FAA6}` —— 纯 IDispatch 接收器会被拒绝，
   返回 `0x80040202 EVENT_E_ALL_SUBSCRIBERS_FAILED`。
   这修正了 §1.21.4 对该返回码的解读：**不是「连接点找不到」，而是「接收器不被接受」**。
2. **事件由 `IDispatch::Invoke` 派发**（服务器命中 vtable 的第 7 个槽，index 6）。
   所以 vtable 里**必须**包含 IDispatch 的四个槽位，少一个就崩。
3. **dispinterface 的 `cImplTypes` 在不同类型库副本里可能不一致**：
   有的副本登记了 IDispatch 基接口（vtable 自带四槽），有的为空。
   因此对 `TKIND_DISPATCH` 无条件补齐 IDispatch 四槽，不能依赖类型信息。

### 1.23.3 实现要点

- **vtable 生成器**（`engine/com.cpp` `BuildRawSink`）：按 ITypeInfo 递归收集继承链，
  槽位顺序 = IUnknown(3) + 继承接口 + 自身方法（`GetFuncDesc` 下标顺序）；
  每槽一个 stdcall 跳板，参数按 ELEMDESC 转换，方法按名字转发到 aardio 同名成员。
- **接口指针 vs 结构体指针**：`VT_PTR|VT_USERDEFINED` 靠 typekind 判定
  （只有 `TKIND_INTERFACE` / `TKIND_DISPATCH` 才是接口指针）。
- **IDispatch 四方法的参数码硬编码**：`Invoke` 的 `pVarResult` 是 `VARIANT*` 出参，
  按类型信息推导会被误判成「按值 VARIANT」而读到未初始化内存（→ `AddRef` 崩溃）。

### 1.23.4 回归

`tests/builtin_api_check.ps1` 新增**第 7 段**（`com.ImplInterface / 原始接口事件`）：
3 参形式未知接口报错 + 三条路径的真实事件派发 + 事件名/参数值正确 + 解绑正常。
双架构（x64 / x86）11 套既有回归全绿。


## 1.24 第二十六轮：官方标准库可用性 —— 扩展名不应依赖外部配置文件

### 1.24.1 缺陷（真实、非测试环境误报）

`nodio.json` 找不到时，引擎的默认扩展名列表只剩 `.nodio`，
官方标准库的唯一源码扩展名 **`.aardio` 直接消失** →
**整个官方标准库静默失效**：`import win.ui` 不报错，但 `win.ui` 是 `null`，
`win.form` 从官方的 `class` 退化成引擎内置的 `native function`。

| 触发场景 | 说明 |
|---|---|
| **发布产物** | 自包含 exe，官方库编译进 payload，外部**没有** `nodio.json` |
| 自定义部署目录 | exe 旁边没有 `nodio.json`（查找只上溯两级） |
| CI 临时工作目录 | `tests/module_sweep.ps1` 把 `WorkingDirectory` 设成 `%TEMP%` |

实测对照（同一个 exe、同一个官方库，只换工作目录）：

| 工作目录 | `type(win.ui)` | `type(win.form)` |
|---|---|---|
| `D:\Nodio`（能找到 `nodio.json`） | `table` | `class` |
| `%TEMP%` / 任意自定义目录 | **`null`** | **`native function`** |

### 1.24.2 根因

`engine/util.cpp` 的 `LoadConfigInternal()`：

- 配置文件里的 `extensions` 数组**替换**默认列表；
- 默认列表只有 `.nodio`（`.aardio` 只写在 `nodio.json` 里）；
- 配置查找只上溯两级（exe 同目录 → 上一级 → 上两级），
  找不到就只剩 `.nodio`。

`magicHeaders` 同理：默认只有 `=nodio` / `=bytecode`，
`=aardio`（官方预编译块魔数）也只在配置文件里。

### 1.24.3 修法

**官方契约固化进引擎内置默认**，配置文件降级为「追加」而非「替换」：

- 默认 `extensions` = `.nodio` + `.aardio`（`.nodio` 在前：同名时 Nodio 自己的源码优先）；
- 默认 `magicHeaders` = `=nodio` + `=bytecode` + `=aardio`；
- 配置文件 / `NODIO_EXTENSIONS` 里列出的扩展名**追加**到内置列表（去重）。

改法只影响**扩展名查找与后缀剥离**，不改变任何查找顺序或优先级规则。

### 1.24.4 顺带澄清（非缺口）

`::RECTF` / `::LOGFONT` / `::PAINTSTRUCT` / `::WNDCLASSEX` / `::MSG` 在
「只 `import builtin.struct`」时是 `null` —— 这是**官方契约本身**：

- `::RECTF` 定义在 `gdip/core.aardio:75`、`::LOGFONT` 在 `gdi/_.aardio:4`，
  属于**按需导入**的包，不是 builtin；
- `::PAINTSTRUCT` / `::WNDCLASSEX` / `::MSG` 官方 lib 里**根本没有** `class ::X` 定义
  （`::MSG` 在 `builtin/_.aardio:49` 被官方**故意**设为 `null`，
  `win` 包用的是另一个 `struct msg`）。

### 1.24.5 回归

新建 `tests/lib_availability_check.ps1`（4 段）：

1. **无 `nodio.json` 时**官方标准库仍可用（`win.ui` 是 table、`::RECT` 是 class、
   `::CopyMemory` 是 native function、`::HIWORD`/`::LOWORD`/`::MAKELONG`/`::MAKEWORD`
   是 function，且函数语义与官方逐位一致）；
2. **有 `nodio.json` 时**行为一致（不因修复而回归）；
3. **配置里只写 `.nodio`** 时官方库仍可用（扩展名是追加，不是替换）；
4. **配置缺失时** `=aardio` 魔数仍被内置识别。

x64 / x86 双架构全绿；`builtin_api_check.ps1` 7 段双架构全绿。

## 1.25 第二十六轮（续）：字节码序列化丢值 —— 发布产物的崩溃链

### 1.25.1 缺陷（真实）

发布产物（自包含 exe）一启动就崩，错误链有两级：

1. `字节码读取错误: 越界的符号名索引 @ gdip\encoder.aardio:3`；
2. 修掉上一条后：`time\performance.aardio:5` 在 ntdll 里访问冲突。

**对照实验**（同一工程、同一台机器）：用**基线 exe**（`build-x64\Release\nodio.exe`，
早于本轮全部改动）发布同一工程 → 两种宿主都报
「运行错误: 载荷中没有可执行的启动脚本」；用本轮改动后的 exe 发布 →
产物能正常跑起来。即：**本轮修复让发布产物从「完全不可用」变成「可用」**。

### 1.25.2 根因

两个独立缺陷，都在 `engine/compiler.cpp` 的字节码序列化上：

**(a) 空字符串被跳过 → 写出越界哨兵**

- 解析器**故意**会产出空名：`parser.h` 的缺位命名空间段、`...` 可变参数标记等；
- `CollectStrings()` 的 `addStr` **跳过空字符串** → 这些名字不在字符串池里；
- `WriteNode` 找不到名字时写出哨兵 `0xFFFFFFFF` → 反序列化判为越界 → 整个发布失败。

**(b) `Node::Bool` 的值被丢弃**

- 布尔字面量的值存在 `num` 字段，但序列化端**只为 `Node::Num` 写 double**；
- 布尔字面量在字节码往返后一律变成 `false`；
- 官方 `time/performance.aardio:4` 的 `..raw.long(0,true)` 因此变成 `raw.long(0,false)`
  → 发布产物启动即在 ntdll 访问冲突。

### 1.25.3 修法

- `CollectStrings` 允许空串入池；新增 `EmptyStringIndex(strMap)` 辅助函数，
  `WriteNode` 找不到名字时写池内空串的**有效索引**（不再写 `0xFFFFFFFF`）；
- `ReadNode` 对 `0xFFFFFFFF` 宽容（读作空串）；
- `Node::Bool` 与 `Node::Num` 一样写/读 double；
- `SerializeNode` 拆出无自检的 `WriteBytecode()`，并新增**往返字节级自检**
  （序列化 → 反序列化 → 再序列化 → 要求两次字节完全一致），
  失败通过新增的 `wstring* err` 参数上报 —— 把「发布时才发现格式丢信息」
  提前到编译期。

### 1.25.4 回归锁

`tests/publish_official_lib_check.ps1`（新建）：发布含 `import win.ui` /
`import builtin.struct` / `import string.format` 的工程 → 复制到**隔离目录**
（确定没有 `nodio.json`、不设 `NODIO_LIBDIR`）→ 断言
`win.ui=table` / `RECT=class` / `HIWORD=4660`。
x64 / x86 双架构全绿。

## 1.26 第二十六轮（续）：结构体编组缓冲区缺少余量 —— callee 越界读

### 1.26.1 缺陷（真实）

发布产物**基名恰好是 `app`**（`APP` / `App` 同样）时，进程在
`Gdiplus!GdiplusStartup` 里访问冲突（`exit=0xC0000005`）：

- 同一份字节、同一目录、同一宿主（`nodio.exe` 与 `nodio-stub.exe` 宿主都复现）；
- 20+ 个其它名字（`app2` / `apps` / `appx` / `xapp` / `main` / `qq` / `xyz` / `abc` …）全部正常；
- 目录深度（4 种）、文件创建顺序、`NODIO_LIBDIR` 的 4 种写法都**不改变**这个结论；
- 两次运行的 `import` 解析日志**逐行相同**（224 行，仅 exe 路径那一行不同），
  结构体声明大小与 `_struct` 元数据**完全相同**（16B / 80B）→ 差异只在缓冲区落点。

### 1.26.2 根因（cdb 实测）

崩溃指令与现场：

```
Gdiplus!GdiplusStartup+0x30:
00007fff`e31275c0 837a1000   cmp dword ptr [rdx+10h],0 ds:00000251`bec17000=????????
00000215`461f2ff0  01 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00  ................
00000215`461f3000  ?? ?? ?? ?? ?? ?? ?? ??-?? ?? ?? ?? ?? ?? ?? ??  ????????????????
```

- `rdx` 指向引擎为 `struct&` 参数编组出来的缓冲区，内容是
  `GdiplusVersion=1` + 3 个 0 —— 共 **16 字节**（4 × `int`）；
- 这 16 字节**正好压在 64KB 页尾**，下一页未映射；
- 崩溃指令读的是 `[rdx+0x10]` = **第 17 字节**，正好在缓冲区之外。

为什么 callee 要读第 17 字节：**官方库是按 32 位 aardio 写的**。

- 官方 `gdip/core.aardio:13` 声明
  `GdiplusStartupInput = class{ int GdiplusVersion; int DebugEventCallback; int SuppressBackgroundThread; int SuppressExternalCodecs; }`
  → 4 × `int` = **16 字节**；
- Win32 的 `GdiplusStartupInput` 字段是 `ULONG_PTR`，**x64 下 8 字节** → 真实结构 **32 字节**；
- 官方 aardio 本体是 **x86**（实测 `D:\aardio\aardio.exe` 的 PE machine = 0x014c），
  x86 下 `ULONG_PTR` 也是 4 字节 → 官方声明与真实结构**完全吻合**，因此官方从不越界；
- Nodio 同时提供 x64 与 x86：x86 无此问题，**x64 必然越界**。

> 未完全定位：为什么偏偏是 `app` 这个名字能让这块 16 字节缓冲区落在页尾。
> 已排除目录深度、文件创建顺序、`NODIO_LIBDIR` 写法、import 解析差异等变量；
> 可确定的是崩溃点是 callee 对 16 字节声明结构的越界读，
> 而余量修复后**所有名字**（含 `app`）都正常。

### 1.26.3 修法

**结构体编组缓冲区按零填充分配，并带一段余量**（`engine/ffi.cpp`）：

- `StructSerialize()` 增加 `cbSlack` 参数，分配 `total + cbSlack`（`HEAP_ZERO_MEMORY`）；
- 新增 `StructOverreadSlack(vTab)`：按字段逐个算「平台指针宽度 − 声明宽度」的差值求和，
  下限 64 字节 —— 覆盖 x64 `ULONG_PTR` 展开带来的额外宽度；
- 静态 FFI（`engine/builtins.cpp` 的 `struct` 参数分支）与动态 FFI
  （`engine/ffi.cpp` 的 `CreateDirectApiWrapper`）都传这份余量。

效果：callee 的越界读**落在分配内部**，读到的多出来字段是 0
（`SuppressBackgroundThread=0` / `SuppressExternalCodecs=0`），
与官方在 x86 上的行为一致；`pOutTotal` 仍报告**声明尺寸**，
`cbSize` 自动戳值与 `StructDeserialize` 回读长度都不变。

### 1.26.4 影响面

- 官方库**不走** `raw.*` 结构体分配 API（实测全库只有 `raw.serializeDupHandle` 一类
  句柄序列化），所以只需覆盖 FFI 路径；
- x86 侧本来就不越界，加余量是无害的冗余。

### 1.26.5 回归锁

`tests/publish_official_lib_check.ps1` 的产物名**就用 `app.exe`**（曾崩的名字），
并断言官方库在**隔离目录**里可用 —— 修前 FAIL（3 项 `<missing>`），修后 PASS。
x64 / x86 双架构全绿。

## 1.27 第二十六轮（续）：每条语句都在做一次 `GetEnvironmentVariableW` —— 解释器吞吐差 4~5 倍

### 1.27.1 现象（实测）

x64、200 万次迭代、每项取 3 次最优值，扣除进程启动基线（59.9 ms）：

| 用例 | 改前 ns/次 | 改后 ns/次 | 倍数 |
|---|---|---|---|
| 空循环 `for(i=1;N;1){}` | 89 | 80 | 1.1× |
| `s=s`（空赋值） | 1838 | **359** | 5.1× |
| `s++` | 2209 | **453** | 4.9× |
| `s=s+1` | 2183 | **449** | 4.9× |
| `s=(s+i*3)` | 2356 | 600 | 3.9× |
| `s=(s+i*3)%1000003` | 2503 | 732 | 3.4× |
| 全局变量写 `s=s+1` | 2049 | 471 | 4.4× |
| 函数内局部变量写 | 1973 | 496 | 4.0× |
| 表字段写 `t.v=t.v+1` | 2180 | 675 | 3.2× |
| 函数调用 `s=f(s)` | 5640 | 1817 | 3.1× |

**空循环只要 89 ns，一条赋值却要 1838 ns** —— 成本不在算术、不在变量种类
（局部/全局/表字段都一个量级），而在**执行一条语句本身**。

### 1.27.2 根因（cdb 采样栈 + 代码定位）

对长跑基准用 cdb 挂载采样，主线程栈顶是：

```
ntdll!RtlQueryEnvironmentVariable+0x188
KERNELBASE!GetEnvironmentVariableW+0x40
nodio+0x596b9        <- 引擎内 EnvFlag()
```

`engine/common.h` 的 `EnvFlag()` 直接调 `GetEnvironmentVariableW()` ——
它要扫一遍进程环境块，**约 1~2 µs**。而 `EnvFlag(L"NODIO_GC_TRACE")` 被放在
**每条语句都会经过**的位置：

- `engine/runtime.cpp` 的 `MaybeCollectGC()`（语句边界上的 GC 安全点）——
  两条分支各调一次；
- `engine/gc.cpp` 的 `CollectGarbage()` —— 7 处（其中 `NODIO_GC_NOFINAL`
  在**每个待终结对象**上都要问一次）。

一次 `GetEnvironmentVariableW` ≈ 1.75 µs，与实测的「每条赋值多花 1.75 µs」
**完全吻合**。

### 1.27.3 修法

诊断开关**一次性读取并缓存**（函数内 `static const bool`，只有一次初始化守卫，
约 1 ns）：

- `engine/common.h` 新增 `GcTraceEnabled()` / `GcNoFinalEnabled()` / `ComTraceEnabled()`；
- `engine/gc.cpp`（8 处）、`engine/runtime.cpp`（2 处）改用缓存值；
- `engine/com.cpp` 的 `ComRawTrace()` 同样缓存，并**删掉遗留的临时诊断块**
  （写死 `%TEMP%\avtest\comraw.log` 的那段初始化日志）。

语义不变：开关仍然按「环境变量是否存在」判定（与原 `EnvFlag` 一致），
只是**每个进程只读一次**。

### 1.27.4 残留性能问题（下一步的方向）

修完之后仍有的开销（同一基准）：

1. **每条语句仍要 ~450 ns**（空循环 80 ns）—— 变量查找走
   `Env::Find()`：作用域链逐层 `unordered_map<wstring,Value>` 查找，
   再叠加命名空间表（`nsTable->scopeVars` / `nsTable->map` / `object` 成员表）
   的 2~4 次哈希查找。**下一步：把变量名 intern 成整数 slot**，
   局部变量改成数组索引。
2. **函数调用 ~1.8 µs/次** —— 闭包/环境帧构造 + GC 根栈与 epoch 栈的进出。
3. **模块加载成本**：`import win.ui` 净 ~310 ms（编译并执行 60+ 个官方模块的
   顶层代码）；冷启动（空脚本）~60 ms。
4. **GC 阈值**：`nGCThreshold` 默认 2000 次分配，粒度偏粗；官方库模块
   顶层代码会产生大量短命对象。

### 1.27.5 回归

`builtin_api_check` 7 段、`aardio_bytecode_check`、11 套既有回归
**x64 / x86 双架构全绿**；x64 全量模块 sweep 与基线**逐项一致**
（ok=899 / fail=70，0 新增失败）。

> 另记：`tests/win_clip_check.ps1` 在 x86 上**偶发变红**（同一二进制 8 连过一次、
> 另一轮 3 次里挂 1 次）—— 剪贴板是**全局系统资源**，被同机其它进程占用时
> 官方语义是「静默失败」，而本测试只在**开始时**判定一次 `CLIP-OK`，
> 中途被占用就会有一条往返断言失配。这条回归锁本身需要改成
> 「每次断言前重新探测可用性」。
## 1.28 第二十八轮：x64 浮点 ABI 缺陷（cdecl 绕过 XMM0..3）+ av.audio.dsp x64 补版

### 1.28.1 起因：补 x64 版本时挖出一个引擎级根因

x64 全量模块 sweep 的 70 项失败里有 34 项是「无法加载 DLL」，归因于官方库
只带 32 位 PE。补版路线（**按官方同名路径在 Nodio 侧提供自己的版本**）
落地时，`av.audio.dsp` 编出 x64 DLL 后**数值全是 0** —— 顺着这条线挖出一个
**引擎级**根因缺陷，影响面远大于 dsp 一个库。

### 1.28.2 实测：x64 的 double 实参根本没送到被调方

用「回读参数」探针 DLL（`double t_geta(double a,double b)` 返回它看到的 a）
在**独立进程**里逐个测：

| 调用 | 结果 | 期望 |
|---|---|---|
| `raw.loadDll(path,name)`（无 cdecl）+ `api("p2","double(double,double)")` | 1502.5 | 1502.5 |
| `raw.loadDll(path,name,"cdecl")` + 同一 api | **2500** | 1502.5 |
| `raw.loadDll("msvcrt.dll",,"cdecl").api("pow","double(double,double)")(2.0,10.0)` | **1** | 1024 |
| `api(..., "stdcall")`（覆盖 dll 级 cdecl 标记） | 1502.5 | 1502.5 |

规律：**只要这一调走 cdecl 分支，double 实参就被换成垃圾**。
`p2(1.5,2.5)` 被调方收到的是 `a=2.5, b=0`（第二个实参落到了第一个位置，
第一个位置是空的）—— 典型的「实参没进 XMM 寄存器」特征。
`p3(1,2,3)` 收到 `a=3, b=1, c=1`、`p4(1,2,3,4)` 收到 `a=4, b=1, c=2, d=0`。

### 1.28.3 根因

`engine/builtins.cpp` 的调用分派：

```cpp
bool bStub = stub && !sig.bCdecl;   // x64
```

MS x64 ABI 只有**一套**调用约定：编译器忽略 `__cdecl` / `__stdcall` 的区别，
float/double 实参必须走 XMM0..XMM3。而 `InvokeCdl` / `InvokeStd` 的定型函数
指针把**每个**实参都声明成整数类（`SlotT = unsigned __int64`），编译器于是把
double 的位模式塞进 RCX/RDX/R8/R9 —— 被调方在 XMM0 读到的是垃圾。

按签名逐字节发射的调用桩（`engine/ffi.cpp` 的 `EmitX64CallStub`）本来就是
对的：它按**参数位置**把浮点实参 `movsd` 进对应的 XMM 寄存器，位置相关的
分配也正确（位置 2 的 double 进 xmm1，位置 3 的整数仍进 r8）。
**只是 cdecl 分支绕开了它。**

### 1.28.4 修法

x64 只有一套调用约定 ⇒ cdecl / stdcall 的区分在 x64 上没有意义：

```cpp
#if defined(_WIN64)
bool bStub = (stub != nullptr);   // x64 一律走 ABI 正确的调用桩
#else
bool bStub = false;
#endif
```

x86 行为不变（x86 上 `bStub` 本来就是 false）。

### 1.28.5 av.audio.dsp：按官方路径补 Nodio 自己的 x64 版本

官方 `lib/av/audio/dsp.aardio` 把 32 位 `dsp.dll` 用 `$` 内嵌进源码再内存加载，
x64 进程装不下该 PE。官方**同目录 `.res/dsp.c`** 就是这份 DLL 的 C 源码
（官方自己的 `build.aardio` 就是用它 + tcc 重建 32 位 DLL 的）。

Nodio 侧的补版（**同名路径覆盖**，overlay 目录优先于官方库）：

* `lib/av/audio/.res/dsp64.dll` —— 由官方 `.res/dsp.c` 用 MSVC 重新编译
  （x64，/MT 静态 CRT，不依赖外部运行库），**38 个导出函数与官方 32 位 DLL
  逐个一致**（`dumpbin /exports` 核对）；
* `lib/av/audio/dsp.aardio` —— 与官方同名同路径的 overlay 模块，
  API 面（函数名 + 原型）逐条相同，差别只在 DLL 来源：
  官方用 `$"..."` 内嵌 32 位 DLL，本版用 `io.libpath()` 定位自身目录后
  加载同目录的 x64 DLL。

> x86 构建不会命中 overlay（`engine/import.cpp` 的 `GetOverlayLibDir`：
> 32 位构建直接用 aardio 官方 32 位标准库），行为与官方完全一致。

### 1.28.6 验证

* **数值一致**：x64 overlay 版与 x86 官方版对同一组输入给出**逐项相同**的结果
  （fill 1.5 → peak 1.5；scale ×2 → peak 3 / sum 12 / sumsq 36；
  ramp 1→4 → peak 12；countOver 4 / firstOver 0 / lastOver 3），
  38 个导出函数全部可调用。
* **回归锁**：新建 `tests/ffi_double_check.ps1`（9 项断言，x64 / x86 双绿）——
  用 msvcrt 的 `pow` / `floor` / `sqrt` / `fmod` / `atan2` / `ldexp`
  锁住「cdecl + double」的 ABI，再断言 dsp overlay 的三项数值。
* **全量**：x64 / x86 各 17 套既有回归全绿；x64 全量模块 sweep
  **ok=899 → 904 / fail=70 → 65**，失败集合**只减不增**：
  修复的正是 `av.audio` / `av.audio.dsp` / `av.audio.mixer` / `av.audio.music` /
  `av.audio.sfx` 五项，**新增失败 0 项**。

### 1.28.7 顺带的结构性结论

x64 上「32 位 DLL 装不下」只是**表层**原因；同一批库里还叠着**浮点 ABI 全错**
这一层。两者叠加导致 gdip / av.audio / av.video.kernel / string.markdown /
sevenZip.lzma 这些「全 double API」的模块在 x64 上一个都跑不起来。
ABI 层修好之后，剩下的就只剩「官方那份 32 位 PE 本身装不下」这一件事 ——
那才是需要**按官方路径补 Nodio 自己版本**的部分。

---

## 1.29 第二十九轮：GUI 层六个根因（按钮不可点 / 组件不显示 / 还原黑屏）

这一轮修的六件事有一个共同点：**每一个都是一行源码或文档里写着的事实**，
但它们当时都表现为「看起来像玄学」。这一节把根因、证据和修法记下来，
下次遇到同类症状先查契约，别再写探针。

### 1.29.1 根因表

| # | 症状 | 根因 | 证据位置 |
|---|------|------|----------|
| 1 | 工具条/页签/树点不动 | 引擎 `win.setPos` 的 `after` **只认指针型**，数值 `1`(HWND_BOTTOM) 被吞成 nullptr=TOP，背景板反而压到最上面 | `engine/builtins.cpp` `a[5].t == Value::Pointer` |
| 2 | 右上角窗口按钮点了没反应 | 官方 `win.form` 的窗口过程把 `wParam >= 0xF000` 的 WM_SYSCOMMAND **直接吞掉**，SC_CLOSE/SC_MAXIMIZE 到不了 DefWindowProc | `win/ui/_.aardio:726` |
| 3 | 改个窗口大小就再也不重绘 | 官方 WM_TIMER 在回调返回值「既不是 null 也不是 true」且转数字为 0 时 **`clearInterval` 静默移除定时器** | `win/ui/_.aardio:342` |
| 4 | 组件半天不显示（>30 秒） | 官方 plus 的 WM_PAINT **只画 `rcPaint`**（本次失效区），首次更新区为空 = 什么都没画 | `win/ui/ctrl/plus.aardio:833` |
| 5 | 取客户区尺寸全错 | aardio 的 `::RECT.width/height` 是**属性（数值）**，不是方法；`rc.width()` 是在拿数字当函数调用 → 抛异常 → 被 try/catch 吞掉 → 回落到官方**缓存**值 | `aardio/lib/builtin/struct.aardio:212` |
| 6 | 最大化/还原后黑屏 | **WM_NCCALCSIZE 阶段窗口矩形还没更新**，里面调 `::GetWindowRect` 拿到的是**旧**（最大化时）的矩形；把它写进 lParam → 客户区被钉在屏幕左上角 | Win32 契约 |

### 1.29.2 第 6 条的实测链条

窗口矩形 1943×1287、`GetClientRect` 却报 3860×2180 —— 客户区比窗口还大，
因为窗口过程**真的**把那个尺寸设成了客户区。

```
t=  13ms  zoomed=True   winRect=-10,-10,3850,2170   client=3840x2160
t=1226ms  >>> 点「最大化/还原」
t=1554ms  zoomed=False  winRect=947,405,2890,1692    client=3860x2180   ← 客户区跑到屏幕左上角
```

修法：改用 **WM_WINDOWPOSCHANGING(0x46) 缓存「即将生效」的新窗口矩形**，
WM_NCCALCSIZE 直接用它 —— WINDOWPOS 里 x,y,cx,cy 是偏移 16 起的连续 4 个 int，
用 `::RECT()` 一次读 16 字节正好对上：

```aardio
var pr = ..raw.convert(topointer(lp) + 16, ::RECT());   // left=x top=y right=cx bottom=cy
```

「是不是最大化」也改用**新尺寸 vs 屏幕尺寸**判断，不能用当前样式
（最大化/还原过程中 WS_MAXIMIZE 什么时候清掉并不确定）。

### 1.29.3 修法清单

* `lib/nodio/ui/rect.aardio`：新增 `sendToBack(ctrl)` / `bringToFront(ctrl)`
  （内部 `topointer(1)` + flag `0x13`）。
* `lib/nodio/ui/board.aardio`：新增画板登记表 + `nodio.ui.redrawAll()`；
  `layout()` 尺寸变化时整块重画。
* `lib/nodio/ui/titlebar.aardio`：窗口按钮改走 `::User32.ShowWindow` /
  `::PostMessage(WM_CLOSE)`；按钮矩形按当前画板宽度实时重算；
  **WM_NCCALCSIZE 改用 WINDOWPOS 缓存的新矩形**。
* `ide/nodio/app/shell.nodio`：`clientWH()` 改用实时 `::GetClientRect`；
  背景板创建时和每次 `layout()` 后 `sendToBack`；`layout()` 末尾 `redrawAll()`。
* `ide/nodio/main.nodio`：启动改**分阶段**（先渲染后加载）；
  所有 `setInterval` 回调显式 `return null`（官方会按返回值移除定时器）。

### 1.29.4 验证

* 还原后截图非黑像素 **0 → 123474**，标题栏/工具条/左右页签/工程树/编辑器/状态栏全部正常。
* 客户区尺寸序列 `3840x2160 → 1943x1287`（修复前是 `→ 3860x2180`）。
* 全量回归：VM 89 passed / EDITOR / INPUT / SMOKE / NAV / STDLIB 全绿。

### 1.29.5 教训（这才是这一轮真正的产出）

六个根因**全部**是「grep 一下官方库/引擎源码就能拿到的一行事实」，
但当时是**一个症状一个症状地用黑盒探针反推**出来的 ——
13 个诊断脚本 / 1675 行，换 6 行根因。

**产出比 1:280。** 效率低的根源不是问题难，是**先写探针、后读契约**。

以后的顺序应该是：

1. 异常行为 → 先 grep 同名消息在官方库/引擎里的处理分支；
2. 再查 aardio 的**类型契约**（`::RECT` 这类结构体的字段是属性还是方法）；
3. 再查 Win32 消息契约（发消息时窗口状态是否已更新）；
4. 三步都排除，**才**写探针。

## 1.29 第二十八轮（续）：按官方同名路径补 Nodio 自己的 x64 库（6 个模块）

### 1.29.1 路线

x64 上「官方库只带 32 位 PE」其实是**两层**问题：

1. **引擎层**（§1.28 已解决）：x64 的浮点 ABI 全错 —— 修掉。
2. **库层**（本节）：官方那份 32 位 PE 本身装不进 x64 进程 —— 按官方
   **同名路径**在 Nodio 侧提供自己的 x64 版本。

Nodio 的 overlay 机制正好支持第 2 条：`engine/import.cpp` 的
`GetOverlayLibDir()` 把 `D:\Nodio\lib` 放在官方库**之前**查找，且**相对路径
完全一致** —— 于是把 Nodio 的模块放成官方同名同路径，官方代码 `import xxx`
时自动命中 Nodio 的版本，**不需要改任何调用方**。

（x86 构建不命中 overlay —— 32 位构建直接用 aardio 官方 32 位标准库，
行为与官方完全一致。）

### 1.29.2 官方随库发布 C 源码的 6 个模块

官方这 6 个模块的 DLL 是用 `$` 内嵌进源码再内存加载的，而官方**同时把
C 源码放在了 `.res` 目录**（官方自己的 `build.aardio` 就是用它 + tcc/MSVC
重建 DLL 的）。Nodio 侧用 MSVC 把**同一份 C 源码**重新编译成 x64：

| 官方模块 | 官方 C 源码 | Nodio x64 产物 | 导出 |
|---|---|---|---|
| `av.audio.dsp` | `.res/dsp.c` | `.res/dsp64.dll` | 38 个 `a_*` |
| `win.region.png` | `.res/build.aardio` 内嵌 C | `.res/c64.dll` | `PngDataScan0ToRegion` |
| `win.region.bitmap` | `.res/build.aardio` 内嵌 C | `.res/c64.dll` | `BitmapToRegion` |
| `gdip.feather` | `.res/build-feather.aardio` 内嵌 C | `.res/feather64.dll` | `copyAlpha` |
| `raw.argsPointer` | `.res/build.aardio` 内嵌 C + `.def` | `.res/argsPointer64.dll` | `getArgsPointer` |
| `sys.audioVolume` | `.res/volumeCtrl2.cpp` / `.h` | `.res/volumeCtrl64.dll` | `CreateVolumeCtrl` 等 6 个 |

编译口径：x64、`/MT` 静态 CRT（不依赖外部运行库）、`/O2`；region 两兄弟
还要 `/link gdi32.lib user32.lib`，audioVolume 的 COM 要
`/link ole32.lib winmm.lib uuid.lib`。

> `sys.audioVolume` 的官方头文件里有一处**类内限定名**声明
> （`IAudioMeterInformation * CVolumeCtrl2::GetMeterInfomation();`），
> MSVC 拒绝 —— 重建时去掉限定名即可，行为不变。

overlay 模块本身是官方模块的**逐字副本**，只把 DLL 的来源换成
`io.splitpath(io.libpath("<模块名>")).dir + ".res\<x64 dll>"`：

```aardio
// 官方：var dll = ..raw.loadDll( $"~/lib/win/region/png/.res/c.dll","win.region.png.dll" );
var dll = ..raw.loadDll( io.splitpath(io.libpath("win.region.png")).dir + ".res\\c64.dll","win.region.png.dll" );
```

用 `io.libpath()` 定位自身目录（与 import 同一套查找顺序），部署到任何位置
都能找到同目录的 x64 DLL。

### 1.29.3 验证

* **导入**：6 个模块在 x64 上全部导入成功（此前 5 个因 32 位 DLL 直接失败）。
* **数值一致**：`av.audio.dsp` 的 x64 overlay 版与 x86 官方版对同一组输入
  给出**逐项相同**的结果；`win.region.png.PngDataScan0ToRegion` 在假扫描
  数据上返回有效 HRGN（`type == "pointer"`）。
* **回归锁**：新建 `tests/x64_overlay_check.ps1`（9 项断言，x64 / x86 双绿）——
  6 个模块的导入 + 导出函数可调用 + dsp 数值 + region 返回值。
* **全量**：x64 / x86 各 17 套既有回归全绿；x64 全量模块 sweep
  **ok=899 → 909 / fail=70 → 60**（**-10**，失败集合**只减不增**，新增失败 0 项）。

修复的 10 项：`av.audio` / `av.audio.dsp` / `av.audio.mixer` / `av.audio.music` /
`av.audio.sfx` / `gdip.feather` / `raw.argsPointer` / `sys.audioVolume` /
`win.region.bitmap` / `win.region.png`。

### 1.29.4 剩下的 60 项 x64 失败（分类）

| 类别 | 项数 | 说明 |
|---|---:|---|
| 官方只带 32 位 DLL（**无源码**） | 24 | `av.video.kernel` 族 4、`web.view` 族 4、`string.markdown` 族 8、`sevenZip.lzma` 族 4、`raw.fasm` 1、`string.keywords` 1、`web.layout` 1、`com.autoComplete` 1 |
| Python 宿主 32 位（`python27.dll` / `python3.dll`） | 13 | `py2.*` / `py3.*`（x86 下 `import py2` 可用） |
| 官方契约：必须先导入父命名空间 | 6 | `sqlite.base` / `sqlite.backup` / `web.form.query` / `web.acme.httpClient` / `web.view.dev` / `winex.accObject.element` |
| 官方库自带安装器 / 下载器（超时） | 6 | `fastcgi.iisInstall` / `java.*` 3 / `process.php.*` 2 |
| 官方库依赖的官方库缺失 | 3 | `crypt.jwt.algorithm.EdDSA` / `process.ruby` 2 |
| 官方库自身断言失败 | 1 | `web.layout.event`（HTMLayout 未装的连带） |
| 官方同款导入顺序限制 | 2 | `win.ui.ctrl.splitter` / `win.ui.ctrl.thread`（官方同样要求先 `import win;`，见 E-8） |
| sweep 清单伪条目 | 1 | `win.#DEFINE`（官方文件名带 `#`，不能出现在 import 名里） |

**没有一项是 Nodio 自身的引擎缺陷。**

### 1.29.5 下一批的可行路径（官方没发源码的那 24 项）

| 模块 | 可行方案 |
|---|---|
| `string.markdown` 族（8 项） | sundown 是公开的 CommonMark 风格 C 库，官方未随库发源码 → **在 Nodio 侧用 aardio 源码重写** markdown 渲染器，API 面按官方 `sd_markdown_*` / `sdhtml_*` 对齐 |
| `string.keywords`（1 项） | 纯算法（关键词抽取）→ Nodio 侧自写 |
| `web.layout`（1 项） | HTMLayout 有 64 位商业版，需许可证；或换用 WebView2 重写（工作量极大） |
| `web.view` 族（4 项） | 同上，依赖 `webview.dll` |
| `av.video.kernel` 族（4 项） | 体量大、依赖专有视频 SDK → x64 上明确标注不支持 |
| `sevenZip.lzma` 族（4 项） | 7-Zip SDK（LGPL）可换 64 位版；但 Nodio 已有 `lib/zlib` 方案，需评估 API 兼容性 |
| `raw.fasm`（1 项） | FASM 源码公开（GPL），可换 64 位构建 |
| `com.autoComplete`（1 项） | 依赖外部安装的 `autoComplete.dll`（`%LOCALAPPDATA%\aardio\std\com\`），属环境缺件 |

## 1.30 第二十九轮（续）：py2 / py3 子模块 —— 按官方同名路径补 Nodio 自己的 x64 版本（13 项）

### 1.30.1 官方 py2 / py3 的结构与两条硬约束

```txt
lib/py2/_.aardio                    父模块：载入 python27.dll，导出全局保留常量 ::Python2
lib/py2/{dict,export,list,module,object,tuple}.aardio      子模块
lib/py3/_.aardio                    父模块：载入 python38.dll，导出 ::Python3
lib/py3/{dict,export,list,module,object,tuple}.aardio      子模块
```

两条约束都从**官方源码直接读出**，不是推测：

1. **宿主 DLL 只有 32 位。** 官方 `lib/py2/.res/DLLs/python27.dll` 与
   `lib/py3/.res/DLLs/python38.dll` 的 PE `machine` 都是 `0x014C`（x86）。
   x64 进程装不下 → `::Python2` / `::Python3` 为空。
   Nodio 仓库内另备了一份 **64 位** `lib/py3/.res/DLLs/python38.dll`
   （`machine == 0x8664`，含整套 64 位 `.pyd`），所以 **py3 在 x64 上可用、py2 不可用**。

2. **子模块约定「必须先 import 父命名空间」。** 子模块在**类体作用域**里直接
   `::Python3.api("PyDict_New","pyObject()","cdecl")`（`py3/dict.aardio:154`），
   父命名空间没载入时 `::Python3` 为空 → 类体求值报错。
   官方自己也遵守这条约定：`py3/_.aardio:49-53` 自己
   `import ..py3.object/module/dict/list/tuple`；而 `py3/pip.aardio` 用的是
   `import py3;`（父命名空间），不是 `import py3.pip`。

实测（x64，官方库直连）：`import py3.dict;` → `尝试调用非函数值 (.api) @ .../py3/dict.aardio:154`；
x86 同样失败 → **与官方同款**，不是 x64 独有的偏差。

### 1.30.2 Nodio 侧的实现（overlay，官方同名路径）

| 文件 | 与官方的差异 |
|---|---|
| `lib/py3/_.aardio` | 官方副本。`::Python3 := ::Py3Runtime \|\| loadDll(...)`，并让 `::Py3Runtime` 与 `::Python3` 指向**同一个** DLL 句柄 |
| `lib/py3/{dict,export,list,module,object,tuple}.aardio` | 官方副本。子模块自己载入 64 位运行时（`::Py3Runtime := ::Py3Runtime \|\| loadDll(...)`），API 绑定从 `::Python3.*` 改绑 `::Py3Runtime.*` → **子模块可独立导入** |
| `lib/py2/{dict,export,list,module,object,tuple}.aardio` | 官方副本。`import ..py2;` 之后若 `::Python2` 为空（64 位无 python27.dll），注册一个**明确报错的占位类**并提前返回 → **导入不崩溃，使用时给出与 `lib/py2/_.aardio` 一致的提示** |

几个关键点：

* **保留常量语义**：`::` 是 aardio 的**保留常量**操作符（全局命名空间下、赋非空值后不可改）。
  `::X := v` 等价于 `::X = ::X or v`，所以六个子模块各自重复执行载入语句是安全的
  （第一次真正加载，之后保持原值，句柄仍然唯一）。
* **`import` 与命名空间作用域的顺序**：`import ..py2;` 必须放在 `namespace py2;` **之前**。
  实测把 `import` 写在 `namespace` 之后，导入语句会把当前命名空间作用域顶掉，
  后面声明的类会落进错误的命名空间（实测 `nsx.sub` 变成空命名空间而不是类）。
* **`py3.export` 例外**：它在**导入期**就要执行 Python 代码
  （`py3/export.aardio:187` 的 `..py3.exec(\`...\`)`），官方同样要求先 `import py3`。
  Nodio overlay 把内部错误「尝试调用非函数值 (.PyModule_Create2P)」换成明确提示
  「py3.export 需要先 import py3 以初始化 Python 运行时」。
  另外给它的命名空间 `_set` 元方法加了 `Py_IsInitialized()` 守卫 ——
  否则 Python 尚未初始化时会调用未初始化的 CPython，**直接让进程崩溃**
  （实测 `Fatal Python error: _PyInterpreterState_Get(): no current thread state`，
  退出码 `0xC0000409`）。这条守卫同时消掉了「导入期崩溃」这个隐患。
* **x86 行为不变**：overlay 目录只在 x64 生效（`engine/import.cpp` 的
  `GetOverlayLibDir()`：32 位构建直接用官方 32 位标准库），
  所以 x86 上 py2 仍能真实调用 32 位 python27.dll（实测 `py2.eval("2+3") == 5`）。

### 1.30.3 验证

* **功能**：新建 `tests/py_overlay_check.ps1`（21 项断言，x64 / x86 双绿）——
  py3 的 `version / exec / eval / dict 往返 / object / list / tuple / module` 全部正常；
  x64 下 py3 六个子模块**可独立导入**（`py3.export` 按官方约定仍要求先 `import py3`）；
  x64 下 py2 `isSupported == false`、`py2.exec` 与 `py2.dict()` 抛出
  「当前 64 位环境未检测到 64 位 python27.dll…」，六个子模块独立导入不崩溃；
  x86 下 py2 真实执行 `py2.eval("2+3") == 5`。
* **全量**：x64 / x86 各 20 套回归全绿（新增 `py_overlay_check` 一套）；
  x64 全量模块 sweep **ok=908 → 919 / fail=60 → 49**（**-11**，
  失败集合**只减不增**，新增失败 0 项）。
* 修复的 11 项：`py2.dict` / `py2.export` / `py2.list` / `py2.module` / `py2.object` /
  `py2.tuple` / `py3.dict` / `py3.list` / `py3.module` / `py3.object` / `py3.tuple`。

### 1.30.4 剩下的 49 项 x64 失败（更新后的分类）

| 类别 | 项数 | 明细 |
|---|---:|---|
| 官方只带 32 位 DLL（**未随库发源码**） | 23 | `av.video.kernel` 族 4（`av.film` / `av.video.flicker` / `av.video.kernel` / `autos.skills.codingFilm`）、`web.view` 族 4、`string.markdown` 族 8、`sevenZip.lzma` 族 4、`raw.fasm` 1、`string.keywords` 1、`web.layout` 1（HTMLayout.dll 缺失） |
| 官方契约：必须先导入父命名空间 / 官方自带守卫 | 12 | `py3.export`（导入期执行 Python 代码，官方同款）、`py3.pip`（官方守卫「只能用于开发环境」）、`sqlite.base` / `sqlite.backup` / `web.acme.httpClient` / `web.form.query` / `web.layout.debug` / `web.layout.valueObject` / `web.layout.element` / `web.layout.element.style` / `web.view.dev` / `winex.accObject.element` |
| 官方同款导入顺序限制 | 2 | `win.ui.ctrl.splitter` / `win.ui.ctrl.thread`（官方同样要求先 `import win;`） |
| 官方库自带安装器 / 下载器（超时） | 6 | `fastcgi.iisInstall` / `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` / `process.php.7.4` / `process.php.8.3` |
| 官方库依赖的官方库缺失 | 3 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）/ `process.ruby` / `process.ruby.simpleHttpServer` |
| 官方库自身断言失败 | 1 | `web.layout.event`（HTMLayout 未装的连带） |
| 依赖外部安装的官方 DLL | 1 | `com.autoComplete`（`%LOCALAPPDATA%\aardio\std\com\autoComplete.dll`） |
| sweep 清单伪条目 | 1 | `win.#DEFINE`（官方文件名带 `#`，不能出现在 import 名里） |

**仍然没有一项是 Nodio 自身的引擎缺陷。** 下一批的目标是「官方没发源码的那 23 项」——
`string.markdown` 族 8 项 + `string.keywords` 1 项是纯算法，
可在 Nodio 侧用 aardio 源码重写（API 面按官方 `sd_markdown_*` / `sdhtml_*` 对齐）。

## 1.31 第二十九轮（续）：string.keywords —— 官方没发源码的模块，Nodio 侧自写 x64 版本（1 项）

### 1.31.1 官方形态与「不能复刻」的理由

官方 `lib/string/keywords/_.aardio` 把 32 位 `keywords.dll` 用
`$"~/lib/string/keywords/.res/keywords.dll"` **内嵌进源码**再内存加载。
x64 进程装不下这个 32 位 PE —— 实测报错：

```txt
无法加载 DLL: (内嵌的内存映像)
```

官方 `.res` 目录里**只有二进制、没有 C 源码**（与 `av.audio.dsp` 那 6 个模块不同），
所以这一项无法像 `av.audio.dsp` 那样「同一份源码重编成 x64」，
只能**在 Nodio 侧自己写一份实现**。

官方 DLL 的导出表实测（`dumpbin /exports`）只有 5 个符号，ordinal 1..5 ——
`findOne` / `findAll` / `anyMatchLines` / `allMatchLines` / `noneMatchLines`。

### 1.31.2 官方语义（x86 上跑官方 32 位 DLL 实测，作为基准）

| API | 官方实测语义 |
|---|---|
| `findOne(data,len,pKw,pSz,n)` | 返回**在数据里出现位置最靠前**的关键字的 **1 基**索引；出现位置相同时取关键字表里下标最小的那个；没有命中返回 0；空数据返回 0 |
| `findAll(...)` | 全部关键字都命中返回 0（官方模块用 `0 == findAll(...)` 转成 true/false），否则返回非 0；**空数据返回 true**；**关键字表为空也返回 true** |
| `anyMatchLines` / `allMatchLines` / `noneMatchLines` | 逐行回调。**只按 `\n` 切行**（孤立 `\r` 不分行）；**关键字匹配用未剥离的原始行**；**回调报告的行长度会去掉行末所有的 `\r`**；数据以 `\n` 结尾时不再多出一个空行；**关键字表为空时三个函数都一行都不回调** |

回调原型：`void (__cdecl *)(const char* p, int len)` —— `p` 指向行首，`len` 为该行长度
（不含行尾换行符）。

官方 32 位 DLL 上的实测样例（x86）：

| 输入 | 官方结果 |
|---|---|
| `findOne({"alpha","beta","gamma"}, "gamma and alpha")` | `3` |
| `findOne({"alpha","beta","gamma"}, "alpha and gamma")` | `1` |
| `findOne({"zebra","apple"}, "apple zebra")` | `2` —— **按数据位置，不是按关键字表顺序** |
| `findAll("")` | `true` |
| 关键字表为空时的 `findAll("abc")` | `true` |
| 关键字表为空时的 `anyMatchLines` / `allMatchLines` / `noneMatchLines` | 三个都返回**空数组** |
| `anyMatchLines('ax\ry\rcz')`（关键字 `x`） | 1 行，长度 7 —— 孤立 `\r` 不分行 |
| `noneMatchLines('abc\r')` | 1 行，长度 3 |
| `noneMatchLines('a\r\r')` | 1 行，长度 1 —— 行末两个 `\r` **都**去掉 |
| `noneMatchLines('a\r\n\r\nb')` | 3 行，长度 1 / 0 / 1 —— 中间空行保留 |
| `noneMatchLines('a\n\rb\n')` | 2 行，长度 1 / 2 —— 行内的 `\r` 保留 |
| 关键字 `"x\r"` 匹配 `'x\r\n'` 的第一行 | 命中，报告长度 1 —— **匹配用原始行，报告长度去掉 `\r`** |

### 1.31.3 Nodio 侧的实现（overlay，官方同名路径）

| 文件 | 内容 |
|---|---|
| `lib/string/keywords/_.aardio` | 官方模块**逐字副本**，只把 DLL 来源换成 `..raw.loadDll(io.splitpath(io.libpath("string.keywords")).dir + ".res\\keywords64.dll","string.keyword.dll","cdecl")` |
| `lib/string/keywords/.res/keywords64.c` | **Nodio 自写实现**（源码入库，可复现）。关键字比较用 `memcmp` 逐字节做 —— 二进制安全，不受 UTF-8 限制 |
| `lib/string/keywords/.res/keywords64.dll` | 上面源码用 MSVC 编出的 x64 DLL（`/LD /O2 /MT /GS-`），导出 5 个符号与官方同名同签名同调用约定 |

overlay 机制（`engine/import.cpp`）：x64 下 `GetOverlayLibDir()` 依次找
`NODIO_OVERLAY_LIBDIR` → `<exeDir>\lib` → `<exeDir>\..\lib` → `D:\Nodio\lib`；
`ResolveModulePath()` 里 **overlay 的全部候选路径优先于官方库** →
官方代码 `import string.keywords` 自动命中 Nodio 的版本，**调用方零改动**。
x86 构建不命中 overlay（32 位构建直接用官方 32 位标准库），行为与官方完全一致
（实测 x86 的 `io.libpath("string.keywords")` = `D:\aardio\lib\string\keywords\_.aardio`，
x64 = `D:\Nodio\lib\string\keywords\_.aardio`）。

### 1.31.4 踩到的坑（都是实测）

* **aardio 双引号是「原样串」**：`"\r\n"` 是 4 个字面字符（`\` `r` `\` `n`），**不是换行**。
  测换行语义必须用单引号 `'\r\n'` —— 否则测的是「一行里含有字面量 \r\n」，
  官方行为会被整个测歪（本轮踩了）。
* **PowerShell `Compare-Object` 的 `=>` / `<=` 方向**在本机与直觉相反（`=>` 落在
  DifferenceObject 上）。任何「谁对谁错」的结论一律以**原始字节**
  （`open(...,'rb')`）为准，不能只看 diff 的输出。
* **空关键字表时 `..raw.buffer({int items[]={}})` 返回空指针**，所以 C 侧不能用
  「指针是否为空」代替「关键字个数为 0」的判断 —— 否则 `findAll` 会与官方**相反**
  （官方空关键字表返回 true）。
* 官方 `keywords.dll` 的导出偏移（`findOne@0x1000` 等）与自写版不同，
  可以用它来确认实际加载的是哪一份二进制。

### 1.31.5 验证

* **黄金样本**：4 组探针（基础语义 / 边界 / 切行规则 / 含 CR 的关键字）在
  x86（官方 DLL）与 x64（自写 overlay）上**输出逐字节相同**（SHA-256 一致）。
* **功能**：新建 `tests/keywords_overlay_check.ps1`（44 项断言，x64 / x86 双绿）——
  断言值全部取自**官方 32 位 DLL 的实测**；另外断言 overlay 三个文件都在库里、
  `keywords64.dll` 的 PE `machine == 0x8664`、以及模块解析路径
  （x64 命中 Nodio overlay，x86 走官方库）。
* **全量**：x64 / x86 各 21 套回归全绿（新增 `keywords_overlay_check` 一套）；
  x64 全量模块 sweep **ok=919 → 921 / fail=49 → 48**（**-1**，失败集合**只减不增**）。
* 修复的 1 项：`string.keywords`。

### 1.31.6 剩下的 48 项 x64 失败（更新后的分类）

| 类别 | 项数 | 明细 |
|---|---:|---|
| 官方只带 32 位 DLL（**未随库发源码**） | 22 | `av.video.kernel` 族 4（`av.film` / `av.video.flicker` / `av.video.kernel` / `autos.skills.codingFilm`）、`web.view` 族 4、`string.markdown` 族 8、`sevenZip.lzma` 族 4、`raw.fasm` 1、`web.layout` 1（HTMLayout.dll 缺失） |
| 官方契约：必须先导入父命名空间 / 官方自带守卫 | 12 | `py3.export`（导入期执行 Python 代码，官方同款）、`py3.pip`（官方守卫「只能用于开发环境」）、`sqlite.base` / `sqlite.backup` / `web.acme.httpClient` / `web.form.query` / `web.layout.debug` / `web.layout.valueObject` / `web.layout.element` / `web.layout.element.style` / `web.view.dev` / `winex.accObject.element` |
| 官方同款导入顺序限制 | 2 | `win.ui.ctrl.splitter` / `win.ui.ctrl.thread`（官方同样要求先 `import win;`） |
| 官方库自带安装器 / 下载器（超时） | 6 | `fastcgi.iisInstall` / `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` / `process.php.7.4` / `process.php.8.3` |
| 官方库依赖的官方库缺失 | 3 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）/ `process.ruby` / `process.ruby.simpleHttpServer` |
| 官方库自身断言失败 | 1 | `web.layout.event`（HTMLayout 未装的连带） |
| 依赖外部安装的官方 DLL | 1 | `com.autoComplete`（`%LOCALAPPDATA%\aardio\std\com\autoComplete.dll`） |
| sweep 清单伪条目 | 1 | `win.#DEFINE`（官方文件名带 `#`，不能出现在 import 名里） |

**仍然没有一项是 Nodio 自身的引擎缺陷。** 下一批的目标是「官方没发源码的那 22 项」——
`string.markdown` 族 8 项是纯算法，可在 Nodio 侧用 aardio 源码重写
（API 面按官方 `sd_markdown_*` / `sdhtml_*` 对齐）；
`av.video.kernel` / `web.view` / `sevenZip.lzma` / `raw.fasm` / `web.layout`
依赖具体二进制能力，需要单独评估可行性。

## 1.32 第三十轮：编辑器「基本功能」缺口 —— 一个掩码写错，全部 Ctrl 快捷键都是死的

这一轮的目标是用户那句「**还有很多基础功能都不行，缺了很多东西**」。
按「契约优先」的方式做：先查官方库的同名消息/函数契约，再写探针。

### 1.32.1 根因一：`GetKeyState` 的掩码写错 → 编辑器所有 Ctrl 快捷键恒不生效

`lib/nodio/ui/editor.aardio` 里：

```aardio
var s = getKeyState(0x11/*_VK_CONTROL*/);
return (s && (s & 0x8000)) ? true : false;
```

**`GetKeyState` 返回的是「字节」状态**（按下 = `0x80`，抬起 = `0x00`）；
**高字 `0x8000` 只有 `GetAsyncKeyState` 才有**。所以这个判断 **恒为 false**。

后果（不是「少一个快捷键」，是**一整类**）：

| 快捷键 | 作用 | 修复前 |
|---|---|---|
| Ctrl+Z / Ctrl+Y | 撤销 / 重做 | 从不触发（按键被原生 richedit 吃掉） |
| Ctrl+Space | 触发补全 | 从不触发 |
| Ctrl+F / Ctrl+H | 查找 / 替换 | 从不触发 |
| Ctrl+G | 转到行 | 从不触发 |
| Ctrl+/ | 注释切换 | 从不触发（按下去只插一个 `/`） |

判别方法（Win32 文档契约）：`GetKeyState` 的**低字节**表示「当前是否按下」，
**高字节**表示「自上次调用以来是否按过」—— 也就是说高字节在
「按下又松开」之后仍然是 `0xFF`，用高字节判断会把「松开」误判成「还按着」。

修法：`& 0x80`。

> 顺带一提：这个 bug 之所以能潜伏这么久，是因为**探针本身也写错了**：
> 键盘状态数组的下标就是虚拟键码（`VK_SHIFT=0x10` / `VK_CONTROL=0x11`），
> 探针把 `0x80` 写在了**偏移 16**（= VK_SHIFT）而不是 17（= VK_CONTROL）。
> 于是「按住 Ctrl」其实变成了「按住 Shift」，
> `Shift+/` 在美式键盘上正好是 `?` —— 现象是注释功能插进去一个 `?`。
> **两个 bug 叠加**，才让「Ctrl+/ 不工作」看起来像「注释功能实现有问题」。

### 1.32.2 根因二：空串在 aardio 里是**真值**

hover 的「鼠标下不是标识符就不该打扰」判断写成：

```aardio
if(!w || !w.word) return;      // ✗ w.word == "" 时 !w.word == false
```

aardio 里**只有 `null` / `false` 为假**，`""` 是真值。判空必须写 `#w.word == 0`。
同样的坑在 `shell.nodio` 的钩子自检里也踩过一次（已修）。

### 1.32.3 根因三：aardio 单引号串按 C 规则转义 → Windows 路径被吃掉

```aardio
'D:\\Nodio\\engine\\builtins.cpp'   // 正确：反斜杠要写两遍
'D:\Nodio\engine\builtins.cpp'      // ✗ \e 是 ESC(0x1B)、\b 是退格(0x08)
```

写出来的字节序列是
`44 3A 5C 4E 6F 64 69 6F **1B** 6E 67 69 6E 65 **08** 75 69 6C 74 …`。
后果：所有**绝对路径**的写文件/读文件全部失败（`string.save` 返回 false、
`io.exist` 返回 null、`fsys.exists` 返回 false），而**相对路径**正常。
探针里凡是写 Windows 路径，一律用**正斜杠**。

（`\r\n` / `\n` / `\t` / `\xNN` 是常规转义；`\e` → 0x1B、`\b` → 0x08
属于 C 的扩展转义，aardio 同样支持。）

### 1.32.4 补上的基础功能

| 功能 | 位置 | 状态 |
|---|---|---|
| 回车续接缩进（含开括号多一级） | `editor.aardio` `__newlineIndent` | 新增 |
| 括号/引号自动配对 + 闭合跳过 | `editor.aardio` `__autoPair` | 新增 |
| Ctrl+Z / Ctrl+Y（组件自己的撤销栈） | `editor.aardio` WM_KEYDOWN | 新增 |
| Ctrl+/ 注释保留缩进 | `editor.aardio` `toggleComment` | 改写 |
| 查找预填当前选词 | `editor.aardio` `showFind` | 改写 |
| 自动触发补全（`.` / 词长阈值） | `editor.aardio` `__autoComplete` | 新增 |
| 悬停提示（浮层 + LSP） | `editor.aardio` + `main.nodio` | 新增接线 |
| F10 = 继续（`resume`）、F11 = 单步 | `main.nodio` + `shell.nodio` | 改写 |
| 断点停住时光标跳到该行 | `main.nodio` `pollDebug` | 新增 |
| 设计保存/载入（源码 DSG 块 + `.dsg` 文件） | `designer.nodio` + `dsg.aardio` | 新增接线 |
| Ctrl+Tab / Ctrl+Shift+Tab 切标签 | `shell.nodio` | 新增 |

### 1.32.5 验证

* `tests/reg_editor_input.aardio` —— 22 项断言（回车缩进 / 配对 / 跳过 /
  撤销重做 / **Ctrl+Z、Ctrl+Y 真实按键路径** / 注释保留缩进）→ `REG-EDITOR-INPUT-OK`
* `tests/reg_hover.aardio` —— 9 项断言（回调行号列号 / 浮层打开 / markdown 剥离 /
  空白处不打扰 / 收起）→ `REG-HOVER-OK`
* `tests/run_editor_component_tests.ps1` —— 上面两个用例的统一入口
* 全量：VM 90 passed / EDITOR / INPUT / SMOKE / NAV / STDLIB **全绿**
* 视觉：`NODIO_IDE_DEMO_HOVER=1` + `tests/ide_shot.ps1` → 悬停浮层两行渲染正确

### 1.32.6 顺带修掉的两个测试脚本毛病

* `tests/ide_smoke.ps1`：进程刚退出就去读重定向文件会撞上句柄竞态
  （偶发 `FAIL: IDE produced no stdout`）→ 读失败重试。
* `tests/ide_shot.ps1`：窗体**刚建好还没布局**时矩形只有 158×26 且停在屏幕外，
  脚本会抓到这个空壳 → 改成「窗口够大（≥400×300）再抓」。


## 1.33 第三十轮（续）：string.markdown —— 官方没发源码的模块，Nodio 侧自写 x64 版本（8 项）

### 1.33.1 官方形态与「不能复刻」的理由

官方 `lib/string/markdown/_.aardio` 把 32 位 `sundown.dll` 用 `$"..."` **内嵌进源码**
再内存加载。x64 进程装不下这个 32 位 PE —— 实测报错：

```txt
无法加载 DLL: (内嵌的内存映像)
```

官方 `.res/README.TXT` 写明：该组件基于 `https://github.com/vmg/sundown` 改造，
**没有随库发布源码**，且许可条款禁止脱离 aardio 单独分发。
所以这一项既不能像 `av.audio.dsp` 那样「同一份源码重编成 x64」，
也不能照抄官方二进制 —— 只能在 **Nodio 侧自己写一份等价实现**。

官方 DLL 的导出表实测（`dumpbin /exports`）共 **17 个符号**，与上游 sundown 的 API 一致：

| 类别 | 符号 |
|---|---|
| 缓冲区 | `bufcstr` / `bufdata` / `bufgrow` / `bufnew` / `bufprefix` / `bufput` / `bufputc` / `bufrelease` / `bufsize` |
| 解析器 | `sd_markdown_free` / `sd_markdown_new` / `sd_markdown_render` / `sd_version` |
| HTML 渲染 | `sdhtml_is_tag` / `sdhtml_renderer` / `sdhtml_smartypants` / `sdhtml_toc_renderer` |

### 1.33.2 官方 API 面（逐字沿用，不缩水）

```aardio
string.markdown(文本)                      // -> HTML 字符串
string.markdown(扩展,嵌套深度,渲染标志)    // -> 解析器实例
```

实例方法：`render` / `renderCode` / `renderFile` / `free` /
`setRenderFlags` / `setTaskCheckboxEnabled`；
命名空间成员：`dll` / `sd_markdown_new` / `sd_markdown_render` / `sd_markdown_free` /
`sdhtml_renderer` / `sdhtml_smartypants` / `buffer` 类 / `html_renderopt` /
`sd_callbacks` / `sd_callbacks_proto` / `codeBlock`。

### 1.33.3 Nodio 侧的实现（overlay，官方同名路径）

| 文件 | 内容 |
|---|---|
| `lib/string/markdown/_.aardio` | 官方模块**逐字副本**，只把 DLL 来源换成 `..raw.loadDll(io.splitpath(io.libpath("string.markdown")).dir + ".res\\markdown64.dll","markdown64.dll","cdecl")` |
| `lib/string/markdown/.res/markdown64.c` | **Nodio 自写实现**（源码入库，可复现），约 3200 行 |
| `lib/string/markdown/.res/markdown64.dll` | 上面源码用 MSVC 编出的 x64 DLL（`/LD /O2 /MT /GS- /W3 /utf-8`，零警告），导出 17 个符号与官方**同名同签名同调用约定** |

结构体布局必须与 aardio 类**逐字节一致**，否则回调会拿到垃圾：

* `html_renderopt` = `{int toc_data[3]; INT flags; pointer link_attributes;}`
  —— x64 布局 24 字节，`flags` 在偏移 12；
* `sd_callbacks` = 26 个指针，顺序与官方类字段逐项一致
  （含官方的拼写错误字段名 `utolink`，**逐字保留官方行为**）；
* 导出函数的整型参数统一声明为 **32 位 `unsigned int`** ——
  aardio 的 `INT` 是 32 位，x64 下用 `size_t` 接 `INT` 实参会读满 64 位寄存器，
  高位是垃圾。

官方 fork 与上游 sundown 的差异（全部按官方实测对齐）：

| 差异点 | 官方输出 |
|---|---|
| 表格对齐样式 | `style="text-align:left;"`（**带分号**） |
| 围栏代码块 | `<pre><code class="js language-js">` |
| 任务列表 | `<input type="checkbox" [checked] [disabled]>`，启用态用 `<label>` 包裹 |
| 普通文本转义 | `'` → `&#39;`、`"` → `&quot;`、`&` → `&amp;` |

### 1.33.4 踩到的坑（都是实测）

* **Windows 下 `io.print` 会把串里的 LF 转成 CRLF**（CRT 文本模式）。
  用 stdout 抓「官方黄金输出」拿到的是 CRLF，而**真实字符串是 LF** ——
  照着它写回归锁会全红。回归锁改成在 aardio 侧把字符串**按字节编成十六进制**回传。
* **aardio 双引号是「原样串」**：`"a\nb"` 是 5 个字面字符，不是换行；
  测换行语义必须用单引号 `'a\nb'`。
* **官方类里写的是 `stuct toc_data`**（`struct` 少一个 r，官方自身的拼写错误）。
  自己的 overlay 用正确拼写 `struct`，引擎侧另加容错（见下一节）。
* **本机无网络**（`web_fetch` / `web_search` 均失败）→ 拿不到上游 sundown 源码，
  只能按官方行为反推实现。
* **Nodio FFI 支持把带 `_topointer` 的类实例当 `POINTER` 传参**
  （`engine/ffi.cpp`）→ 官方 `buffer` 类实例可以直接传给 `sd_markdown_render`。

### 1.33.5 验证

* **61 例黄金电池**：从**官方 32 位 sundown 的实测**采集
  （ATX/setext 标题、段落、强调×3、代码段、围栏、缩进代码、引用、
  列表×3、嵌套列表、表格与对齐、链接/图片/引用式链接、自动链接、
  裸链接、裸邮箱、任务列表 4 种形态、HTML 块、行内强调、转义、实体、
  Front Matter、硬换行、撇号/反斜杠/尖括号…），
  x86（官方）与 x64（自写）**逐字节完全一致**。
* **回归锁**：新建 `tests/markdown_overlay_check.ps1`（x64 / x86 双绿）——
  61 例渲染用例 + 16 项 API 面断言 + overlay 文件存在性 +
  `markdown64.dll` 的 PE `machine == 0x8664` + 17 个导出符号齐全 +
  模块解析路径（x64 命中 Nodio overlay，x86 走官方库）。
* **全量**：x64 / x86 各 **22 套回归全绿**（新增 `markdown_overlay_check` 一套）。
* **sweep**：x64 全量模块 sweep **ok=921 → 929 / fail=48 → 40**（**-8**，失败集合**只减不增**）。
  修掉的 8 项 = `string.markdown` 族 8 项：
  `string.markdown` / `string.markdown.toDocx` / `string.markdown.toRtf` /
  `autos.ui.memories` / `autos.ui.setting` / `web.form.chat` /
  `web.form.simpleMarkdown` / `win.dlg.flashDict`。

### 1.33.6 本轮挖出并修掉的一个实现级 bug

`rndr_paragraph` 的 **hard-wrap 分支漏了 `i++`**：段落里出现**真实换行**时
会无限追加 `<br>`，直到 `realloc` 失败，进程被 `exit(1)` 掉 ——
实测 stderr 是 `markdown64: out of memory`，而 x86 官方同一输入正常输出
`<p>a<br>\nb</p>`。

这个 bug **61 例电池抓不到**（电池用默认渲染标志，段落里没有真实换行），
是回归锁的 API 面断言（`setRenderFlags(0x80, ...)` 开 hard wrap）把它挖出来的 ——
**这正是「黄金电池 + 行为断言」两层锁的意义**。

### 1.33.7 剩下的 40 项 x64 失败（更新后的分类）

| 类别 | 项数 | 明细 |
|---|---:|---|
| 官方只带 32 位 DLL（**未随库发源码**） | 14 | `av.video.kernel` 族 4（`av.film` / `av.video.flicker` / `av.video.kernel` / `autos.skills.codingFilm`）、`web.view` 族 4、`sevenZip.lzma` 族 4、`raw.fasm` 1、`web.layout` 1（HTMLayout.dll 缺失） |
| 官方契约：必须先导入父命名空间 / 官方自带守卫 | 12 | `py3.export`（导入期执行 Python 代码，官方同款）、`py3.pip`（官方守卫「只能用于开发环境」）、`sqlite.base` / `sqlite.backup` / `web.acme.httpClient` / `web.form.query` / `web.layout.debug` / `web.layout.valueObject` / `web.layout.element` / `web.layout.element.style` / `web.view.dev` / `winex.accObject.element` |
| 官方同款导入顺序限制 | 2 | `win.ui.ctrl.splitter` / `win.ui.ctrl.thread`（官方同样要求先 `import win;`） |
| 官方库自带安装器 / 下载器（超时） | 6 | `fastcgi.iisInstall` / `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` / `process.php.7.4` / `process.php.8.3` |
| 官方库依赖的官方库缺失 | 3 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）/ `process.ruby` / `process.ruby.simpleHttpServer` |
| 官方库自身断言失败 | 1 | `web.layout.event`（HTMLayout 未装的连带） |
| 依赖外部安装的官方 DLL | 1 | `com.autoComplete`（`%LOCALAPPDATA%\aardio\std\com\autoComplete.dll`） |
| sweep 清单伪条目 | 1 | `win.#DEFINE`（官方文件名带 `#`，不能出现在 import 名里） |

**仍然没有一项是 Nodio 自身的引擎缺陷。** 下一批的目标是「官方没发源码的那 14 项」——
`av.video.kernel` / `web.view` / `sevenZip.lzma` / `raw.fasm` / `web.layout`
都依赖具体二进制能力，需要逐个评估可行性。

## 1.34 第三十轮（续）：官方 `stuct` 拼写导致结构体布局错位 —— 一个**引擎级**缺陷（与架构无关）

### 1.34.1 缺陷（真实）

官方 `lib/string/markdown/_.aardio` 的 `html_renderopt` 类里写的是：

```aardio
class html_renderopt {
	stuct toc_data = {          // <-- 官方自己的拼写错误（struct 少了一个 r）
		int header_count;
		int current_level;
		int level_offset;
	};
	INT flags;
	pointer link_attributes;
}
```

aardio 容忍这个写法。Nodio 的解析器用 `IsStructType()` 判断「这是不是一个结构体成员声明」，
`stuct` 不在名单里 → **这条声明整个从布局里消失** → 类的内存布局随之错位。

**这个缺陷与架构无关**：x86 走官方库文件一样中招（x86 是唯一能跑官方 sundown 的架构，
也正是它把缺陷暴露出来）。

### 1.34.2 实测（pre-fix 引擎 vs 修复后引擎）

pre-fix 引擎用 `git archive HEAD` 导出到**独立目录**单独构建，与仓库工作区完全隔离
（不动工作区、不动仓库 git 状态）。

| 测量项 | pre-fix | 修复后 |
|---|---|---|
| x64：`struct` 正确拼写的类，序列化长度 | 24 | 24 |
| x64：`stuct` 拼写的类，序列化长度 | **16** | **24** |
| x64：`stuct` 的 `flags` 落点 | 偏移 0 | 偏移 12 |
| x86：官方 `string.markdown.html_renderopt()` 的 `raw.buffer` 长度 | **8** | **20** |
| x86：官方库渲染 `[a](http://x.com)` | **崩溃 `0xC0000005`** | `<p><a href="http://x.com">a</a></p>` |

### 1.34.3 根因链

1. **解析**：类体里的 `stuct toc_data = {...}` 因为 `stuct` 不是已知结构体类型名，
   不被当作「结构体成员声明」→ 类的 `_struct` 布局串里**没有这个嵌套结构体**；
2. **布局**：嵌套结构体（3 个 int = 12 字节）不存在 → `flags` 落到偏移 0，
   `link_attributes` 落到偏移 4，整个类只有 8/16 字节；
3. **C 侧**：sundown 按「24 字节 / `flags` 在偏移 12 / `link_attributes` 在偏移 16」读写
   → **越界读**，`flags` 拿到的是堆上的垃圾 → XHTML 位（`0x100`）随机
   → 同一个输入的渲染结果在 `<hr>` / `<hr/>` 之间跳变；
4. **越界访问**：`link_attributes` 在偏移 16，而缓冲区只有 8（x86）/ 16（x64）字节
   → 渲染含 `://` 的链接时直接 **0xC0000005 崩进程**（实测）。

### 1.34.4 修法（引擎两处，缺一不可）

| 位置 | 改法 | 作用 |
|---|---|---|
| `engine/common.h` | `IsStructType()` 的类型名表里加 `L"stuct"` | 让**解析器**把 `stuct toc_data = {...}` 认成结构体成员声明 |
| `engine/ffi.cpp` | `ParseStructDef()` 解析 `_struct` 布局串时把 `stuct` 归一化成 `struct` | 让**布局计算**按嵌套结构体（12 字节）算，而不是按未知标量算 |

### 1.34.5 回归锁

新建 `tests/struct_alias_check.ps1`（x64 / x86 双绿）：

* `stuct` 与 `struct` 两种拼写，序列化出来的**字节完全相同**；
* 布局长度 = 16 + 指针宽度，`flags` 落在偏移 12（两个架构都是 12）；
* 官方 `string.markdown.html_renderopt` 的布局同上
  （x86 走官方库文件 —— 那个文件里写的就是 `stuct`）。

在 **pre-fix 引擎**上跑这个回归 → **3 项失败**（`16 ≠ 24` / 字节不同 / `4 ≠ 16`），
证明它真的锁住了这个缺陷，而不是「一直绿」的假回归。

### 1.34.6 影响面与教训

这个坑的通用形态是：**官方源码里的任何拼写错误，只要落在「类型名」位置，
Nodio 就会把它当成未知标量，结构体布局随之错位**。后续补 x64 版本时，
凡是遇到官方自造拼写（`stuct` 这种），都要按同样方式在引擎侧容错，
而不是在自己的 overlay 里绕开 —— 否则 x86 这条唯一能跑官方库的路径会一直错下去。

## 1.35 第三十轮（续）：av.video.kernel —— 官方**随库发布 C 源码**的模块，重编 x64 即可（4 项）

### 1.35.1 官方形态与关键发现

官方 `lib/av/video/kernel.aardio` 把 32 位 `kernel.dll` 用 `$"..."` 内嵌进源码再内存加载，
x64 进程装不下 —— 但这一项**不需要自写实现**：

官方 `D:\aardio\lib\av\video\.res\kernel.c`（28KB）**随库发布了 C 源码**，
文件头注释写得很清楚：

```txt
C 源码在 .res/kernel.c，改完运行 .res/build.aardio 重建 kernel.dll（tcc 编译）；
库（../kernel.aardio）用 $ 把 kernel.dll 编译进代码并内存加载，运行时不需要 tcc。
```

全库扫描（`D:\aardio\lib` 下所有 `.res` 目录）**只有两个 C 源码**：

| 源码 | 状态 |
|---|---|
| `av/audio/.res/dsp.c` | 已补（`av.audio.dsp` 那批，5 项） |
| `av/video/.res/kernel.c` | 本节补（4 项） |

所以「官方没发源码」的三类里，`av.video.kernel` 属于**第三类：有源码 → 直接重编**，
和 `av.audio.dsp` 同款路径。

### 1.35.2 官方 API 面

13 个导出（`dumpbin /exports` 实测，官方 DLL 与自编 DLL **同名同顺序**）：

```txt
v_bg  v_blend  v_blit  v_bounds  v_boxMean  v_clear  v_flickerScore
v_glitch2  v_grain  v_lumaStats  v_motionCheck  v_over  v_resample
```

aardio 侧的封装逐字沿用官方 `kernel.aardio`：
`background` / `resample` / `over` / `clear` / `bounds` / `blend` / `grain` / `blit` /
`glitch` / `glitchMix` / `lumaStats` / `boxMean` / `flickerScore` / `motionCheck`。

依赖链（补一个模块修好 4 项）：

```txt
av.video.kernel  ←  av.film  ←  autos.skills.codingFilm
                 ←  av.video.flicker
```

### 1.35.3 Nodio 侧的实现（overlay，官方同名路径）

| 文件 | 内容 |
|---|---|
| `lib/av/video/kernel.aardio` | 官方模块**逐字副本**，只把 DLL 来源换成 `io.splitpath(io.libpath("av.video.kernel")).dir + ".res\\kernel64.dll"` |
| `lib/av/video/.res/kernel64.dll` | 官方 `.res/kernel.c` 用 MSVC 编出的 x64 DLL（`/LD /O2 /MT /GS- /W3 /utf-8`，**零警告**），13 个导出与官方同名同顺序 |

### 1.35.4 验证

* **13 个导出全覆盖的黄金探针**：同一段 aardio 代码把每个内核的输出缓冲
  按十六进制全量回传，x86（官方 32 位 DLL）与 x64（自编 DLL）
  **逐字节完全一致**（37263 字符全等）。
* **回归锁**：新建 `tests/av_video_kernel_check.ps1`（x64 / x86 双绿）——
  14 项像素级黄金值（`v_bg` 线性/径向、`v_grain`、`v_bounds`、`v_clear`、`v_over`、
  `v_blend`、`v_blit`、`v_glitch2`、`v_resample`、`v_lumaStats`、`v_boxMean`、
  `v_flickerScore`、`v_motionCheck`）+ overlay 文件存在性 + PE `machine == 0x8664` +
  13 个导出符号齐全 + 模块解析路径（x64 命中 overlay，x86 走官方库）。
* **sweep**：x64 全量模块 sweep **ok=929 → 933 / fail=40 → 36**（**-4**，失败集合**只减不增**）。
  修掉的 4 项：`av.film` / `av.video.flicker` / `av.video.kernel` / `autos.skills.codingFilm`。

### 1.35.5 踩到的坑

* **`raw.buffer(...)` 指针不能按字段写**：`P.v[1] = 3.5` 对 raw 缓冲指针**无效**
  （读回来是 null）—— 往缓冲区塞浮点数组必须走
  `raw.buffer({double v[] = {...}})`，否则探针其实在测「全零输入」。
* **官方 DLL 是 tcc 编的 32 位版**：tcc 用 x87 算浮点，MSVC x64 用 SSE2，
  理论上最后一位可能有差异；实测这 13 个内核的输出**完全一致**
  （代码里几乎所有浮点都立刻量化成 8 位整数，误差被吃掉了）。
* **MSVC 编译官方 C 源码零警告**：`kernel.c` 只依赖 `math.h` / `string.h` / `stdlib.h`，
  没有 32 位假设（像素偏移都用 `(u32)` 显式转换）。

### 1.35.6 剩下 36 项 x64 失败（更新后的分类）

| 类别 | 项数 | 明细 |
|---|---:|---|
| 官方只带 32 位 DLL（**未随库发源码**） | 13 | `web.view` 族 4（`webview.dll`，第三方 WebView2 封装）、`sevenZip.lzma` 族 4（`LzmaUtil.dll`）、`raw.fasm` 1（`FASM.DLL`，汇编器）、`web.layout` 族 4（`HTMLayout.dll` 是第三方 32 位商业库，这 4 项全是它的连带） |
| 官方契约：必须先导入父命名空间 / 官方自带守卫 | 12 | `py3.export`、`py3.pip`、`sqlite.base` / `sqlite.backup`、`web.acme.httpClient`、`web.form.query`、`web.layout.debug` / `web.layout.valueObject` / `web.layout.element` / `web.layout.element.style`、`web.view.dev`、`winex.accObject.element` |
| 官方同款导入顺序限制 | 2 | `win.ui.ctrl.splitter` / `win.ui.ctrl.thread` |
| 官方库自带安装器 / 下载器（超时） | 6 | `fastcgi.iisInstall` / `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` / `process.php.7.4` / `process.php.8.3` |
| 官方库依赖的官方库缺失 | 3 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）/ `process.ruby` / `process.ruby.simpleHttpServer` |
| 官方库自身断言失败 | 1 | `web.layout.event`（HTMLayout 未装的连带） |
| 依赖外部安装的官方 DLL | 1 | `com.autoComplete` |
| sweep 清单伪条目 | 1 | `win.#DEFINE` |

**仍然没有一项是 Nodio 自身的引擎缺陷。** 剩下真正需要动手的是 13 项「官方只有 32 位 DLL、
没发源码」的：`web.view`（第三方 WebView2 封装）、`sevenZip.lzma`（LZMA 编解码）、
`raw.fasm`（x86 汇编器）、`web.layout`（第三方 HTML 渲染引擎，许可与体量都不现实）。

## 1.36 收官盘点：用 x86 对照 sweep 把「官方同款行为」摘出去 —— x64 真正还差 11 项，不是 36 项

### 1.36.1 为什么要做这个对照

之前对 x64 失败的分类是**按报错文本人工归因**的（"官方契约守卫 12 项"、"官方同款导入顺序 2 项"…），
归因是人写的，就可能写错。更硬的口径是：

> **同一个模块，x86（走官方 32 位标准库）也失败吗？**
>
> * 两边都失败、报错一致 → **官方同款行为**，不是 x64 缺口，不用做任何事；
> * 只有 x64 失败 → **真正的 x64 缺口**，需要补 x64 版本。

做法：两个架构各跑一次全量 sweep（969 个模块，同一份清单 `tests/public_lib_modules.txt`），
再逐模块比对**状态 + 报错文本**。

### 1.36.2 实测结果

| 分类 | 项数 | 说明 |
|---|---:|---|
| **x64 专属失败（真缺口）** | **11** | 见 §1.36.3 |
| 两边都失败、报错一致（官方同款行为） | 24 | 12 项官方契约守卫（`sqlite.base` / `sqlite.backup` / `py3.pip` / `web.acme.httpClient` / `web.form.query` / `web.layout.*` / `web.view.dev` / `winex.accObject.element`）+ 2 项官方同款导入顺序（`win.ui.ctrl.splitter` / `win.ui.ctrl.thread`）+ 6 项自带安装器/下载器超时（`fastcgi.iisInstall` / `java.jdk.*` / `java.jre.v8ora` / `process.php.*`）+ 3 项官方库缺失（`crypt.jwt.algorithm.EdDSA` / `process.ruby*`）+ 1 项官方库自身断言（`web.layout.event`）+ 1 项清单伪条目（`win.#DEFINE`） |
| 两边都失败、报错不同 | 1 | `py3.export`：x64 报「需要先 import py3 以初始化 Python 运行时」（Nodio overlay 给的明确提示），x86 报官方的「尝试调用非函数值 (.PyModule_Create2P)」—— 都是失败，x64 的提示更友好 |
| x64 通过、x86 失败 | 11 | `py2.*` / `py3.*` 共 11 个子模块：官方要求「先 import 父命名空间」，x86 走官方库就是这个行为；Nodio 的 x64 overlay 让子模块**可独立导入**（§1.30 有意为之） |

**两次 sweep 的总数完全一样（都是 ok=933 / fail=36），但含义完全不同。**
总数相同是因为 x64 补版修好了 12 项、同时 x86 那边本来就有的失败项两边一样多。

### 1.36.3 真正的 x64 缺口：11 项，分 5 个模块族

| 模块族 | 项数 | 根因（实测） | 可行性 |
|---|---:|---|---|
| `sevenZip.lzma` 族 | 4 | 官方 `LzmaUtil.dll`（32 位 / 76KB / 3 个导出 / **无源码**）→ `sevenZip.lzma` / `.httpFile` / `.tar` / `fsys.update.maker` | **可自写**：LZMA 编解码器（解码器 ~400 行 + 编码器 ~800 行 C），用官方 DLL 的输出做**交叉验证**（我的编码 → 官方解码；官方编码 → 我的解码） |
| `web.view` 族 | 4 | 官方 `webview.dll`（32 位 / 381KB / 26 个导出，第三方 WebView2 封装）→ `web.view` / `autos.skills.videoDownloader` / `.bilibili` / `.toutiao` | 需要自己写 WebView2 的 COM 绑定（工作量大）；本机已装 WebView2 运行时 `155.0.4283.62` |
| `raw.fasm` | 1 | 官方 `FASM.DLL`（32 位 / 98KB / 3 个导出，Flat Assembler，**无源码**） | 可自写，但**工作量最大**（要实现一个 x86 汇编器） |
| `web.layout` | 1 | `HTMLayout.dll`（**第三方 32 位商业库**，2.4MB / 100+ 导出） | **不现实**（许可 + 体量）；它连带的 3 项在 x86 上同样失败 |
| `com.autoComplete` | 1 | `%LOCALAPPDATA%\aardio\std\com\autoComplete.dll`（实测存在，**32 位**，由官方安装器安装） | 官方安装器只发 32 位版；x64 要么自写要么另找来源 |

### 1.36.4 结论

* **x64 的真实缺口是 11 项**（不是 36 项）；
* 其中 **9 项可解**：`sevenZip.lzma` 族 4（自写 LZMA）+ `raw.fasm` 1（自写汇编器）+
  `com.autoComplete` 1（自写）+ `web.view` 族 4（自写 WebView2 绑定）；
* **1 项不现实**：`web.layout`（第三方 32 位商业库），它连带的 3 项在 x86 上同样失败；
* 剩下的 **24 项不需要做任何事** —— 那是官方模块自己的契约（守卫 / 导入顺序 / 自带安装器 /
  依赖缺失），Nodio 复现得**一模一样**（报错文本逐字相同）。

## 1.37 第三十一轮：overlay 收口后全量复测 —— x64 自有缺口归零（ok=943 / fail=26）

### 1.37.1 实测

§1.36.3 定性的 9 项可解缺口随后全部以 overlay 落地（扩展名已迁移 `.nodio`，
见 `tests/*_overlay_check.ps1` 六套回归锁）：`sevenZip.lzma` 族 4、`web.view` 族 4、
`raw.fasm` 1、`com.autoComplete` 1。全量 969 模块 sweep 复测：

| | 基线（§1.36） | 本轮实测 |
|---|---:|---:|
| x64 ok | 933 | **943**（+10） |
| x64 fail | 36 | **26**（新增失败 0） |

### 1.37.2 剩余 26 项逐项定性（零 Nodio 自有缺陷）

| 分类 | 项数 | 明细 |
|---|---:|---|
| 官方库自身缺依赖 | 3 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）、`process.ruby` / `process.ruby.simpleHttpServer`（`sevenZip.decoder2.httpFile` 官方库不存在该路径） |
| 自带安装器/下载器超时 | 6 | `fastcgi.iisInstall`、`java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora`、`process.php.7.4` / `process.php.8.3` |
| 官方契约守卫（先导入父命名空间，报错逐字同官方） | 9 | `py3.export`、`py3.pip`、`sqlite.backup`、`sqlite.base`、`web.acme.httpClient`、`web.form.query`、`web.layout.debug`、`web.view.dev`、`winex.accObject.element` |
| 官方同款导入顺序（x86 同错，官方冷导入同样失败） | 2 | `win.ui.ctrl.splitter`（`..win.registerClassEx`——其导入头只 import `win.ui.ctrl.metaProperty`，未 import `win`）、`win.ui.ctrl.thread`（裸 `metaProperty()` 依赖 `win.ui.ctrl.metaProperty` 已由父模块载入） |
| `web.layout` 族（第三方 32 位商业库 HTMLayout.dll，x86 上同样失败） | 5 | `web.layout`、`web.layout.valueObject`、`web.layout.element`、`web.layout.element.style`、`web.layout.event`（官方断言） |
| 清单伪条目 | 1 | `win.#DEFINE` |

**结论：x64 真缺口只剩 `web.layout` 1 项（第三方 32 位商业库，不现实），
其余 25 项全部是官方契约 / 环境 / 物理边界。**

### 1.37.3 顺带闭账两条 still-open（roadmap 旧清单）

1. **「console.log 编码原语层 / .join / .getMessage 缺口」——陈旧错账，闭**。
   实测：`import console` 解析到官方源码库（`io.libpath("console")` =
   `D:\aardio\lib\console\_.aardio`），60+ 成员全量可用；官方 console **本就没有**
   `join` / `getMessage` / `isStdout` / `writec` 成员（全库 grep 无定义，只有
   `..string.join` 引用）；编码原语按官方文档语义逐一核对通过——
   `string.getUtf` 返回的是 **UTF 标记**（双引号串 = 8 即 UTF-8，
   `docs/language-reference/datatype/string.md` §41-51），`isUtf16` /
   `toUtf16` 判别与转换正确（UTF-16 串 6 字节、`isUtf16` 为 true）。
2. **「io.file 文本模式语义（CRLF 翻译由调用方负责、fd 恒为二进制）」——本轮落地，闭**。
   按官方文档实现（`docs/language-reference/datatype/newline.md` +
   `docs/library-guide/builtin/io/file.md`）：io.file / io.open **默认文本模式**
   （'t'，'b' 退出）——读 CRLF→`'\n'`、写裸 `'\n'`→CRLF、文本模式读把
   `'\0'`/`'\x1A'` 当终止符（不消耗、后续读停在原地）；CRT fd 恒为二进制
   （活管道在 CRT 文本模式会死锁），翻译在引擎字节层逐调用完成。
   **边界（实测定出）**：文本模式只作用于**磁盘文件与载荷资源**；从裸 OS 句柄
   打开的流（`process.popen` 管道）保持**字节精确**——IDE 的 LSP/DAP 走
   popen 传 JSON-RPC，Content-Length 按原始字节计数，首版实现曾把管道也
   翻译导致帧错位（`ide_nav_smoke` 符号树变空实测复现），已改为句柄路径
   恒二进制。回归：`tests/iofile_text_check.ps1`
   （13 项十六进制字节级断言，x64 / x86 双绿；VM 90/90、stdlib、publish、
   IDE 冒烟 / 导航、builtin_api 全绿，新增失败 0）。

## 1.38 第三十二轮：官方 ide.command 外部命令面落地（附带修掉两个引擎级缺陷）

### 1.38.1 背景

官方 aardio.exe 把可被外部进程/插件驱动的能力注册成一张**命令名表**
（37 条，逐条抄自二进制字符串块，`docs/official-ide-reference.md` §1），
外部工具/插件经 `ide.command.join()` + `ide.command.send(hwnd,"命令名",...)`
驱动 IDE。账本里 `import ide` 长期挂着「需要宿主 ide.command.join() 主管道
已建立」——本轮实测 join **直接 0xC0000005 崩溃**，顺藤摸出两个引擎缺陷。

### 1.38.2 引擎缺陷一：`::Kernel32` 裸调 API 的返回值被截成 32 位（x64）

- **定位**（cdb：崩溃指令 `movsxd rdi, [rdi]`，RDI = 视图基址低 32 位符号扩展）：
  `::Kernel32.MapViewOfFile(...)` 走 `CreateDirectApiWrapper`
  （`engine/ffi.cpp`，DLL 表成员的裸调用路径），默认返回类型 `ARInt`，
  解码 else 分支 `(double)(int)r` 把 64 位映射基址**截成 32 位** →
  脚本拿到假指针 → `fsys.mmap.readTable()` 解引用即崩。
- **同族先例**：第五轮修过官方 `win/api.aardio` 把 `::CallWindowProc` 声明成
  int 返回的 LRESULT 截断——那是**静态声明**路径，这是**裸调默认**路径。
- **修法**：HWND/HANDLE 按 Win32 互操作规则本就 32 位有效（不动）；
  返回**真实 64 位指针**的内存映射/模块加载族升为全宽（`ARI64`）：
  `MapViewOfFile` / `MapViewOfFileEx` / `CreateFileMapping` / `OpenFileMapping` /
  `LoadLibrary` / `LoadLibraryEx` / `GetProcAddress` / `GetModuleHandle` /
  `VirtualAlloc`（官方库裸调实证：fsys.mmap、process.*、dotNet host）。

### 1.38.3 引擎缺陷二：`table.tostring(tab, true)` 非标识符键未括号化

- 崩溃修掉后总线仍断：mmap 注册表往返**静默丢数据**。
  `writeTable` = `table.tostring(tab, true)`（可重新 eval 的字面量）+
  `readTable` = `eval`。Nodio 的 tostring 把 `[-1]` / `[hwnd数字]` 这类
  非标识符键写成**裸** `{-1=...}` → 自己的解析器把它读成数组元素
  （实测 `eval("{-1='v'}")` 得 `{1:-1}`）→ 官方 mmap 协议的保留返回槽
  `[-1]` 与 hwnd 数字键**双向静默丢失**。
- **修法**（`engine/builtins.cpp`）：非标识符键一律括号化——数字键
  `[-1]=...`，字符串键 `['a b']=...`（转义引号）。
- **边界**：表构造器 `{-1=x}` 官方本就不是合法键写法（官方也是运行时
  下标赋值 `t[-1]=x`），解析器行为未动。

### 1.38.4 命令面本体

- **`app/ideCommand.nodio`**：官方 37 命令**全部注册**进共享注册表
  （外部可探测命令存在性；官方语义：未注册的命令 send 返回 null）。
  有依托的 24 条经 main.nodio 注入的 impl 表接真实现（save/saveAll/
  openDocument/newDocument/getActiveCode/setActiveCode/insertCode/gotoLine/
  showStatus/getMainHwnd/getActiveViewHwnd/redrawCodeEditor/projectIsOpened/
  isInProject/getProjectPath/getProjectDir/getProjectMainFile/getConfig/
  setConfig/getProjectProperty/setProjectProperty/switchActiveForm/
  getActiveForm/getSelectedControl/setSelectedControl/getControlParam/
  setControlParam/refreshStdLib/refreshUserLib/createProcessEx/
  loadPrivateFont/killOnExit）；pluginViewCreate/pluginViewShow 等暂为
  注册桩（右侧栏子窗体宿主是独立特性）。
- `app/designer.nodio` 增画布最小访问面（canvasSelected/canvasSelect/
  canvasGetParam/canvasSetParam/isActive），命令实现不摸设计器内部。
- 进程内调用走 `app.ideCommand.dispatch(name, ...)`；跨进程与官方
  插件/工具完全同法（`ide.command.join()` + `send`）。

### 1.38.5 回归

新增 `tests/ide_command_check.ps1`（x64 / x86 双绿）——**真实双进程**：
服务端经 `app.ideCommand.init` 注册 37 命令并写出主窗体句柄，客户端进程
`ide.command.join()` 后逐条 send 官方命令名，断言返回值（setConfig/getConfig
往返、gotoLine 语义、getMainHwnd 一致性、未注册命令返回 null、killOnExit
关停服务端）。附带回归：VM 90/90（x64）+ 89/0/1（x86，t23 剪贴板环境门控）、
IDE 冒烟/右键/导航、编辑器/输入、iofile_text、win_clip、builtin_api、
stdlib、publish 全绿，新增失败 0。

## 1.39 第三十三轮（复验）：`ctrl.setPos` 解禁 —— 第五轮的引擎断言崩溃已不存在

### 1.39.1 背景

§1.10.3（第五轮）记档：官方 `ctrl.setPos(x,y,宽,高)` 在 Nodio 里触发引擎断言
崩溃（`STATUS_BREAKPOINT`），处置为**禁用**、改用 `nodio.ui.setRect`。
当时的引擎是第五轮时点（`::` 系统 API 表刚落地、官方事件路由刚修通）。
此后 IDE/LSP 线又有 7 个修复 commit（含 §1.29 的 `win.setPos` after 参数
只认指针型的修复、P1-13 `::` sysapi 表）与性能线 15 轮改动。本轮目标：
在最新引擎（HEAD `b5f12e8` 等价物）上复验该崩溃是否仍存在。

### 1.39.2 实测（双架构 6/6 全绿，第 15 轮引擎）

| 探针 | 覆盖场景 | 结果（x86 / x64 一致） |
|---|---|---|
| `tests/probe_setpos.aardio` | `win.form` + button，`b.setPos(20,20,120,50)` | `SETPASS 120`（官方 x,y,宽,高 语义正确）、exit 0 |
| `tests/probe_setpos2.aardio` | `f.getScale()` 直调、`b.saveScale()` 直调（官方 setPos 第二步全链）、`b.adjust()` 直调、`show()` 后 setPos、static 控件、同一控件二次 setPos、null 参数 | `GETSCALE type=number`、`SAVESCALE type=boolean`、`ADJUST_OK`、`SET1 120`/`SET2 100`/`SET3 140`/`SET4 140`（null=不改大小语义正确）、exit 0 |
| `tests/probe_setpos3.aardio` | child form（第五轮崩溃场景）里 add + setPos + saveScale/adjust 链 + child 自身 setPos | `CHILD_SET1 120`、`CHILD_CHAIN_OK`、`CHILD_SELF_SET 220`、exit 0 |

**结论：崩溃已不可复现。** 官方 `metaProperty.setPos` 全链
（`win.setPos` → `saveScale()`（内部 `getScale`/`getRect`/`getStyle`/
`toClientRect`/`table.assign`/锚点除法）→ `adjust()`）在双架构上完全可用。

最小复现探针全文（`tests/probe_*.aardio` 按仓库惯例被 .gitignore 忽略、
不入库，故内联于此；运行方式 `nodio.exe <探针路径>`，期望打印 SETPASS/
PROBE_DONE 且 exit 0，崩溃则为 STATUS_BREAKPOINT）：

```aardio
import win.ui;
import console;
var f = win.form(text="setpos-probe"; right=400; bottom=300);
var b = f.add({cls="button"; text="ok"; left=10; top=10; right=110; bottom=45});
console.log("ADDED");
b.setPos(20, 20, 120, 50);
console.log("SETPASS " ++ b.getRect().width());
console.log("PROBE_DONE");
```

### 1.39.3 崩溃为何消失（归因，非本轮修改）

本轮**没有改任何引擎/库代码**。崩溃消失是 IDE/LSP 线后续修复的连带收益，
两个最可能的根因修复（按时间序）：

1. **P1-13（第五轮内）**：裸 `::` 系统 API 签名表落地 —— `saveScale`/
   `adjust` 内部依赖的 `::` 调用链从「抛错」变「可用」；
2. **§1.29（第二十九轮）**：引擎 `win.setPos` 的 `after` 参数只认指针型、
   数值 `1`(HWND_BOTTOM) 被吞 —— 官方 `ctrl.setPos` 底层正是 `win.setPos`，
   该修复直接扫清了这条链上的断言点。

无法进一步二分归因（需要重建第五轮时点的引擎做对照，成本高、无行为差异
价值），按「已修复、以现状实测为准」闭账。

### 1.39.4 处置

- §1.10.3、§1.22 的「已禁用」提法已加解禁注记；第五轮正文的历史叙述保留原文。
- `nodio.ui.rect` 的 `setRect` 封装**保留不动**（行为正确：物理像素、方言无关），
  IDE 线是否切回官方 `ctrl.setPos` 由 IDE/LSP 工作线自行决定（涉及 `ide/`、
  `lib/nodio/` 写禁区，本轮不触碰）。
- 三个探针留在 `tests/` 作回归证据。

## 1.40 第三十四轮：插件视图托管（pluginViewCreate/Show）+ 变参语义对齐

### 1.40.1 背景

§1.38 把官方 37 命令全部注册后，`pluginViewCreate`/`pluginViewShow` 仍是注册桩。
官方契约（docs/official-ide-reference.md §2）：插件面板是
`extensions/plugin/pane/*.aardio` 脚本，内部 `win.form(...;mode="child";parent=...)`，
IDE 端 loadcode 该脚本并调用 `fn(container)` —— 容器以变参注入，面板脚本用
`parent=...` 接住。

### 1.40.2 变参语义对齐（一次误判与回退，全部有据）

- **初始误读**：parameter.md「可变参数不放在最后面就只能取第一个值」——该句说的是
  **多返回值实参**（string.unpack 例子），不是 `...` 本身。据此把 4 处绑定的
  `...` 主值改成「第一个变参」→ VM 套件 8 项红（t18/t19/t20/t21/t30/t43/t47/t69）。
- **定案证据**：`tests/vmtests/mytable.aardio` 的
  `push = function(a,...){ if(...!==null) for(i=1;#...) table.append(a,...[i]); }`
  —— 官方库惯用法 `#...` / `...[i]` / `...!==null` 证明**值位置的 `...` 就是打包表**。
- **最终语义**（逐条对应证据）：
  - 值位置 `...` = 打包表（mrets 挂链供展开点使用）；
  - 实参位置 `f(...)` 调用点 = 整体展开（mrets）；
  - `var a,b = ...` 位置展开（既有行为，回归锁定）；`{...}` 数组槽 = 官方
    「不定参数转换为数组」全展开；
  - **具名槽 `k=...` 取第一个变参** —— 官方 pane 契约 `win.form(parent=...)`
    的实现选择；此前具名槽把键丢掉、整表塞进数组，parent 直接丢失。
- **连带真修复**：6 条内建快速路径（io.print/print/tostring/tonumber/error/sleep）
  原先绕过 `...` 展开（手写实参求值）——统一改走提升后的 `evalCallArgs`，
  原生调用点同样正确展开变参；实参求值 lambda 连同 GcRootScope 一并提升。

### 1.40.3 IDE 侧

- `app/ideCommand.nodio` dispatch 修复：命令登记在 `mgr._commands`
  （`_meta` 只重载了 `_set`），原实现读 `mgr[name]` 恒 null；
- `pluginViewCreate`：剥掉 pane 源尾部的 `win.loopMessage()` → loadcode →
  `fn(container)`；`pluginViewShow`：dock 选中对应页签并显示；
- `lib/nodio/ui/dock.nodio` `addPluginTab(caption, icon)`：child form 容器页签，
  `__applyVisibility`/布局联动识别 `p.plugin` 页；
- 样品插件 `ide/nodio/plugins/tool.nodio`（`NODIO_RUN` 目录自动注册内置 pane）。

### 1.40.4 回归

- VM 90/90 双架构（x64 / x86）——含误判期间 8 项红全部回绿；
- `tests/ide_plugin_check.ps1`（新增，无头 IDE 冒烟）：PLUGIN-CREATE / SHOW / TAB
  三断言，x64 / x86 双绿；
- ide_command 双进程锁双架构、iofile_text 13 项字节断言、builtin_api、
  stdlib imports、win_clip、publish（run_publish_tests）、ide_nav_smoke 全绿。

## 1.41 第三十五轮：函数调用路径提速（-44%~-81%）+ GC 注册表惰性化的环安全修复

### 1.41.1 背景

第三十轮残留清单（roadmap）里引擎线剩「函数调用 ~1.8µs/次」。微基准
（`callbench`，每形态 100 万次、取对空循环差值）基线（x64）：

| 形态 | 基线 µs/次 | 本轮后 µs/次 | 降幅 |
|---|---|---|---|
| 全局函数 2 参 | 2.17 | 1.1~1.3 | -40%~-49% |
| 局部函数 2 参 | 2.70 | ~1.3 | -51% |
| 方法调用 | 4.27 | 1.2~1.3 | -69%~-72% |
| 变参 `f(...)` | 7.81 | 1.7~1.8 | -77%~-79% |
| 定参+`...` 混合 | 7.97 | 1.7~2.0 | -75%~-78% |
| 内建 tostring | 1.06 | 0.17~0.20 | -81%~-84% |

（区间为多轮实测；并行会话的 IDE 进程同核争抢可让墙钟数字虚胖 ~3×，
以安静时段为准。）

### 1.41.2 引擎工作（全部双架构回归锁定）

1. **Env GC 注册表惰性化**：注册表本为「清扫器找环成员」存在，而 Env 能成环
   当且仅当被闭包捕获（FuncLit/类体求值把弱 `self` 提升为共享 `envUp`）——
   注册从构造移到捕获点（`GcRegisterEnv`），普通调用帧的构造/析构不再付
   互斥锁 + 哈希集插入/擦除。
   **途中踩出并修复一个真崩溃**：被捕获而注册的 Env 析构时若不擦除注册表项，
   悬垂指针在新 Env 复用同一地址后会被清扫器当作垃圾清空——活帧的槽在执行中
   被清 → 正在使用的 shared_ptr 控制块落到 `feeefeee` 填充上（cdb 定案
   `lock xadd [rbx+8]`）→ 0xC0000005 + 内存暴涨。析构按 `bGcRegistered`
   补擦除；普通帧仍然零锁零查找。
2. **调用帧/块帧槽化 + 字节预算缓冲池**：第 13 轮只槽化了常驻 Env，调用帧
   仍是 `unordered_map`（每调用桶数组 + 每参数节点分配/释放）。5 个帧创建点
   （EvalCall/CallThisOwner/InstantiateClass/CallVmFunc/ExecBlockScoped）切到
   池化槽向量（线程局部 freelist，acquire 清 bound 标记，僵尸值不可见）。
   池按**字节预算（24MB）在推入时拒绝**——不做淘汰：淘汰销毁僵尸值的级联
   （~Env 重入池）正是首次实现堆损坏（`RtlpLogHeapFailure`）的根源。
   uiprobe13 工作集增量：无界池 206MB → 推入预算 ~54MB（低于无池对照 86MB）。
3. **变参 mrets 别名化**：aliasing shared_ptr 直接指向打包表自身数组——
   省一次 vector 分配 + 整份拷贝，且 `#...`/`...[i]`（表视图）与调用点展开
   （mrets 视图）恒一致；别名指向 vector 对象本身，表增长只重分配缓冲。
4. **快路径实参求值（evalFastArgs）**：6 条内建快速路径（io.print/print/
   tostring/tonumber/error/sleep）的手写实参求值统一为独立 lambda——与
   evalCallArgs 的唯一差别是**多返回值实参保持首值**（官方 parameter.md
   「可变参数不放在最后面就只能取第一个值」），`...` 仍整体展开；
   evalCallArgs 补 `reserve`。

### 1.41.3 回归

VM 90/90 双架构；gcstress ×3（exit=0，uiprobe13）；gcprobe；ide_plugin
双架构；ide_command 双进程锁双架构；iofile_text 13 项字节断言；
builtin_api、stdlib imports、win_clip、publish、ide_nav_smoke 全绿。

### 1.41.4 会话冲突记录（工程过程，供后续协调）

本轮引擎改动两次被同仓库的并行会话以其 `.round34-wip.patch` 快照覆盖回退
（该快照含池化但**缺 §1.41.2-1 的析构擦除修复**，直接应用会重新引入
0xC0000005）。最终以重放 + 立即提交（98e5f94）收口。**并行工作期间
engine/ 目录的单向约定（谁在改谁持有）需要建立**，否则 `git checkout` 式
回退会再次静默丢工作。

---

## 1.42 第三十六轮：IDE「设置」面落地 + 一个把回归带进沟里的宿主陷阱

### 1.42.1 交付（本轮 IDE 线）

1. **设置面**：主题（dark/light，重启后生效——各 chrome 组件是构造期读一次
   `theme()`，官方 aardio 同样重开 IDE 才生效）、编辑器字号（6–48pt，**即时
   生效**：重设 richedit 字体 → 重量行高 → 行号栏字体 → 浮层字号 → 重排）、
   外部工具（最多 9 个，名称/命令/参数，`%f`=当前文档、`%p`=工程目录，绑定
   Ctrl+1..9，与官方约定一致）。配置落 `%APPDATA%\Nodio\ide.cfg`，**与官方
   命令面 `getConfig/setConfig` 共用同一文件**（插件可读到 IDE 设置）；
   `NODIO_IDE_CFG` 可覆盖配置路径（回归锁隔离用）。
2. **回归锁**：`tests/ide_settings/sett.nodio` + `tests/ide_settings_check.ps1`
   —— 36 项断言全绿；`tests/ide_smoke.ps1` 增 SET-FONT / SET-FONT-CFG /
   SET-THEME / SET-TOOLS 断言。
3. **全量跑批脚本正式化**：`tests/run_regression.ps1`（15 项）。旧脚本
   （`%TEMP%\nodio_regress.ps1`）对 **vm / builtin / import 三项没传
   `-Exe`**，这三项会落到各自的默认路径 —— 「想测 A 二进制、实际测了 B
   二进制」，A/B 对比时结论直接作废。新脚本 15 项**全部显式传 `-Exe`**。

### 1.42.2 宿主陷阱：火绒按**程序路径**拦截写入 → 整片假红

现象：新建构建目录（`build-verify64`）跑回归，`vm`（t30/t59）、`iofile`、
`ide_smoke`（manifest 解析）、`ide_settings`（配置落盘）、`ide_ctx` 相继变红，
症状统一为「写操作返回 null / false」，读操作却正常。看起来像引擎回归，
实际**引擎无责**：宿主跑着**火绒安全**（`HipsDaemon`/`HipsTray`），它按
**可执行文件路径**拦截写入。

定位链（每一步都有可复现的对照）：

| 实验 | 结果 |
|---|---|
| 同一脚本、同环境，两个二进制分别跑 | 一个全好、一个全红 |
| 逐字节比对两个二进制 | **仅 4 字节不同**（PE 时间戳），代码完全一致 |
| 把同一个二进制拷到另一个目录再跑 | **行为随目录翻转**（与 exe 路径绑定，与 CWD 无关） |
| 插桩打印 `io.createDir` 收到的路径 | 路径**逐字节相同**（52 字节，同 hex），`GetFileAttributesW` 结果也相同 |
| 换写入位置 | 写仓库目录 ✅ / 写 `%TEMP%` ❌ / 写 `%USERPROFILE%` ❌（仅在被拦的目录启动时） |
| `Get-MpComputerStatus` | Defender 未运行 → 不是 Defender CFA，而是第三方 HIPS |
| 进程列表 | `HipsDaemon` + `HipsTray` = 火绒 |

Win32 侧证据：`_wfopen_s(path,"w+b")` 与 `CreateDirectoryW(path)` 双双返回
`ERROR_ACCESS_DENIED(5)`，而同路径 `"rb"` 正常 —— 只拦「修改」，不拦「读取」。

**结论与处置**：

- 引擎的文件 I/O 没有缺陷，**不要**为这组假红改引擎（本轮曾一度误判为
  「GC 缓冲池提前析构导致 use-after-free」，靠上面的目录互换实验才排除）。
- 回归一律用仓库的**正规构建目录**（`build-x64` / `build-x86`）启动
  `nodio.exe`；临时构建目录需先加入宿主安全软件的信任区，否则红绿不可信。
- 已把该约束写进 `tests/run_regression.ps1` 的文件头注释。

### 1.42.3 回归（本轮最终态）

| 架构 | VM | ide_smoke | ide_ctx | ide_nav | ide_cmd | ide_plugin | ide_settings | editor | input | iofile | winclip | builtin | stdlib | publish | import | 合计 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| x64 | 90/90 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ 36 项 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | **15/15** |
| x86 | 90/90 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ 36 项 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | **15/15** |

（x64 跑在 `build-x64`，x86 跑在 `build-x86`，均为当前工作树重建。）

### 1.42.4 x86 历史崩溃的归因收口

前几轮记录里 x86 的 `0xC0000005`（exe offset 0x43B4）与 x64 冒烟「跑到
`PROJOBS-START` 后不完成」，本轮**在当前树上均未复现**：

- 崩溃根因是并行会话 GC WIP 的「注册表项未擦除 → Env 地址复用 → 清扫器把
  活帧当垃圾清空 → shared_ptr 控制块落到 `feeefeee`」，已由 **98e5f94** 修掉
  （析构按 `bGcRegistered` 补擦除）。
- 而「写盘类用例整片变红」的那部分，是 §1.42.2 的宿主拦截假象，与引擎无关。

两条线都清掉后，x64 / x86 双架构 15/15 全绿。


## 1.43 第三十七轮：IDE「AI」聊天面板（OpenAI 兼容接口）+ 一个把 IDE 一起带走的引擎 GC 悬垂指针

### 1.43.1 交付（AI 线）

1. **三层库**（全部无头可测，回归锁在 `tests/ide_ai/ai.nodio`）：
   - `lib/nodio/ai/http.nodio` —— WinINet 带头 HTTP 客户端。引擎自带的
     `web.get/web.post`（`engine/builtins.cpp:8795`）底层是 `util.cpp` 的
     HttpFetch，**只发 Content-Type、不支持自定义请求头**，而 OpenAI 兼容
     接口必须带 `Authorization: Bearer <key>`，所以这里自己
     `raw.loadDll("wininet.dll")` + FFI。
   - `lib/nodio/ai/cred.nodio` —— Windows 凭据管理器（advapi32 FFI）。
     **API Key 只存这里，不进明文 `ide.cfg`**（配置文件会被同步/备份带走）。
   - `lib/nodio/ai/chat.nodio` —— OpenAI 兼容的会话 / 请求体 / 响应解析
     （纯逻辑，不碰窗口、不碰注册表）。
2. **面板** `ide/nodio/app/ai.nodio`：右栏「AI」页签从「一块只读 richedit」
   换成真正的聊天面板 —— 顶栏自绘板（状态文字 + 发送/清空/设置三个自绘按钮）、
   对话日志、输入框、配置区（接口 / 模型 / 密钥，密钥 `password=true`）。
   - Enter = 发送，Ctrl+Enter = 换行（子类化输入框 + `::GetKeyState(0x11)`）。
   - 请求走 **worker 线程**（`thread.create` + `thread.set/get` 共享表 +
     `winform.setInterval` 轮询）：WinINet 是阻塞调用，放主线程 IDE 会整个
     卡住（鼠标不响应、窗口不重绘）。
   - 配置分工：`base_url` / `model` → `ide.cfg`（`app.settings`，只认
     http/https）；**API Key → 凭据管理器**。
   - 面板给 dock 的是「可见性代理」`self.ctrl = {show=...}`（与
     `nodio.ui.searchPanel` 同一套路），**`lib/nodio/ui/dock.nodio` 一行未改**。
3. **回归锁**：
   - `tests/ide_ai/ai.nodio` + `tests/ide_ai_check.ps1` —— **83 项断言**，
     x64 / x86 双架构全绿（含 8 项 API Key 凭据断言）。
   - `tests/ide_settings/sett.nodio` 增 10 项 AI 配置断言（`ai.baseUrl` 只认
     http/https、`ai.model` 非空、读写往返），设置锁共 **47 项**。
   - `tests/ide_smoke.ps1` 增 AI 面板断言：页签显隐、控件在场、会话初始为空，
     以及**端到端发两条**（成功打本地 mock + 失败打没人监听的端口，
     见 §1.43.7 的「轮询等待」写法与 §1.43.8 的 mock 服务器）。
   - `tests/ai_mock_server.py` —— 本地 mock OpenAI 接口（只依赖 Python 标准库），
     把 AI 面板的**成功回包**路径也变成无头断言（不依赖外网、不碰用户真实 Key）。
   - 凭据隔离：`nodio.ai.cred.apiKeyTarget()` 读环境变量
     `NODIO_AI_CRED_TARGET`，回归锁与冒烟都用 `NodioIDE/test/...` 这样的
     **隔离名** —— 否则 `apiKeyRemove()` 会把用户自己配的那条
     `NodioIDE/ai/apiKey` 抹掉。IDE 冒烟还额外要求「凭据名里含 `/test/`」
     才发那条请求（`main.nodio` 的 `aiIsolated` 判断）。

### 1.43.2 坑一：CREDENTIALW 的 `LastWritten` 是 FILETIME（8 字节），不是 SYSTEMTIME（16 字节）

`CredWriteW` 一直返回 `ERROR_INVALID_PARAMETER(87)`，根因是**结构体布局
整体后移了 8 字节**：把 `LastWritten` 声明成 4 个 INT（16 字节的 SYSTEMTIME），
后面所有字段的偏移全部 +8 —— Windows 读到的是

```
BlobSize=0 / Blob=NULL / Persist=12（非法值）/ Attributes=乱指针
```

直接被参数校验打回。改成 2 个 INT（8 字节）后实测 `sizeof(CREDENTIAL)=80`
（与 Windows x64 一致），`CredWriteW` 返回 TRUE。
声明见 `lib/nodio/ai/cred.nodio:69-82`（写路径）/ `:86-99`（读路径）。

**⚠ 写用 ustring 字段、读用 pointer 字段**：`CredentialBlob` 是 `BYTE*`，
**不是 C 串**（没有结尾 NUL）。写的时候把普通文本赋给 ustring 字段 ——
序列化器（`engine/ffi.cpp` StructSerialize）会写该字符串自身的 UTF-16 缓冲区
地址，正好就是 blob 要的「一段 UTF-16 字节」；读回来要用 pointer 字段拿指针
+ `CredentialBlobSize` 拿长度，再 `raw.readW(ptr, ..math.floor(blobSize/2))`。

**⚠ 读 32 位整数不能用 `raw.uint`**：官方 `lib/builtin/raw.aardio` 把它覆盖
成了「数值包装器」（`raw.uint = lambda(v,ref) number("INT",...)`），它**根本不
读内存**。拿它读 DWORD 只会拿到指针值本身。

### 1.43.3 坑二：`HttpSendRequestW` 是 **5 参数**原型 —— x86 stdcall 多压 8 字节直接 0xC0000005

本机 SDK 头文件 `Windows Kits\10\Include\10.0.22621.0\um\wininet.h:2605-2611`
写得很清楚：

```c
BOOL HttpSendRequestW(HINTERNET hRequest, LPCWSTR lpszHeaders, DWORD dwHeadersLength,
                      LPVOID lpOptional, DWORD dwOptionalLength);
```

引擎自己的 `util.cpp:140-142` 也只传 5 个。原先按 7 参声明
（多传 `dwFlags`/`dwContext`）时：x64 看不出问题（多出来的只是寄存器里的
垃圾值），**x86 是 stdcall** —— 被调方按自己的原型弹栈，多压的 8 字节把
调用方的栈冲歪 → 返回地址变成 0 → `0xC0000005`（崩溃报告 `EIP=00000000`）。
改为 5 参后 x64 / x86 均正常。见 `lib/nodio/ai/http.nodio:52-60`。

**一般性教训**：FFI 声明必须与 SDK 头的**真实原型**一致；x86 的 stdcall
栈失衡不会在 x64 上暴露，双架构回归是唯一能抓住它的手段。

### 1.43.4 坑三：`HttpQueryInfoW` 返回的是 **UTF-16**

W 版 API 的通例：状态码缓冲区内容是 `"32 00 30 00 30 00"`（"200" 的
UTF-16LE）。原先按 UTF-8 解、再用 `string.split(s,'\u0000')`（按字符拆）
只取到首字符 —— 于是状态码恒为 "2"。响应头同理。
现在统一 `raw.readW(pStatus, ..math.floor(n2/2))`
（`lib/nodio/ai/http.nodio:209`）。**只有响应正文是原始字节流**（按 UTF-8 解）。

### 1.43.5 坑四：官方 richedit **没有** `append` 方法 —— AI 日志曾经永远空白

`ide/nodio/app/ai.nodio` 的对话日志原来写的是 `self.log.append(text)`，
而官方 `lib/win/ui/ctrl/richedit.aardio` 只有 `appendText` / `appendLink`，
**没有 `append`**。这个空调用被 `try/catch` 吞掉 → **面板看起来一切正常，
聊天记录永远是空白**。

实测（IDE 冒烟里探针）：

```
type(ctrl.text)="string"  type(ctrl.append)=null
type(ctrl.appendText)="function"  type(ctrl.clear)="function"
type(ctrl.setCharformat)="function"  type(ctrl.getText)="function"
```

修法跟 `app/output.nodio` 一样：**自己留一份 buffer 当真相**，控件只负责显示
（`ide/nodio/app/ai.nodio:305-314`）。顺带暴露一条通用结论 ——
**AI 面板这条线上的控件是官方 aardio 控件**（实测 `type(ctrl.text)=="string"`，
`text` 是字符串属性），`nodio.ui.rect` 里那套「官方控件 text 是属性 /
引擎原生控件 text 是函数」的双方言在这条线上没出现。

### 1.43.6 坑五：`thread.set` 的共享表**不是 GC 的根** —— 跨线程回包要传字符串

`thread.set` 把值存进进程级的 `g_threadSharedMap`（`engine/builtins.cpp:9117`），
而收集器的标记阶段只看得见**当前解释器**的根 —— 共享表里的表对象不是根，
GC 跑起来会把它的**内容清空**（表对象本身还在，于是读回来是个空表）。

所以 AI 面板的 worker 把结果 **JSON 序列化成字符串**再回传
（`ide/nodio/app/ai.nodio:417`）：字符串不是堆对象，GC 碰不到它。

### 1.43.7 引擎缺陷：worker 线程退出后，GC 会摸到**已释放的线程栈**（0xC0000005）

这是本轮最硬的一个坑，**与 AI 面板无关**，任何 `thread.create` 都会踩到。

**现象**：IDE 冒烟在 AI 发送之后整进程 0xC0000005：

```
[FATAL CRASH] code=0xC0000005 at addr=0x...5A3ED file= line=10
  AccessViolation: op=0 target=0x88D1FFEF18
  RIP=0x...5A3ED RSP=0x88CDFFA770
```

注意 **faulting target 与 RSP 不在同一个栈**（`0x88D1…` vs `0x88CD…`）——
主线程解引用了**另一个线程**（已退出的 worker）的栈。

**最小复现（10 行，确定性）** —— `tests/ide_thread_gc.nodio`：

```aardio
import thread;
thread.create(function(a){ return a; }, "x");
for(i=1; 300000; 1){ var junk = {a=i; b="str" ++ i; c={d=i; e={f=i}}}; }
```

对照实验（同一二进制、同一脚本）：

| 条件 | 结果 |
|---|---|
| 默认（自动 GC 开） | **exit = 0xC0000005**（每次必崩） |
| `NODIO_GC_THRESHOLD=0`（关自动 GC） | exit = 0 ✅ |
| worker 线程**还活着**时主线程分配 | exit = 0 ✅（不崩） |
| 主线程只做数值运算、不分配表 | 不崩 |

⇒ 触发条件是「**worker 已退出** + 「主线程随后分配」→ 主线程的收集器跑起来
→ 摸到了 worker 的东西。

**根因**：`engine/builtins.cpp` 的 `ThreadWorkerProc` 里，worker 的解释器是
**线程栈上的局部对象**：

```cpp
Interp wt{ &threadErr };      // ← 住在 worker 的栈上
```

而 `Interp::globals` 是解释器的**成员**（一个 `Env`）—— 也就是说
**这个解释器的全局环境就住在那个即将被释放的栈上**。线程一退出，栈被回收；
但主线程的 GC 仍可能顺着 `Value::envUp` / `Env::parent` 链摸到这个解释器
拥有的 env → 解引用已释放的栈内存 → 访问违规。
（收集器其实**已经知道**这类问题：`engine/gc.cpp:540-545` 有
「另一个解释器在别的线程上跑脚本时，本线程推迟清扫」的护栏；
但那只挡住了**并发**，挡不住「worker 已经死了、引用还留着」。）

**修法**（本轮已改）：解释器改成**堆分配**，并挂在 `ThreadState` 上随线程状态
一起保活 —— 线程栈可以释放，解释器不能：

- `engine/common.h:675` —— `ThreadState` 增 `std::shared_ptr<Interp> interp;`
- `engine/builtins.cpp:968-970` —— `auto wtOwner = std::make_shared<Interp>(&threadErr);`
  + `job->st->interp = wtOwner;`

改完同一个复现脚本 exit = 0（x64 / x86 都验过），IDE 冒烟也恢复全绿。
代价是**每个 worker 线程留下一个解释器**（线程状态本来就不回收，
`g_threadStates` 的条目到进程结束都在），量级可忽略。

**回归锁**：`tests/ide_thread_gc.nodio` + `tests/ide_thread_gc_check.ps1`
（断言**进程退出码**为 0 —— 崩了就是非 0），已加入
`tests/run_regression.ps1`（15 项 → **17 项**）。

**冒烟里怎么断言「请求真的跑完了」**：不能用固定时间点。worker 线程第一次跑
要**自己解析** `nodio.ai.chat` / `nodio.ai.http` 两个模块（每个解释器各有一份
模块缓存，没有跨线程复用），x86 上这一步比 900ms 检查点还慢 —— 固定点断言会在
请求还没回来时就打印「没有错误」，看着像通过、其实链路根本没跑完。
现在的写法是**轮询到 `busy` 变 false 再断言**（100ms 一跳，上限 20s），
并且**关窗定时器要等它**（`aiCheckDone` 门控），否则窗口会先关掉。

### 1.43.8 把「成功回包」也变成无头断言：本地 mock OpenAI 服务器

上一版冒烟只断言了 AI 面板的**失败**路径（连 `127.0.0.1:1` → 必须出现一条
`[错误]`）。**成功**路径 —— AI 的回答进日志、进会话 —— 从来没被断言过，
于是「面板收包那半边」其实是**裸奔**的：解析写错了、角色写错了、
内容取错了，测试都发现不了。

补法：本地起一个 mock OpenAI 服务器（不依赖外网、不依赖用户真实 Key），
让 IDE 面板对着它跑一次成功回包。

**`tests/ai_mock_server.py`** —— 只依赖 Python 标准库（`http.server`）：

| 行为 | 返回 |
|---|---|
| 绑定 | `127.0.0.1:0`（系统挑空闲端口），stdout 打一行 `READY <port>` 并 flush |
| `POST <任意路径>` | 200 + OpenAI 形状 JSON，`content = "MOCK-OK 你好，这是 mock 服务器的回包"` |
| `POST <含 /fail 的路径>` | 500 + `{"error":{"message":"mock failure"}}` |
| `GET /ping` | 200 `{"pong":true}` |

端口由 `tests/ide_smoke.ps1` 读 `READY` 行取出，用环境变量
`NODIO_IDE_AI_MOCK_PORT` 传给 IDE（`main.nodio` 读它决定打 mock 还是走
「只测失败路径」的退化分支 —— 但脚本已把 Python 变成硬依赖，
所以**正常跑批时那个退化分支不会发生**，它只是手工调试时的兜底）。

> ⚠ **Python 是 IDE 冒烟的硬依赖**。起不来 mock → AI 成功路径根本没法断言 →
> 这条回归就只剩失败路径，等于白跑。所以脚本**明确报错**（带 stderr 线索），
> **不做静默降级** —— 静默降级 = 锁不住东西，却看起来是绿的。
> Python 的找法与 `tests/run_vm_tests.ps1` 一致：先找 PATH，再找
> `C:\Python311\python.exe`；仓库里本来就有 5 个测试脚本依赖 Python
> （`run_vm_tests.ps1` / `aardio_*_check.ps1` / `py_overlay_check.ps1` /
> `webview_overlay_check.ps1` / `fasm_overlay_check.ps1`），不是新引入的负担。

**`ide/nodio/main.nodio`** —— AI 段改成**两段状态机**（`aiPhase`）：

- 阶段 0（成功）：`base_url = http://127.0.0.1:<mockPort>/v1`，发「冒烟-成功」
  → 断言日志里有 `我: 冒烟-成功` **和** `AI: MOCK-OK`，会话消息数 = 2
  （user + assistant），`busy` 回到 false。
- 阶段 1（失败）：`base_url = http://127.0.0.1:1/v1`，发「冒烟-失败」
  → 断言日志里有 `我: 冒烟-失败` **和** `[错误]`，会话消息数 = 3
  （失败的那次**用户消息仍留在会话里** —— 面板的约定是「先落用户消息再发请求」）。

新增断言键：`AI-SEND-MOCK` / `AI-SEND-USER` / `AI-SEND-REPLY` /
`AI-SEND-MSGS` / `AI-SEND-BUSY` / `AI-SEND-ERRUSER` / `AI-SEND-ERR` /
`AI-SEND-ERRMSGS`。

**坑六：mock 的 stdin 在 `Start-Process -NoNewWindow` 下**立刻 EOF****

原来的 mock 靠「读 stdin 等父进程收工」来退出。实测这条在自动化里**不成立**：
`Start-Process -NoNewWindow` 不重定向 stdin，子进程继承的是控制台/已关闭的
管道 → `sys.stdin.read()` **立刻返回** → 随即 `srv.shutdown()`。
更要命的是 `shutdown()` 的时序：

```
BaseServer.shutdown()  →  __shutdown_request = True; 等 __is_shut_down
serve_forever()        →  while not __shutdown_request: ...   # ← 直接不进
                         finally: __is_shut_down.set()
```

`shutdown_request` 在 `serve_forever` **真正开始之前**就被置位 → 循环一次都
不进 → 服务器秒退。症状极具迷惑性：**`READY` 打印了、端口拿到了，一连就
`Connection refused`**（而不是「连不上服务」）。

改法：**父进程 PID 看门狗**。

```python
handle = ctypes.windll.kernel32.OpenProcess(0x00100000 /*SYNCHRONIZE*/, False, pid)
kernel32.WaitForSingleObject(handle, 0xFFFFFFFF /*INFINITE*/)   # 父进程一死就醒
srv.shutdown()
```

内核在父进程终止时唤醒这个等待，不用轮询。

> ⚠ **千万别用 `os.kill(pid, 0)` 探活**：Windows 上 `os.kill` 会直接
> `TerminateProcess` —— 那不是探活，是**把父进程杀了**。

**坑七：`BaseServer.shutdown()` 只在 `serve_forever()` 里能被叫停**

`__is_shut_down` 这个事件**只有 `serve_forever()` 的 finally 会置位**。
原先 mock 用的是手写 `for _ in range(MAX): srv.handle_request()` 循环，
此时调 `shutdown()` → `__is_shut_down.wait()` **永久阻塞** → 看门狗线程挂死、
主线程 `join(timeout=120)` 干等。改用 `serve_forever(poll_interval=0.2)`，
`shutdown()` 才真正可停。

**坑八：孤儿 mock 进程会让整条调用「看起来卡住」**

`Start-Process -NoNewWindow` 的子进程与父进程**共用控制台**，父进程退出后
mock 还活着 → 它一直占着控制台句柄 → **调用方的 stdout 管道等不到 EOF**
→ 整条命令不返回（实测：冒烟脚本早就打印了 `SMOKE OK`，调用方仍在等）。
三道保险：

1. mock 侧：父进程 PID 看门狗（上面的坑六）；
2. 脚本侧：`tests/ide_smoke.ps1` 把主体整段包进 `try { … } finally { … }`，
   `finally` 里 `taskkill /T /F`（连进程树）+ `WaitForExit(3000)` 收掉 mock
   并删临时日志。（PowerShell 的 `exit` 在 `try` 内**也会**执行 `finally`，
   所以十几条 `exit 1` 的断言失败路径同样能收干净。）
3. 兜底扫一遍：按**命令行**扫 `ai_mock_server\.py`，把「上一次被中断
   （Ctrl+C / 崩了）」留下的孤儿也回收掉 —— 这套做法直接沿用仓库既有的
   `tests/run_vm_tests.ps1`（它对 `webroot\_srv.py` 就是这么收的：
   先按 PID 杀树，再按命令行扫一遍）。按**脚本路径**匹配，不按 "python"，
   免得误伤别的 Python 进程。

收尾后实测：`leftover mock procs = 0`，冒烟命令正常返回。

### 1.43.9 回归（本轮最终态）

| 架构 | ide_smoke | ide_ai | ide_settings | ide_threadgc | 其余 13 项 | 合计 |
|---|---|---|---|---|---|---|
| x64 | ✅ | ✅ 83 项 | ✅ 47 项 | ✅ | ✅ | **17/17** |
| x86 | ✅ | ✅ 83 项 | ✅ 47 项 | ✅ | ✅ | **17/17** |

（x64 跑在 `build-x64`，x86 跑在 `build-x86`，均为当前工作树重建；
引擎本轮改过 `common.h` + `builtins.cpp`，两个架构都重建过。）

`ide_smoke` 一项在本轮扩成**两段 AI 断言**（成功 + 失败）：

| 断言键 | 含义 | x64 | x86 |
|---|---|---|---|
| `AI-SESSION-INIT` | 启动时会话为空 | `0` | `0` |
| `AI-SEND-START` | 发送动作跑起来了 | `1` | `1` |
| `AI-SEND-MOCK` | 打的是本地 mock（不是降级路径） | `1` | `1` |
| `AI-SEND-USER` | 用户消息进了日志 | `1` | `1` |
| `AI-SEND-REPLY` | **AI 的回答进了日志** | `1` | `1` |
| `AI-SEND-MSGS` | 会话里多了 assistant 消息（共 2 条） | `2` | `2` |
| `AI-SEND-BUSY` | 请求结束后回到空闲 | `false` | `false` |
| `AI-SEND-ERRUSER` | 失败那次用户消息仍留在日志 | `1` | `1` |
| `AI-SEND-ERR` | 失败时出现 `[错误]` | `1` | `1` |
| `AI-SEND-ERRMSGS` | 失败后会话共 3 条 | `3` | `3` |

冒烟跑完后实测 **遗留 mock 进程 = 0**（x64 / x86 均是），冒烟命令正常返回。

---

## 1.44 第三十九轮：import / 加载路径分配治理（普查驱动）

### 1.44.1 工具：全局分配普查（engine/allocstat.cpp，常驻诊断）

NODIO_ALLOC_TRACE=1（或 NODIO_BOOT_TRACE）打开。全局 `operator new/delete`
override 统计每次堆分配，按 import 三个阶段归账（1=read/lex/parse、
2=exec、3=模块收尾拷贝），出口打印：总量、阶段表、2 的幂尺寸直方图、
**阶段×尺寸交叉表**、>64KB 大块分配的**栈位点表**（CaptureStackBackTrace
+ dbghelp 符号化，符号需 prof 构建）。本节所有定位都来自它，不再靠
调试堆放大倍数猜。

### 1.44.2 普查结论（import win.ui 家族，优化前）

358k 次分配 / 184.8MB：parse 264k 次 90MB（74% 按次数）、exec 90.6k 次
94MB。尺寸直方图：256B 桶 13.0 万次（= make_shared\<Node\>，AST 节点
~13 万）、≤32B 桶 14.6 万次（节点小 vector）、>32K 桶仅 775 次却
133MB（token 向量倍增搬移 churn + exec 大槽向量）。大块栈位点表把
exec 的鲸鱼钉死在 **`Env::VarAt` 的 `slots.resize`**（175 次 × ~400KB）。

### 1.44.3 五项优化（全部进本轮提交）

1. **PNode 节点池**：`NewPNode()` 统一 9 处 `make_shared<Node>` 为
   `allocate_shared` + 尺寸恒定 thread_local 自由链表（gc.cpp）。块级
   回收——块只在引用归零后入链，复用时旧 AST 必已死，无 arena 的生命
   周期问题；链表上限 10 万块（≈23MB）与槽池预算同量级。256B 桶
   13.0 万 → 6.5k。
2. **Lex 预留 + Parser move**：token 向量按 src/3 预留（倍增搬移消失），
   `Parser ps{ std::move(toks), ... }` 免 64B/token 整份拷贝。
3. **VarAt 几何增长**：intern id 进程级递增，逐次 +1 resize 每次都要
   realloc 拷贝整段旧槽；几何步长让容量内的后续写退化为 O(1)。
4. **槽空间封顶 + 整帧退化（kSlotMax=512）**：尺度病根——槽下标 =
   全局 intern id，而 `sizeof(Value)=328B`（7 个 shared_ptr +
   std::function）。晚定义函数的调用帧为几个局部变量要把槽向量撑到
   max-id × 328B（win.ui 家族 id ~5 万 → 4.4MB/帧），24MB 池预算只够
   缓存个位数大缓冲，之后每帧 malloc+resize+清零。非驻留帧（池化槽帧）
   越界时把已有槽位搬进 vars map、整帧退化为 map（一次性
   O(本帧变量数)，`conv=154` 帧/次 import），槽缓冲归还池；驻留帧
   （globals/mainEnv，`bSlotsKeep`）保持几何槽不受限。
5. **BootTrace 计时器全部环境门控（g_bootTrace）**：途中发现的独立
   问题——语句/Ident/Member/FuncLit 等计时器是无条件 QPC，Ident 是
   最高频求值，每条语句白付 ~60ns（3 次 QPC + 频率除法）。先读一次
   环境变量存 `g_bootTrace`（动态初始化），全部累积器按它门控。
   **注意：第三十五轮台账的 µs 表在今天的 HEAD 上无法复现**（当时
   计时器已在但表在更早测得？无法考证），本轮一律以同日同机 A/B 为准。

### 1.44.4 数字（同日同机 A/B，HEAD=8175aba 工作树对照）

| 项 | HEAD | 本轮 | 变化 |
|---|---|---|---|
| import win.ui 家族（4 轮 min） | 281ms | 250ms | -11% |
| exec 阶段分配 | 90.6k 次 / 105.7MB | 91.1k 次 / 27.3MB | **字节 -74%** |
| parse 阶段分配 | 264k 次 / 90MB | 126k 次 / 32.9MB | -52% / -63% |
| 总分配 | 358k 次 / 184.8MB | 220k 次 / 60.8MB | -39% / -67% |
| callbench 变参 `f(...)` | 22-29µs | 8.6-12.3µs | **-2.5~3×** |
| callbench 全局 2 参 / 方法 | — | 打平 | ±0 |
| callbench 内建 tostring | 329-453ms | 234-376ms | -25% |
| 冷启动（无 import 脚本） | ~65-83ms | 打平 | 进程创建主导 |

（callbench 每形态 100 万次取空循环差值，交错 4 轮；背景负载可造成
单轮毛刺，取多数轮的众数区间。）

### 1.44.5 结构挂账：Value = 328B

Value 里 7 个 shared_ptr（112B，每次拷贝 7 次原子操作）+ std::function
（64B）+ 两个 wstring + params vector。拷贝一个 Value = 328B memcpy +
7 次 incref/decref——这是 exec 侧剩余成本与调用路径的公共下限，也是
callbench 打平部分的原因。**FuncData 侧表重构**（函数专属字段收进一个
shared_ptr，Value 收缩到 ~100B、拷贝 1 次原子操作）挂账下一轮，预计
同时改善调用路径与 GC 压力。

### 1.44.6 回归（本轮最终态）

x64 17/17、x86 17/17 全绿（run_regression.ps1，槽封顶后与计时器门控后
各跑一轮）；gcstress uiprobe13 ×4 通过（DONE-2000）；uiprobe13 WS 增量
20-54MB（PNode 池高水位驻留，上限 23MB + 槽池 24MB）。

### 1.44.7 会话协调

文档第三十六~三十八轮与 §1.42~1.43 为并行 IDE 会话所写（本轮提交时
仍在工作树未提交，其完整性经核对后与本节一起落账）；其第三十七轮的
引擎修复（thread.worker 解释器生命周期）已按 hunk 拆分单独提交
（694a399），归属其轮次。


## 1.45 第四十轮：编辑器字体「时大时小」—— 一个键名写错，Consolas 静默变成了 Segoe UI

### 1.45.1 现象

IDE 的**代码编辑框**里，同一行的中文和英文**大小明显不一致**：英文小一号、
中文大一号，中文还像是另一种字体（笔画更细、风格不同）。整片代码看上去
「时大时小」。

截图（`build-x64/Release/shot_font_ide.png`，150% DPI）里第 8 行：
`// ⚠ 主脚本必须在 app.shell **之前**应用：各 chrome 组件都是…`
—— 「主脚本必须在」明显比「app.shell」大，且风格不同。

### 1.45.2 根因：`ctrl.setFont` 的键名写错了（`faceName` ≠ `name`）

`lib/nodio/ui/editor.nodio` 设编辑器字体时写的是：

```aardio
ctrl.setFont({faceName=self.fontName; point=self.fontSize});   // ← 错
```

但官方 `LOGFONT` 类的字体名字段就叫 **`name`**（`WORD name[32]`），
而它的构造函数是**逐键原样拷贝**：

```aardio
// D:\aardio\lib\gdi\_.aardio:7
class ::LOGFONT{
  ctor(arg,point){
    this.name = "Tahoma";
    ...
    for(k,v in arg){ if( k!="_struct" ) this[k] = v; }   // ← 认不得的键被当垃圾塞进去，不报错
    if(this.name =="Tahoma"){ ... elseif(!_WINXP){ this.name = "Segoe UI"; } }   // ← 兜底
  }
  WORD name[32];   // ← 字体名
}
```

于是 `faceName` 被当成一个**无关的结构体字段**扔掉，`name` 保持默认值
→ 触发「Tahoma → **Segoe UI**」兜底 → **Consolas 从来没生效过**。
实际渲染的是 **Segoe UI**（比例字体）→

1. 代码**不等宽**，列对不齐；
2. Segoe UI 没有汉字字形 → 汉字走 GDI 字体回退（另一套字体）→
   字面更大、风格也不同 → 「时大时小」。

**为什么能活这么久**：这一行包在 `try{ … }catch(e3){}` 里，**异常被吞掉**；
而错键名**根本不抛异常**（只是被忽略），所以既没有报错、也没有日志，
只剩下「看起来怪」。

**实测对照**（`tests/ide_font/font.nodio` 的前三条断言）：

```
::LOGFONT({faceName="Consolas"; point=11}).name  ==  "Segoe UI"   ← 错键名
::LOGFONT({name="Consolas";      point=11}).name  ==  "Consolas"   ← 对键名
```

官方 API 文档也写死了键名：

- `D:\aardio\lib\win\ui\ctrl\edit.aardio:802`
  `!edit.setFont(混入字体属性) = @.setFont(h=-12;name="Tahoma");`
- `D:\aardio\lib\win\ui\ctrl\richedit.aardio:799`
  `setFont = function(f,own = true ){ … f = ::LOGFONT(f) … }`

### 1.45.3 修复

`lib/nodio/ui/editor.nodio` 两处（首次设字体 + `setFontSize`）改成：

```aardio
ctrl.setFont({name=self.fontName; point=self.fontSize});   // ← name
```

`point`（pt）仍然是对的：官方 `setFont` 会 `stripPoint` 把它换算成
像素高度，再除以 DPI 缩放（richedit 自己会缩放，避免二次放大），
所以**不能**自己再乘 `dpiScale`。

### 1.45.4 顺带修的三处「口径不一致」

| 位置 | 问题 | 改法 |
|---|---|---|
| `lib/nodio/ui/editor.nodio`（行号栏画板） | 画板字号单位是**逻辑像素**，却直接塞了 **pt**（`fontSize = self.fontSize`）→ 这块板面的字体比别处小 1/4 | 改用 `self.uiFontSize`（pt × 4/3） |
| `lib/nodio/ui/editor.nodio`（`setFontSize`） | 同上，改字号后行号栏画板字号跟着错 | 同上，改用 `self.uiFontSize` |
| `ide/nodio/app/shell.nodio`（分栏对比编辑器） | **硬编码 `fontSize = 11`**，不跟设置走 → 用户改了字号，左右两半对不上 | 改成 `app.settings.fontSizeGet()` |
| `ide/nodio/main.nodio`（`onEditorFontSize`） | 改字号只通知了主编辑器，分栏那个不动 | 同时 `shell.editor2.setFontSize(n)` |

### 1.45.5 回归锁：`tests/ide_font/` + `tests/ide_font_check.ps1`（9 项）

**怎么断言「字体对不对」**：不去读 HFONT —— 我们引擎的 `::SendMessage`
回不了指针型返回值（实测 `WM_GETFONT` 恒为 0），改用**等宽性**这个
可观测特征：等宽字体里 `"iiiiiiiiii"` 与 `"WWWWWWWWWW"` 宽度相同，
比例字体里差一大截。宽度用 `EM_POSFROMCHAR(0x426)` 量（低 16 位 = x）。

实测灵敏度（同一二进制，只改键名）：

| 设置方式 | `iiiiiiiiii` | `WWWWWWWWWW` | 等宽 |
|---|---|---|---|
| `name=`（修好） | 81 px | 81 px | ✅ |
| `faceName=`（原 bug） | 31 px | 131 px | ❌ |

最后一条是**反证**：故意用错键名再设一次，断言字体确实变回比例 ——
保证前几条不是「怎么测都过」的假绿。

冒烟里另加两条（`ide/nodio/main.nodio` + `tests/ide_smoke.ps1`）：

```
DOC-FONT:11/11      ← 主编辑器字号 / 设置里的字号
DOC-FONT-DIFF:11    ← 分栏对比编辑器的字号
```

三者必须相等（`tests/ide_smoke.ps1` 逐条比）。

### 1.45.6 顺带修的回归夹具：`ide_ctx` 的剪贴板**间歇性假红**

字体改完跑全量回归时，`ide_ctx` 在**批量**里时红时绿、**单独跑**恒绿。
查下来与字体无关，是**共享桌面环境的剪贴板争用**：

- 本机剪贴板被别的进程**高频短暂占用**（远端会话 / IM / 剪贴板管理器）：
  `GetOpenClipboardWindow` 连续采样 5 次都**看不到持有者**（持有窗口太短），
  但 `.NET` 的 `Clipboard::SetText` 稳定报「所请求的剪贴板操作失败」
  = `CLIPBRD_E_CANT_OPEN`；仓库自己的 `tests/win_clip/clip.nopio` 开头
  也打印 `CLIP-OK 0`。
- `tests/ide_ctx/ctx.nodio` 的剪贴板段是**复制后直接回读、不校验**：
  写失败时回读拿到的是**上一次遗留的内容**，于是打印出一个「说不清对错」
  的值 → 跟 `ctx.expect` 逐行比对就红：
  - 单独跑：上一轮遗留的正好是 `"hello"` → **碰巧通过**（假绿）
  - 跟着别的测试跑：遗留的是别的东西 → **间歇性假红**

**改法**（沿用仓库既有的思路，见 `tests/win_clip/clip.nopio`：剪贴板是全局
资源，**每步都要重新探测**，按「成功语义 / 官方静默失败语义」分开处理）：

- 复制 / 剪切 / 写入 / 粘贴**每一步都「操作 → 回读校验 → 不对就重试」**
  （各 8 次，带 40ms 让出）；
- 任何一步对不上 → 整组退成 `EDIT-CLIP-SKIP`（`ide_ctx_smoke.ps1` 已有
  这个分支：它会同时从 got / want 里去掉那 4 行剪贴板断言）；
- 右键链路相关的断言**一条都不放松**。

**两条分支都验过**：

| 分支 | 验证方式 | 结果 |
|---|---|---|
| 成功语义（剪贴板可用） | 临时副本把 `win.clip` 换成内存假实现（编辑器的文本效果照常） | 逐行对齐 `ctx.expect` → `CTX-SMOKE OK` |
| 静默失败语义（剪贴板被占） | 实机（当前剪贴板就是被锁的） | 打印 `EDIT-CLIP-SKIP` → `CTX-SMOKE OK` |

> 真正的剪贴板信号仍由 `tests/win_clip_check.ps1` 那条专门锁负责 ——
> 它每步带 `CLIP0/CLIP1` 门、按门选择期望值（roadmap 第 27 轮挂账项）。

### 1.45.7 回归（本轮最终态）

| 架构 | ide_smoke | ide_font | ide_ai | ide_settings | ide_ctx | ide_threadgc | 其余 12 项 | 合计 |
|---|---|---|---|---|---|---|---|---|
| x64 | ✅ | ✅ 17 项 | ✅ 83 项 | ✅ 47 项 | ✅ | ✅ | ✅ | **18/18** |
| x86 | ✅ | ✅ 17 项 | ✅ 83 项 | ✅ 47 项 | ✅ | ✅ | ✅ | **18/18** |

> `ide_font` 的项数：§1.45.3 落锁时 9 项，§1.45.8 补完「字号真的生效」
> 的盲区后为 **17 项**（含 5 条变异/反证）。

本轮**未改引擎**，两个架构沿用上一轮的构建产物；改动全在
`lib/nodio/ui/editor.nodio`（脚本层）、`ide/nodio/app/shell.nodio`、
`ide/nodio/main.nodio`、`tests/ide_ctx/ctx.nodio` 与新增的
`tests/ide_font/`、`tests/ide_font_check.ps1`。

### 1.45.8 给字体锁补盲区：「字号真的变了」必须有判据

**上一轮锁的盲区**

`tests/ide_font/font.nodio` 原来的 9 项断言里，字号相关的只有三条，
而这三条**全都只读返回值**：

```
chkTrue("editor/setFontSize-accepts", ed.setFontSize(14), ...)   ← 返回 true
chk("editor/fontSize-echo", ed.getFontSize(), 14)                ← 回显 14
chkTrue("editor/mono-after-setFontSize", ...)                    ← 仍然等宽
```

如果哪天 `setFont` **静默失败**（`point` 没传进字体 / 键名又写错 /
异常被 `catch` 吞掉），`setFontSize` 照样返回 true、`getFontSize` 照样回显 14、
字体照样等宽 —— **三条断言全过，但用户改字号一点反应都没有**。
这就是假绿，也是这轮要补的洞。

**判据：字号变了，排版必须跟着变**

point 落到字体上，一定会影响**字宽**和**行高** —— 这两个量都能**直接从控件
里量出来**，不依赖任何返回值：

- 字宽：`EM_POSFROMCHAR(0x426)` 的低 16 位 = x
- 行高：同一消息的高 16 位 = y，取「行 2 首字符的 y − 行 1 首字符的 y」

实测（x64 / x86 同值；本机 96 DPI）：

| point | 10 个 W 的宽度 | 行高 | `ed.lineHeight` |
|---|---|---|---|
| 11 | 81 px | 17 px | 17 |
| 14 | 101 px | 22 px | 22 |
| 18 | 131 px | 28 px | 28 |
| 调回 11 | 81 px | 17 px | 17 |

断言用**比值**而不是绝对像素：字宽正比于字体高度、字体高度正比于 DPI，
所以比值与 DPI 无关（本机 96 DPI 实测 14pt/11pt = 101/81 = **1.247**；
而「point 没生效」的比值是 **1.00**）。阈值取 `w14 * 100 > w11 * 115`，
在实测 1.247 与失败点 1.00 之间留足了余量。

「调回 11pt 后宽度**精确**回到 81、行高回到 17」这条也在锁里 ——
它证明测量是字号的确定函数，不是噪声。

**顺带踩到并绕开的坑：aardio 双引号串是原样字面量**

第一版探针里文本写成双引号 `"WWWWWWWWWW\r\nWWWWWWWWWW"`，
而 aardio 的**双引号串不转义** → `\r\n` 是 4 个字符 `\ r \ n` →
文本成了**一行 24 个字符**（`EM_GETLINECOUNT` 返回 1），
行高量出来恒为 0，差点误判成「这引擎量不出行高」。
换行必须用**单引号** `'\r\n'`（`editor.nodio` 的 `__detectEOL` /
`__toCRLF` 用的正是单引号，那两处没问题）。

**变异测试：证明新断言真有判别力**

把 `lib/nodio/ui/editor.nodio` 的 `setFontSize` 改成
`ctrl.setFont({name=self.fontName; point=11})`（模拟「point 没生效」），
**不重新构建**（`.nodio` 脚本运行时从 `NODIO_OVERLAY_LIBDIR` 加载），
重跑字体锁：

| 断言组 | 结果 |
|---|---|
| 旧的 8 项（含 `setFontSize-accepts` / `fontSize-echo` / `mono`） | **全 PASS** ← 旧锁确实是假绿 |
| 新增 4 项（字宽 / 行高各两组「变大」） | **全 FAIL** ← 新锁抓住了 |

```
PASS editor/setFontSize-accepts
PASS editor/fontSize-echo
PASS editor/mono-after-setFontSize
FAIL editor/width-grows-with-size  w11=81 w14=81
FAIL editor/lineheight-grows-with-size  lh11=17 lh14=17
PASS editor/lineHeight-field-matches
FAIL editor/width-grows-again  w14=81 w18=81
FAIL editor/lineheight-grows-again  lh14=17 lh18=17
== FONT FAILED: 4
```

改回后 x64 / x86 恢复 17/17。

**反证也补了一条**

- `counter/wrong-point-key-not-11pt-not-14pt`：故意把 point 键名写错
  （`fontPoint=`）→ point 被丢掉 → `LOGFONT` 退回默认 `h = -12`（≈9pt）
  → 宽度 71 px，**既不是 11pt 的 81、也不是 14pt 的 101**。
  这条证明「宽度随字号变」不是恒真的假绿。
- `editor/wrong-key-becomes-proportional`（原有的字体名键名反证）保留。

**规模**：字体锁 9 项 → **17 项**（`tests/ide_font_check.ps1` 统计 `^PASS `）。




## 1.46 第四十一轮：Value 侧表化（FuncData）+ native 裸指针化——调用路径近乎腰斩

### 1.46.1 动机（上一轮的挂账）

`sizeof(Value)` 曾为 **328B**：7 个 shared_ptr（112B，每次拷贝 7 次原子
引用计数）+ std::function（64B）+ 两个 wstring + params vector，而其中
~200B 只有 Func/Class 值用得上。每个 Value 拷贝 = 328B memcpy + 7 次
原子操作——这是调用路径、exec 分配量与 GC 压力的公共下限。

### 1.46.2 两项结构手术

1. **FuncData 侧表**：9 个函数专属字段（fn/classBody/params/fname/
   nsClosure/envUp/pdefaults/thisVal/ownerVal）收进
   `shared_ptr<FuncData>`，仅在创建 Func/Class 值时分配一次。
   `sizeof(Value)` 328 → **136B**（-58%），拷贝 7 → 3 次原子操作。
   全部 232 处字段访问点改为 `.fnData->`（编译错误驱动逐点修补）。
2. **native 裸指针化**：64B 内联的 `std::function` 换成裸函数指针 +
   `shared_ptr<void> nativeCtx`。`NF`/`Native_` 模板化后 407 个注册点
   **零改动**：无捕获 lambda 经类型级 trampoline + 静态空对象（别名
   指针不拥有）实现零分配；有捕获 lambda 一次 make_shared。30 个调用
   点改传 `nativeCtx.get()`；等价判据从 `target_type()` 改为跳板地址
   （同一 lambda 类型同一跳板，类型级判等语义不变）。

### 1.46.3 途中踩出并修复的三类真坑

1. **创建点漏分配 fnData**：`Native_` 模板化时漏加
   `x.fnData = make_shared<FuncData>()`（脚本断言中断导致半截写入）——
   builtin 注册期读 `fname` 直接空指针崩溃。规则固定为：**t ∈ {Func,
   Class} 当且仅当 fnData 非空**，全部 6 个直接创建点 + NF/Native_ 落实。
2. **GcMarkValue 无条件解引用**：非函数值在第一次 GC 即崩。侧表访问
   必须以 `if (v.fnData)` 门控。
3. **共享侧表的写透**：util.cpp 跨线程 DeepCopy 尾路对 `nsClosure`
   原地 reset——fnData 共享后会把**原函数**的闭包一起清掉，必须先
   `make_shared<FuncData>(*v.fnData)` 深拷贝再清；标量（Null/Num/Str）
   与 native 函数也走这条尾路，fnData 为空时跳过（旧代码等价于对 null
   shared_ptr reset，重构后成了空指针解引用，t28/t29 线程用例当场抓获）。

### 1.46.4 数字（同日同机，与第三十九轮终态对比）

| 项 | 第三十九轮终态 | 本轮 | 变化 |
|---|---|---|---|
| sizeof(Value) | 328B | **136B** | -58% |
| callbench 全局 2 参 | 3.9-4.4µs | **1.8-2.1µs** | **-55%** |
| callbench 方法调用 | 8.5-13.5µs | **3.9-4.0µs** | **-55%** |
| callbench 变参 | 8.6-12.3µs | **2.6-2.8µs** | -60% |
| callbench 内建 tostring | 234-376ms | **140-155ms** | -45% |
| import win.ui 家族 | 250ms | **172-188ms** | -30% |
| exec 阶段分配 | 91k 次 / 27.3MB | **75.9k 次 / 11.6MB** | -58% 字节 |
| uiprobe13 WS 增量 | 20-54MB | **11.7-14.9MB** | GC 压力大降 |

### 1.46.5 回归（本轮最终态）

x64 18/18、x86 18/18 全绿（run_regression.ps1，含线程/IDE 全套）；
gcstress uiprobe13 ×2 通过；字节码/记录 VM/官方样例全绿。

### 1.46.6 会话协调

文档第四十轮/§1.45（编辑器字体）为并行 IDE 会话所写，本轮提交时仍在
工作树未提交，完整性经核对后与本节一起落账；第四十一轮编号顺延其后者。

## 1.47 第四十二轮：发布链路补齐（IDE 发布入口 + 发布器认 .nproj）

### 1.47.1 盘点：为什么是发布链路

用户问「还有什么功能欠缺的补齐」。逐项盘点结论：菜单/钩子面基本齐
（唯一占位是「工程属性」只 `setStatus`）；**真正的缺口是发布链路**——
发布器引擎（`publish/publish.cpp`，完整管线：.aproj 规则/语法门/
import 闭包/payload 追加/PE 补丁）与 CLI 契约早已存在且有回归锁，
但 **IDE 没有任何发布入口**（能写/能跑/能调试，出不了货）；
且发布器只自动探测 `default.aproj`，不认 Nodio 原生 `.nproj` 工程清单
（本仓库 IDE 自己就是 `.nproj`）。「最近文件」列表也是缺口，需设置
持久化 + 菜单重建支持，推迟到后续轮。

### 1.47.2 发布器：.nproj 清单 + 显式 --proj（C++）

- `publish.h`：`Options` 增 `std::wstring strProj`（显式清单路径）。
- `publish.cpp:738-757`：清单解析顺序 = **显式 `strProj`**（给了但
  不存在 → 报「找不到工程清单」防拼错静默退化）→ `default.aproj` →
  `default.nproj`；`ParseAproj` 原样复用（`<file path= embed= ignored= local=`，
  `embed` 缺省 false，与官方 .aproj 语义一致；脚本靠 import 闭包进
  payload，数据文件要显式 `embed="true"`）。
- `main.cpp:415` CLI 增 `--proj <manifest>`、`:385` 用法串更新；
  `:554` env 入口增 `NODIO_PUBPROJ`。
- 新夹具 `tests/m7/proj_nproj/`（main.aardio + default.nproj +
  custom.nproj + res\note.txt）；`tests/run_publish_tests.ps1` 增第 2 步
  （default.nproj 自动探测）与第 3 步（`NODIO_PUBPROJ=custom.nproj`），
  7 项 → **10 项**，收尾标记 `publish regression green (nproj + --proj)`。

### 1.47.3 IDE 侧发布入口（.nodio 脚本层，无需重建引擎）

- `app/runner.nodio`：`endTag` 机制 + 新 `runArgs(argLine, workDir, title)`
  —— 参数行不含 exe 本身，复用同一套 100ms 流式轮询与结束标记；
  `run()` 行为不变（既有冒烟的 `[运行结束]` 门不受影响）。
- `main.nodio`：`publishRun(entry, outExe, manifest, bGui)`（拼
  `'--publish "<entry>" -o "<out>"'` + 可选 `--proj`/`--gui`，切底栏
  「输出」页，`runner.runArgs(args, pi.dir, "发布")`）、`publishExe(bGui)`
  （entry = 工程启动文件或当前文档，草稿先落 `%TEMP%`，`fileDlg.save`
  弹输出路径，工程打开时显式传 `proj.path`）、真「工程属性」对话框
  （工程文件/根目录/启动文件/清单数/发布清单存在性）。
- `app/shell.nodio:1106,1112-1113`：「工程属性」接 `onProjProperties`；
  工程菜单增「发布为独立 exe（控制台）/（界面程序）」两条显式入口
  （不做「import win.ui 就自动 --gui」的猜测）。

### 1.47.4 坑：popen 的 startInfo 塞 environment 表实测报错

IDE 进程带着 `NODIO_RUN`（跑 IDE 自己的脚本），发布的产物子进程会
继承——main.cpp 里 NODIO_RUN harness（464 行）在 payload 模式（586 行）
**之前**，不清掉产物会去跑 IDE 脚本而不是自己的 payload。最初往
`process.popen` 第三参 startInfo 塞 `environment` 表 → **必报**
`尝试调用非函数值 (.indexOf) @ D:\aardio\lib\process\popen.aardio:63`
（popen 的 ctor 把 startInfo `table.assign` 进 STARTUPINFO，environment
表传不进去；`process/_.aardio:76-97` 基类 ctor 虽支持但走不到）。
修法（`main.nodio` 冒烟段）：**spawn 前后临时清父进程 env**——
`string.getenv` 存 `NODIO_RUN/NODIO_PUBLISH/NODIO_PUBOUT/NODIO_LIBDIR`
→ `string.setenv(name, "")`（空串 ≡ 删除变量，main.cpp 的
`GetEnvironmentVariableW > 0` 判定等于未设置）→ spawn → 按原值恢复
（原未设置则置空串）。定时器回调同线程同步执行，无重入风险。

### 1.47.5 冒烟锁：PUB 三断言（端到端）

`NODIO_IDE_SMOKE` 分支增发布段：token = `"pubtok-" ++ time.tick()`
（**每次不同**，旧发布产物混进来骗不过）→ `publishRun` → 400ms 轮询
等 `[发布结束]`（30s 超时打 `PUB-TIMEOUT`）→ `PUB-EXE`（产物落盘）→
spawn 产物回读 stdout 校验 token 打 `PUB-RUN`。runner 是单槽，冒烟
顺序定为**先发布、后 F5 回声**（600ms 定时器等 `pubDone` 门，关窗门
同样加 `pubDone`）。`tests/ide_smoke.ps1:346-352` 断言
`PUB-START:1 / PUB-EXE:1 / PUB-RUN:1`。

### 1.47.6 回归（本轮最终态）

发布回归 10 项 × 双架构全绿；冒烟 x64/x86 全绿（`PUB-START:1/
PUB-EXE:1/PUB-RUN:1`，`IDE-SMOKE-OK`，无 `PUB-EXC`/`PUB-TIMEOUT`）；
全量回归 **x64 18/18、x86 18/18**。途中一次 AI mock 段偶发假红
（`AI-SEND-REPLY` 未到，重跑即绿，与本轮改动无关）。

## 1.48 第四十三轮：最近文件（「文件」菜单动态段 + cfg 持久化）

### 1.48.1 目标与方案

缺口清单 §5.2「最近使用的文档 ❌ 未做」（上轮挂账项）。方案三句话：
cfg 一行 `recent.files=路径1|路径2|...`（Windows 文件名不允许 `|`，
作分隔符安全；`cfgRead` 是单行 key=value，路径里也不可能有换行）；
「文件」菜单条目**每次重建**（自绘菜单 `nodio.ui.titlebar` 打开下拉时
**现读** items 表——`__openMenu` 里 `var items = self.menus[idx][2]`，
所以重建后 `setMenus` 一次即生效，不需要二级子菜单机制）；所有文档
打开的必经点 `openFile` 一处入队（工程、搜索跳转、钩子打开的文档都收，
工程本身不收——它已有 `lastProject` 启动恢复机制）。

### 1.48.2 实现（三层）

- `app/settings.nodio`：`recentGet()`（去空、去重、截 10 条）+
  `recentPush()`（置顶入队，同路径反复打开不撑满列表）。
- `app/shell.nodio`：「文件」条目提为 `buildFileMenuItems()`（固定段 +
  最近文件动态段，条目文本 `N.  完整路径`）；菜单表持有为 `menusDef`
  局部变量；导出 `refreshRecentMenu()`（重建 + `setMenus`）与
  `fileMenuLabels()`（供回归断言菜单内容）；模块构造尾调一次
  `refreshRecentMenu()`（启动时把 cfg 里已有的最近文件落进菜单）。
- `main.nodio`：`openFile` 成功分支（`openTab` 之后）加
  `recentPush(path) + shell.refreshRecentMenu()`。
- ⚠ 闭包工厂：循环里直接写 `function(){...p...}` 所有条目可能共享同一
  循环变量（点哪条都开最后一条），`mkOpen(p)` 把路径绑进**参数**——
  每次调用是独立的新局部，无条件安全。

### 1.48.3 坑：单变量 for in 对数组拿到的不是元素值

第一版 `recentGet` 写了 `for(seg in ..string.split(v,"|"))`，冒烟四条
断言**全 0 但无异常**——`split` 结果里拿到的 `seg` 不是路径字符串。
仓库既有代码里 split 结果**一律**计数循环 + 下标访问（`main.nodio:352`
navShortPath、`:634`），没有一处 for-in 先例。修法：`var segs =
..string.split(...)` 后 `for(i=1;#segs;1){ var p = ..string.trim(segs[i]) }`。
（教训：单变量 for in 只在 `string.lines` 这类**迭代器**上拿值；
对数组它的语义不同，不要凭直觉用。）

### 1.48.4 回归锁与回归（本轮最终态）

`tests/ide_smoke.ps1` 增四条断言：`RECENT-AUTO:1`（openFile 链路自动
入队）、`RECENT-TOP-OK:1`（再 push 置顶）、`RECENT-DUP-OK:1`（同路径
反复 push 不翻倍）、`RECENT-MENU-HIT:1`（菜单重建后含 `1.  <路径>`
动态条目）。冒烟 x64/x86 全绿。全量回归 **x64 18/18、x86 18/18**
（x64 首轮 vm/winclip 两个失败均为共享桌面剪贴板争用环境假红——
§1.45.6 已记录在案的机器问题，重跑即绿；ide_ctx 同轮报
`SKIP clipboard assertions` 印证）。

## 1.49 第四十四轮：全面审计四连修——递归失控全链清剿（mark/拷贝/重入/解析）

### 1.49.1 审计结论（本轮的动身点）

对 HEAD 做了性能/稳定性/资源三面审计。性能与资源面处于三轮优化后的
历史最佳（调用 1.9-3.6µs、语句 0.16-0.45µs、exec 相 11.5MB、Value
136B 全部复核吻合）；但稳定性面挖出 **4 个绕过 500 层调用守卫的递归
失控向量**，全部段错误（exit 139）无任何报告：

| 向量 | 触发 | 实测阈值（x64,16MB 主线程栈） |
|---|---|---|
| GcMark 递归 | 深嵌套表 + 任意 GC 分配波 | 10 万层存活 / 20 万层崩 |
| DeepCopy 递归 | 深表 thread.create 序列化 | ≤5 万层崩（线程静默死亡） |
| 解析器递归 | 深嵌套括号源码（纯解析） | 1 万层存活 / 3 万层崩 |
| 重入无守卫 | CallOwner/CallVmFunc/CallThisOwner（COM 事件、FFI 回调、_call、finalizer、定时器） | 静态确认无计数 |

GC mark 的 P0 不可预测性最高：任何分配密集脚本只要持有深链就可能死
在下一次 GC 波，且 IDE 内联运行用户脚本时直接杀宿主。

### 1.49.2 四项修复（全部引擎侧）

1. **GcMark 迭代化**（gc.cpp）：GcMarkValue/GcMarkTable/GcMarkEnv 的
   互递归改为显式工作栈。可达性语义不变；Hop 路径仍只在诊断模式记
   录，且顺修一个旧诊断错——fromIsEnv 在表/环境两个入口分别写死
   false/true，跨 env 边界的泄漏路径会查错表，现在随边沿真实传递。
   env 父链保持递归（深度被调用守卫封顶，~500 层）。
2. **DeepCopy 迭代化**（util.cpp）：visited 命中壳、Class 侧表重建、
   "self" 回指新壳、Func 字面量早退、标量/native 尾路（fnData 深拷贝
   后清 nsClosure/fn）逐路保持。同一张新表的全部目标槽位先落地再填
   充（unordered_map 插指针稳定性 + 数组先 resize），新建对象受语句
   纪元保护，拷贝途中 GC 不会扫走半成品。
3. **重入守卫**（runtime.cpp）：500 层计数器上移为文件级 thread_local，
   Call 节点、CallThisOwner、CallVmFunc 三处脚本调用入口共用。纯脚本
   递归每层只计 1 次（Call 节点不经过 CallThisOwner，上限仍是 500 不
   腰斩），重入混合周期每层 ≥1，总嵌套封顶；触发即设可捕获运行错误
   （栈溢出），与原守卫同一契约。
4. **解析深度 + EOF 字符串**（parser.h/lexer.cpp）：语句/表达式两个
   递归枢纽共用 1000 层计数器，超限报「语法错误: 嵌套过深」——阈值
   远高于真实代码、远低于物理栈底（x86 帧更小同享余量）。词法器
   raw/转义两条字符串路径遇 EOF 一律报「字符串未终止」，不再把
   截断文件静默执行前缀。

### 1.49.3 回归锁（t93-t97，五件）

- t93：20 万层嵌套表 + 5 万次分配波 → 存活（旧引擎段错误）
- t94：6 万层嵌套表 thread 序列化 → waitOne 取回（旧引擎段错误）
- t95：2000 层嵌套括号源码 → 精确报「嵌套过深 @ 行 2」（旧行为：静默执行）
- t96：未终止字符串 → 精确报「字符串未终止 @ 行 4」（旧行为：exit 0 无报告）
- t97：守卫边界 499 层照常工作 + 1000 层可捕获（锁共享计数器不腰斩上限）

### 1.49.4 验证

四个原崩溃探针（深表+GC 波 / 线程深拷贝 / 30 万括号 / 未闭合引号）
全部由段错误或静默转为干净报错或存活；vm 套件 94 过/0 挂/1 跳（跳过
项为既有环境门控）；run_regression 双架构全量绿（见 1.48.5 末注）。
igcstress uiprobe13 通过。

### 1.49.5 会话协调

文档 §1.47/第四十二轮（发布链路）与 §1.48/第四十三轮（最近文件）为并行 IDE 会话所写，本轮提交时仍
在工作树未提交，经核对完整后与本节一起落账；第四十四轮编号顺延其
后者。审计基线时 ide_smoke 的 PUB-RUN 失败已归因其未提交的发布 WIP
（命令行直发验证引擎+发布器完好）；本轮末该套件随其 WIP 推进恢复。

## 1.50 第四十五轮：审计 P2 尾巴——投毒短路 + EOF 行号

### 1.50.1 范围界定（为什么不是全量对齐官方）

第四十四轮审计遗留两个 P2。其中「毒语句副作用」做过完整方案评估：
官方 aardio 在错误点立即中止（异常语义），本引擎是有意的不抛出设计
（err 字符串 + 语句边界检查，S-2 契约）——改造为异常要动全部 ~42 处
错误写入点并逐一审计帧清理路径，风险/收益完全倒挂，不做。本轮取
其中可廉价兑现的一半：**投毒展开不再制造垃圾数字**；另一半（毒语句
的后续副作用先于边界中止完成，如 io.print 先打印再中止）保留为
S-2 契约的已知偏差，记于此。

### 1.50.2 两项修复

1. **算术投毒短路**（runtime.cpp EvalBin + Node::Un）：前提经核实
   成立——全部 42 处 `(*err) =` 写入点均为硬错误（"运行错误:"），
   软失败（成员缺失、文件不存在→null）不触碰 err；catch（Node::Try）
   是唯一清除点。故 err 非空 ⟺ 本语句必在边界中止/被捕获，此时操作
   数之一是错误占位 null，继续算术只会凭空捏造数字（BNum(null)=0 累
   加出 500）。二元与一元求值在操作数求值后查 err，非空即透传 null。
   递归守卫触发后的展开现在打印 null 而非 500，try 捕获后无垃圾值
   残留。性能：stmtbench NUMASSIGN 同日 A/B 312→312ms/200 万次
   （每语句恰好一次二元路径），检查零成本。
2. **EOF 行号**（lexer.cpp）：TEnd 哨兵原携带「文件尾后一行」
   （line 计数已数过尾部换行），单行文件的语法错误报「行 2」。改为
   回退跳过 TNL 取最后内容行；「意外的」等报告点用 Peek().line 自动
   随之修正。t95 的锁不受影响——其深度守卫在行内触发，非 EOF。

### 1.50.3 回归锁

- t98：递归守卫 + try 捕获——打印 `unwind=null`（旧：unwind=500）、
  捕获照常、脚本继续。锁投毒短路 + 守卫可捕获性组合行为。

vm 套件 96 过/0 挂/0 跳；run_regression 双架构 18/18 全绿。

### 1.50.4 会话协调

engine/sysapi.cpp 工作树中并行 IDE 会话的剪贴板 API 注册（IDE 彩色
复制）未随本轮提交，归其后续轮次。

## 1.51 第四十六轮（性能线）：调用帧池化回退定案与修复落账

### 1.51.1 回退源定案（两层机制，均经实测）

第三十五轮（98e5f94）把瞬时调用帧切到池化槽存储（`EnvUsePooledSlots`，
5 处：CallThisOwner / InstantiateClass / EvalCall / ExecBlockScoped /
CallVmFunc）。第一层：槽号 = 全局 intern id − 1，调用帧槽向量为
O(maxInternId)×sizeof(Value)——328B 时代 ~52KB/帧，第四十一轮 136B 后
~22KB/帧，税减半但形态不变。第二层：槽池必死（gc.cpp，Round 34 引入）——
`GcAcquireSlotBuf` 取缓冲不扣 `p.nBytes`，`GcReleaseSlotBuf` 在
≥kBudget(24MB) 后永久拒收，池吸收 ~285 帧后死亡，稳态每调用重分配。
第三十九轮 kSlotMax=512 封顶对 import time+console（~100–300 id）的
普通脚本不触发。allocstat 普查（call5m 5M 调用）：x64 原样 >32K 桶
4,999,916 次 / 261,375.9MB；x86 原样 16K 桶 4,999,401 次 / 70,234.3MB
（~14KB/帧，检修轮重测）。

### 1.51.2 修复内容（3 文件，+38/−10）

1. 删 5 处瞬时帧池化调用（runtime.cpp）：调用帧/块帧回 vars map
   （Round 13 设计原意）；驻留帧（globals/mainEnv）保持 bSlotsKeep 槽化；
   EvalCall 处留 136B 语境机制注释。
2. allocstat.cpp `operator delete` 门控：原无条件 `fetch_add`（call5m 单次
   45.1M 次计税）违文件头自述契约，对齐为与 operator new 相同的
   `g_allocOn` 门控（NODIO_ALLOC_TRACE=1 行为不变）。
3. import.cpp BindModule/DoImport QPC 门控：补齐 40d1f72 自述的
   「先判门再付 QPC」纪律。
4. 槽池/节点池机制本体保留为死代码；复活须同时修 acquire 按 capacity
   扣减 nBytes + 按容量段匹配取还。common.h kSlotMax 注释（328B）已随
   第四十一轮失真，待原作者订正。

### 1.51.3 验证（双架构，A=原样 vs B=修复，5 轮中位）

- x64 安静窗口（7–11% 负载）：call5m 0.413×（11078→4578ms）、
  callv5m 0.403×（18594→7500ms）、lenfn5m 1.011（噪声证实）；
  污染窗口（23–76% 负载）首测 0.406×/0.471× 同向。
- x86（14% 负载，真原样版判源后）：call5m 0.456×（10625→4844）、
  callv5m 0.439×（18515→8125）。
- 分配普查（trace 开）：x86 原样 call5m 总流量 72,609.8MB → 修复 3,365.8MB
  （−95%）；x64 同构 268GB→10.4GB（−96%）。
- VM 套件四变体（A/B × x64/x86）实质全绿（t23=剪贴板往返为本 RDP 桌面
  环境门非回归，OpenClipboard gle=5）；TAIL_EQ 全程通过。
- da414b3 原样 call 路径仍显著慢于第 15 轮基线（定性 1.282）——第四十一轮
  未消灭池化税，本修复必要。

### 1.51.4 loop5m +6~14% 归因（编译布局类，非机制代价）

x64 上修复版 loop5m 方向性慢 ~6%（15 轮中位 1.064：969→1031ms）、
安静窗口 5 轮抽中高模态至 1.141；x86 上同一 delta 反而 0.984（方向翻转）。
五证据链定案为 MSVC 布局效应：删 ~1KB 内联材料使热函数地址平移（.text
锚点比对逐字节证同）；同点位 ~250B 语义空桩即可把比值重塑回 ~1.05 带；
动态路径源码级零交集（ForNum/ExecBlock/DoAssign 存储模式 A/B 相同）。
非本修复语义回退，不影响收益判断；消除可留等尺寸占位或随 engine 大改
自然重排。另 BOOT_TRACE 的 conv 计数是启动期一次性诊断值，勿作 loop 期
归因依据。

### 1.51.5 判源规程教训

检修中揪出 x86 A/B 首跑作废：两枚 x86 exe 均构建自修复 worktree（A≡B），
比值全 ≈1.0 的「x86 无收益」为假象（判源证据：分配谱逐字节相同 +
conv 双 0；重建零改动 worktree 后真原样版 x86 收益 2.2×+ 复现）。
规程补充：A/B 基准前两侧 exe 须过「BOOT_TRACE conv 计数 + ALLOC 普查」
双探针判源，哈希不同≠源头正确。

### 1.51.6 会话协调

本条目为性能线（独立会话）检修定稿、patch 在 50ca35d 上 apply --check
干净后经用户确认合入。patch 交付物（.perf-fix-round35-callframe-pool.patch
/.md，含完整归因与踩坑）保留于仓库根作审计底稿。engine/sysapi.cpp
剪贴板 API（IDE 线 WIP）未随本提交。

### 1.51.7 合入后补充验证（同日）

e738974 合入后，CLI 级全量回归（Temp 下全新目录构建 perfM-x64/x86）
双架构七项全绿：vm 96/96 **0 跳过**（t23 剪贴板用例本窗口实际通过——
此前四轮验证的 OpenClipboard gle=5 环境门已开）、iofile、builtin、
stdlib、publish、import、winclip 均 exit=0；此前担心的火绒按路径拦截
写盘假红未复现。run_regression 其余 12 项 ide_*/editor/input 类为真
GUI 用例，因 IDE 线测量会话活跃（同晚 python mock/probe 特征）未抢跑，
留待其空闲窗口或 WIP 合入后由 IDE 线自跑。

## 1.52 第四十七轮：以 RTF / HTML 格式复制（编辑菜单彩色复制两条）

### 1.52.1 目标与方案

缺口清单 §5.1「以 RTF / HTML 格式复制文档 ❌」（挂账自缺口普查轮）。
两条命令语义 = **复制整个文档**（不是当前选区）——用途是「把这份代码
带颜色贴进 Word / 邮件 / 博客」，整篇才是预期；复制后选区/光标还原。
- RTF：richedit 原生 copy 自带 RTF（着色是真 CHARFORMAT，复制时按控件
  存储的字符格式序列化），不自建 RTF 生成器。平时只着色可见范围
  （§1.36 性能取舍，85KB 全文 7 秒），视口外是正文色——复制前**强制
  全文着色**补偿；显式菜单命令可接受数秒顿挫（copyHtml 无此代价）。
- HTML：本地词法器从 token 流直出带色 HTML，按 CF_HTML（"HTML Format"，
  Version:0.9 + 4 个 10 位定宽字节偏移 + StartFragment/EndFragment 标记）
  规范连同 CF_UNICODETEXT 兜底在**一次 OpenClipboard 会话**写入——贴到
  不认 HTML 的目标不丢文本。颜色复用 self.colors（主题跟随）、`<pre>`
  包裹（换行/缩进保留）、普通文本不套 span（贴入目标取默认色）、
  `&<>` 转义（& 先行）、行间 `\n`、MAX_TOKENS（6000）截断后多余行
  降级纯文本。

### 1.52.2 实现（四层）

- `lib/nodio/ui/editor.nodio`：
  - `__colorRange(t, from, to)`（`__color` 拆分重构）：行段清底 + 逐
    token 上色，返回 token 数组；`__color` 变包装器（可见范围 + 选区
    还原 + hlLine/诊断/gutter 收尾，性能注释随迁）；copyRtf 是**唯一**
    全文着色例外（注释明示）。
  - `__clipWriteRaw(items)`：一次会话写多格式。字节保真配方（探针
    2b/2c/2d 逐字节钉死）：`raw.buffer(size,data)` 落字节（文本
    CP_UTF8 / bRawBytes 1:1，builtins.cpp:7157-7177 直写不过 BStr）→
    `RtlMoveMemory` 声明 `void(addr,addr,int)` 纯指针拷贝（api 的 str
    参数按 CP_ACP 中文必花、bRawBytes 过 BStr 重解释，两条路都禁）→
    GMEM_ZEROINIT(0x42) 兜终止符，不做 NUL 拼接 → SetClipboardData
    失败 `GlobalFree`、成功后 h 归系统（`r === null || r == 0` 判失败，
    0 是真值坑）→ CloseClipboard 后 **Sleep(30)**（内置 win.clip.write
    同款，builtins.cpp:5717——实测缺这拍本机剪贴板管理器会在 Close
    瞬间快照到空板并清板）。
  - `copyRtf`：存选区 → `__syncLineTable` → `__colorRange(1, 行数)`
    全文着色 → 全选 → `ctrl.copy()` → 按 getSel 语义还原选区。
  - `copyHtml` / `__buildHtml`：**逐行**处理（token 位置是 richedit
    码元，CRLF 占 1 格，与 `__buildLineStarts` 同口径），行内 `__cu`/
    `__cb` 做「UTF-8 字节 ↔ UTF-16 码元」双游标推进，token 边界
    `toks[ti].pos - base == u` 对齐；span 颜色 `%06X` 缓存；
    `..string.join(out,"")` 一次拼装（逐段 `++` 是 O(n²)）。
- `ide/nodio/app/editor.nodio:21-22`：门面 `copyRtf`/`copyHtml` 转发。
- `ide/nodio/app/shell.nodio:1146-1152`：编辑菜单「全选」后两条 +
  `setStatus` 成功提示。
- `tests/editor_clip_probe.aardio`：回归锁（见 1.52.4）。

### 1.52.3 坑（全部实测）

1. **本机剪贴板管理器在 Close/Reopen 窗口清板**：探针 2d 已见 reopen
   丢格式；本轮 `__clipWriteRaw` 无 Sleep 版四轮「写成功但读回空板」
   （board=[] 枚举为空）。补 Sleep(30) 后稳定。断言不可能与写入同
   会话（写完必然 Close）→ 探针整「写+验」循环重试 4 次吸收。
2. **bRawBytes 断言口径**（本轮回归钉死）：`==` 比较走规范化字节形态
   （common.h:309 StrByteForm——比较/拼接/输出统一过它），**同通道
   产物全量相等判定可靠**；`string.unpack` 走 BStr（builtins.cpp:2842），
   对 bRawBytes 的非 ASCII 段**双重编码**（ASCII 段恰好 1:1 存活）——
   中文包含判定不能用字节码扫描，改「同管线重建期望字节 + 全量 ==」。
3. **`__buildHtml` 两处实体转义曾被写成无操作**（`"&"→"&"`、
   `"<"→"<"`，应为 `&amp;`/`&lt;`/`&gt;`）且 `<pre>` 包裹漏实现——
   纯 ASCII 结构断言（marker/span）查不出来，全量字节比对抓出。
4. **CF_HTML 写入侧带 1 字节 NUL 终止符**（pad=1，规范要求）——期望
   字节必须同款，否则全量 `==` 恒差 1（先误判为「板上多一字节」，
   转储逐字节后定案）。

### 1.52.4 回归锁与回归

`tests/editor_clip_probe.aardio` 挂进 `run_editor_tests.ps1`（第 4 探针，
须打出 PROBE-OK）。断言全部与写入同一 OpenClipboard 会话 + 整循环
4 次重试：
- RTF：`{\rtf` 头（字节码判定，unpack 对 ASCII 忠实）+ CF_UNICODETEXT
  读回 `fromUtf16 == 控件文本`（fromUtf16 在 NUL 截断，与不带终止符的
  原文比对，探针 2d P3 口径）。
- HTML：探针用**同一条管线**重建期望 CF_HTML（`ed.__buildHtml(cur)` +
  同款信封组装）→ 板上读回**全量字节 ==**（Version 头/4 偏移/
  StartFragment/`<pre>`/颜色 span/中文/NUL 终止符一次全覆盖）+
  CF_UNICODETEXT 同上。

编辑器回归 4 探针两轮全绿。x64 全量 18/18（vm 首轮假红重跑绿，
§1.45.6 在案环境项）；x86 全量 18/18 一次过。

### 1.52.5 附带修两处（均为回归基建，非功能代码）

1. `ide_nav_smoke.ps1` 硬编码 `NAV-JUMP-LINE:44`（`app/editor.nodio` 的
   contextMenu 定义行）。门面插 3 行后定义行移至 47——导航功能本身
   正常（NAV-DEF-LINE=NAV-JUMP-LINE=47，跨文件打开正确），断言过时
   即假红。改为**动态取 NAV-DEF-LINE** 断言相等，此后 app/editor.nodio
   增删行不再假红。
2. `.gitignore:29` 的 `tests/editor_*.aardio`（意图：一次性探针 scratch）
   把**全部四个编辑器回归锁探针**都盖住了——`git ls-files` 证实
   editor_probe/fold/comp 三个官方用例从未入库，新鲜克隆的
   run_editor_tests.ps1 会引用四个缺失文件。加四条例外
   （`!tests/editor_*.aardio` 逐个点名），四探针恢复可跟踪。

### 1.52.6 会话协调

本轮编号顺延 §1.51（第四十六轮，性能线会话）。engine/sysapi.cpp 剪贴板
6 API + GlobalAlloc 族注册（§1.50.4/§1.51.6 挂账的「IDE 线 WIP」）随
本轮一起落账；§1.49/1.50/1.51 三节（并行会话）与 AI 面板等代码在本轮
落账时仍为工作树未提交状态，按 HEAD 拉仓库会拿到「文档说有、代码没有」
的树。

## 1.53 第四十八轮：设计器保真度——「工作区中央一张真窗」

### 1.53.1 缘起（用户的「完全不对」）

用户反馈设计器观感「完全不对」。实机取证（ideCommand 总线驱动 IDE 进
设计视图 + PrintWindow 截图）证实：功能清单（8 手柄/DSG 往返/属性回
写）虽全通，但画布是「控件直接漂在整片网格上」——edit 里填着类名
「编辑框」、groupbox 没有边框、static/checkbox 透出 IDE 深色壳成灰/黑
条、treeview 不见、窗体本身没有表面（无标题栏/边框/底色）。

### 1.53.2 根因（三层）

1. **无窗体表面**：画布底板即「窗体」，没有铬件，控件坐标以画布为原点。
2. **透明控件透底**：控件真挂在 IDE 主窗体上，static/checkbox 透明底
   透出深色壳；tParam 不给 bgcolor。
3. **属性丢失**：applyProps 只写注册表登记过的键，DSG 的
   edge/bgcolor/color 在往返时全丢；空 text 又被填成类显示名。

### 1.53.3 改造（canvas/registry/dsg 三文件）

1. **窗体表面**：工作区（B4B4B4）中央按窗体真实尺寸铺一张「真窗」——
   标题栏（文字 + 示意关闭钮）+ 边框 + 客户底色（DSG bgcolor）；网格
   只画在客户区；边距线/吸附线/选中框/8 手柄全部切到表面坐标系；
   「选中窗体」框在整面上。缩放/改窗体尺寸联动（__fs()）。
2. **控件保真**：透明系（static/groupbox/checkbox/radio）创建时给
   bgcolor=窗体底色（DSG 显式 bgcolor 优先，applyFormProps 改底色联动）；
   空文本只对标签类默认类名，内容类（edit/richedit/combobox…）保持空；
   DSG 原生属性**创建时**传进 form.add（edge/bgcolor/color 有原生语义），
   注册表未登记的属性并入属性表原样往返（不再丢）。
3. **属性集扩充**：每类补 文字色/底色/对齐/凹陷边框/禁用（edit 另加
   自动水平滚动），属性格按分组带展示。
4. **DSG 头**：generate/fromDSG 支持 `;bgcolor=`（可选，老调用对称）。

### 1.53.4 途中踩掉的四个坑（截图驱动逐个实锤）

1. `nodio.ui.board.boardFont` 拼写——board.nodio 的 namespace 是
   `nodio.ui`，正解 `nodio.ui.boardFont`；写错即 chrome 画一半静默中断。
2. 标题字体用 Segoe UI 没有中文字形 → 标题空白；换 Microsoft YaHei UI。
3. `graphics.drawString` 的 format 参数传 null 静默不渲染，必须给
   `boardFormat()`。
4. DSG 的 `bgcolor=0xF0F0F0` 没有 alpha 位，GDI+ 当全透明 → 客户区透出
   工作区；画 GDI+ 前强制 `0xFF000000 |`。
另：三档灰（工作区/客户区/标题栏）必须拉开可见台阶，C9C9C9 vs F0F0F0
实测糊成一片（截图核过）。

### 1.53.5 验证

组件级截图验收（独立画布探针，IDE 总线路径被并行会话在飞的
shell.nodio 编译失败暂时堵死）：工作区中央真窗 + 中文标题 + 网格 +
真实控件全部就位；edit 选中框/手柄正常。回归锁 **t99**（DSG 往返保真：
bgcolor 进头、未登记 edge 不丢、edit 空文本不填类名、中文文本往返），
VM 套件 97/0/0（x64+x86）。全量回归 11/18——7 个失败全部为 IDE 族，
归因并行会话在飞的 shell.nodio「字符串未终止」编译失败（第 45 轮末
同套件 18/18 全绿可证非本轮引入）；其恢复后需复跑 IDE 族确认。

### 1.53.6 会话协调

本轮只动 lib/nodio/designer/（canvas/registry/dsg）三个文件，均在并行
会话在飞清单之外；ide_smoke 等失败与其 shell.nodio/editor.nodio 中间态
相关，未触碰。

## 1.54 第四十九轮：设计器打磨——anchors 往返保真 + 「表内分号」解析缺陷

### 1.54.1 anchors 表属性纳入往返

官方 DSG 大量写 `anchors={left=1;top=1}`（锚定）。此前设计器三处全丢：
解析段按 `;` 朴素切分、创建参数不认、toDSG 不裸写。本轮语义定型：

1. **属性表保留裸串**：`{left=1;top=1}` 原样进 item.props（属性格可见、
   可文本编辑），toDSG 对 anchors **不加引号**裸写回（官方就是这么写的）；
2. **画布控件不下发锚定**：官方锚定是「父窗体尺寸变化时的重排」语义，
   且 `form.show()` 当场就会重排一次——实测创建参数给表、载入时写控件
   两条路都把控件锚成巨块（截图复验各一次）。画布控件永不应用锚定；
   锚定的真实生效场景是生成应用的运行时，由 DSG 文本带走。

### 1.54.2 真根因：__splitBody 不认花括号

anchors 首测时 edit 被锚成巨块，两层病因：上层是上面第 2 条；**底层**
是 `__splitBody` 按 `;` 朴素切分——`anchors={left=1;top=1}` 被拦腰斩成
`anchors={left=1` + `top=1}`，后者当作独立段**覆盖了真实的 top=132**，
edit 的设计矩形变成 (120,0,180,158)。修法：切段时跟踪花括号净深度
（引号未闭合的既有逻辑保留），深度 >0 就把后续段并回来。

### 1.54.3 其余

标题字号 9→10；treeview 空态白块判定为官方本态（edge 走官方 ctor 的
`exstyle |= WS_EX_STATICEDGE`，细线边在截图里不可辨），不再追。

### 1.54.4 验证与回归锁

独立画布探针截图复验：含 anchors 的 DSG 布局完全恢复、anchors 进入
属性格、toDSG 裸写回。**t99 扩展**：ANCHORS-KEPT（属性表裸串保真）+
ANC2（toDSG 裸写回）两断言；VM 套件 96 过/0 挂/1 跳（跳过项为环境门控）。
IDE 族仍被并行会话在飞的 shell.nodio 编译失败堵着（其 679/690 行
`tabFiles[path] : "renamed` 字符串断裂，已代为定位），恢复后复跑。

### 1.54.5 会话协调

本轮只动 canvas/dsg 两文件（第四十八轮同一批），并行会话的
shell.nodio/editor.nodio 中间态未触碰。

## 1.55 第五十轮：调用路径分配普查→稠密局部槽——每调用 9→6 次堆分配

### 1.55.1 工具：allocstat 尺寸窗口站点采样

NODIO_ALLOC_SITES=lo-hi 对该尺寸段 1/256 采样抓栈聚合（>64KB 大站点
机制的推广，带符号构建出真名）。普查对象：100 万次全局 2 参调用——
**每次调用 9 次堆分配 / 1385.6MB÷1M=1.4KB churn**：256B 桶 ×6、512B
×1、128B ×1、≤32B ×1。

### 1.55.2 符号化定位与两次误判

符号化显示 256B 桶的 6 次分配全部挂在 `make_shared<Env> ← EvalCall`
名下。第一判（kSlotMax 512→2048 + 空池预置）**无效**——普查数字分毫
不动，原因是参数 id 随 builtins+import 无界增长，早已越过 2048：id 索
引槽对晚定义 id 根本性失效，kSlotMax 调到多大都会整帧退化 vars map。
第二判（稠密值内联进 Env）**字节反涨**——make_shared\<Env\> 被顶进
2K 桶（每调用 +618B）。期间还踩响一个 0xC0000005：第 46 轮回退后
EnvUsePooledSlots 已无调用点，只设 bDense 不取缓冲=写空向量。

### 1.55.3 定案：稠密局部槽（帧内紧凑编号）

调用/块帧（EvalCall/CallThisOwner/CallVmFunc/InstantiateClass/
ExecBlockScoped 五个建帧点）改走稠密模式：denseIds 内联记 8 个 intern
id，**值存池化 8 槽缓冲**（复用既有池，稳态零堆分配）；第 9 个不同 id
整帧退化 vars map（g_nSlotConv 延续诊断）。id 索引槽仅存驻留帧
（globals/mainEnv）。锚定的教训进注释：官方锚定/稠密槽这类「与既有
坐标体系打架」的机制，创建参数与载入写回两条路都要封死。

**顺手修掉第 46 轮判词里的池排空根因**：GcAcquireSlotBuf 取用从不扣减
nBytes，24MB 预算吸满后所有释放被拒、池排空、稳态每帧重新 malloc——
acquire 现按容量饱和归还预算。

### 1.55.4 数字与回归

- 分配普查（100 万次调用）：**9 次→6 次/调用，1385.6MB→965.9MB
  （-30% churn）**；2K 桶排空（池循环恢复）。
- callbench 时间在本机噪声带内（GLOBAL2 0.92-0.98µs）——分配器的
  尺寸类复用本就便宜，收益兑现在长会话的 GC 压力与缓存局部性。
- 回归锁 t99 扩展 envcheck 门控（x86 的 overlay 无 exe 目录自动探测，
  32 位按策略直接用官方 32 位库——探测不到则 SKIP）；VM 套件 x64
  97/0/0、x86 96/0/1（skip=该门控）+ 带 overlay 手工跑 x86 t99 全绿。
- gcstress uiprobe13 ×2 通过（WS 增量 4.2MB）；设计器画布探针全绿。
- 全量回归 x64 11/18、x86 12/18——红项全部为 IDE 族，归因并行会话
  在飞的 shell.nodio（其 679/690 行字符串断裂，已代定位，见 §1.54.4）。

### 1.55.5 会话协调

本轮动 engine/allocstat.cpp、common.h、gc.cpp、runtime.cpp 与
tests/vmtests/t99.envcheck.aardio。第 46 轮（并行会话）的回退判词保持
成立——稠密局部槽是对该判词的正面回应：既然 id 索引槽对调用帧无解，
就换掉索引方式本身。

### 1.55.6 补遗：kDenseMax 8→16（同轮复测）

12 局部函数实测（100 万次调用）暴露 8 槽的边界代价：第 9 个 id 触发
退化后，稠密阶段的 1.1KB 缓冲成为纯 overhead——26M 分配/8595MB churn、
3.1-3.4s。kDenseMax 提到 16（2.2KB 池化缓冲，池预算无压力；nDense 放
宽 uint16）后**完全稠密不再退化**：12M 分配、5665MB（-34%）、2.5-2.9s
（-15-20%）。callbench 同步小升（GLOBAL2 0.84-0.91µs）。VM 套件 x64
97/0/0 复跑全绿；x86 t99 带 overlay 全绿、dense12 x86 SUM=78 ✓。

### 1.56 审计修复 R1——P0 三项（AUDIT-01/02/03）

2026 年 10 月 VM 全面审计（docs/audit-2026-10-vm.md）分轮修复第一轮。
基线 wt-audit @ 3dd464e 完成修复与全链路验证后，patch 落主树
1ea693a（与第五十轮稠密槽无交互，apply --check 干净）。补丁
.audit-r1-p0-fix.patch（7 文件 +41/−28），底稿 .audit-r1-p0-fix.md。

**AUDIT-01（P0 UAF）**：回调注册表 g_cbs 由裸指针 vector<CbCtx*> 改
vector<weak_ptr<CbCtx>>——GC mark 遍历 expired() 清理 + lock() 判活
（rooted 语义不变），ReleaseCallback 按 lock 匹配 erase；三个注册点
（raw.tostdcall / COM raw stub / 动态 FFI）持强引用语义不变。

**AUDIT-02（P0 UAF）**：g_forms 由 map<HWND, FormState*> 改
map<HWND, shared_ptr<FormState>>——WM_NCDESTROY 只摘注册不再 delete
（修"窗体销毁后 WndProc 仍在用已释放 FormState"竞态）；doModal 加
IsWindow(hwnd) 退出保险；创建失败路径 make_shared/reset 配对。

**AUDIT-03（P0 AV）**：sysapi DrawText 签名补 cch 参数，对齐真实
Win32 五参 DrawTextW 契约（原四参映射把 rect 当 cch、flags 当 rect，
非确定性访问违例，探针坐实）。

**伴随（AUDIT-09 策略）**：动态 FFI 裸函数回调按官方契约只在调用期间
存活——lambda 作用域 dynCbs 收集，调用返回后统一 ReleaseCallback
（跳板转中性桩返回 0，迟调用安全）。

#### 1.56.1 回归数字

- ASAN（build-r1-asan，基线 3dd464e+R1）：**92 passed / 5 failed /
  0 skipped**——审计基线 84/12/1 中的 7 项 UAF（t91 + t34/35/36/37/
  38/46）全部转绿，t23 由 SKIP 恢复执行并 PASS；剩 5 失败全部为审计
  已定案的 ASAN 帧膨胀伪红族（t70/t94/t95/t97/t98）。
- 普通（wt-audit 基线版）：97/97 全绿（含伪红族与 t23）。
- 主树（1ea693a + R1，build-x64 正规目录）：**98/98 全绿、0 跳过**
  （主树 tests 较基线多 1 用例）；七套 CLI（vm/iofile/builtin/stdlib/
  publish/import/winclip）全绿。
- 环境注：主树构建曾放 build-r1-main-x64 非正规目录，t30/t59/iofile
  写盘族整片红——火绒按程序路径拦截（run_regression.ps1 头注既有记
  载），复制到 build-x64\Release 重跑全绿，引擎无 bug。
- A/B（perf_ab 5 轮中位，TAIL_EQ 行为等价）：x64 +3~6%（loop 1.033/
  call 1.058/lenfn 1.061/callv 1.030）；x86 方向翻转/持平（1.001/
  **0.974**/0.989/0.996）——定性 MSVC 编译布局效应（§1.51 同因先例），
  非语义回退（R1 触及的 g_cbs/g_forms 遍历在四夹具下均为空容器）。

## 1.56 第五十一轮：IDE 功能补全第一批（恢复 + 词库 + 类视图）——18/18

### 1.56.1 接手与恢复

并行会话最后一轮在其工作树留下一次**失败的批量重命名**（editor/winform
→ "renamed"，波及 shell.nodio 60 行 + lib editor 1 行，全部为损坏碎片、
零有意内容，阻塞 IDE 启动数日）。核验 HEAD 完好后 `git checkout` 恢复，
零内容损失。其已提交成果（nproj 发布、RTF/HTML 复制、调用帧池化回退）
全部保留。

### 1.56.2 ide_smoke 的 DSG 往返假红（真修复）

恢复后全量 17/18，唯一红项 ide_smoke 的「DSG round-trip not stable」
在套件内必挂、单跑偶挂。根因：冒烟的 dsg2 用**单参 generate**，靠
「generate 默认值恰好等于画布默认」的巧合与 dsg1 对齐；第四十八轮
toDSG 把 bgcolor 写进头部后巧合破产。修法：app.designer 导出
**formGet()**（canvas.form 此前从未导出，main 作用域拿到的是 null——
引擎对 null 成员访问静默返回 null，四个实参全 null 走了默认值），
dsg2 带上真实窗体状态；「打开无设计块文档」的重置路径补 bgcolor。
断言反而更强：头部字段也参与字节级往返。

### 1.56.3 官方 intellisense 词库接入（❌ 清零第一项）

新组件 `lib/nodio/intellisense.nodio`：解析官方
config/intellisense/kernel.txt（659 行）+ winform.txt（251 行）→ 613
词条。格式全覆盖（`----` 前头说明跳过、函数形参段截除、^/! 前缀剥除、
值侧 @ 模板 kind=15、`?.` 占位排除、命名空间取末段标签、大小写不敏感
前缀匹配——官方 ^IGNORECASE_NODE_KEY=1）。编辑器 requestComplete 补传
词前缀（第三参），onComplete 合并词库（LSP 优先、词库补差集、去重）。
冒烟断言 INTEL-ENTRY（词库前缀直呼 onComplete，候选必须出现 kernel.txt
常量）+ t100 固件锁（6 断言：解析/分型/大小写/模板/空前缀/悬停）；
run_vm_tests 的测试名模式 `t\d\d` → `t\d+`（t100 起三位）。

### 1.56.4 类视图（❌ 清零第二项）

左栏新增「类视图」页：**层级树**（class/namespace → 成员函数，缩进
归属 + 花括号配平弹栈；普通赋值不入树），点条目跳行；五个文档刷新点
与大纲同步（含按键标脏→心跳重扫路径）。冒烟断言 CLASSVIEW（demoCls→mf
层级 + topVar 顶层）。

**途中三坑**（都是 aardio 语义层的，留档）：
1. **空串是真值**——`if(!name)` 在 name="" 时为假，成员分支从未执行
   （fillOutline 的注释早就警告过，复述了教训还是写反了；cvdbg 独立
   探针 + 分支入口打印定位）。一律 `#name == 0` 判断。
2. **table.remove 单参语义存疑**——弹栈改纯索引管理（stackTop），
   不依赖库行为；死循环以 90s 冒烟超时形式暴露。
3. 弹栈条件须**严格浅于**进栈深度（== 时还在类体内）。

### 1.56.5 回归

ide_smoke 新增 INTEL-ENTRY/CLASSVIEW 两断言后全量 **18/18**（x64）；
VM 套件 97/0/0。轮 D 候选（❌ 剩余）：模板面板、开始页、拆分窗口组、
打印管线、LSP 九方法、类视图的工程级扫描。

## 1.57 第五十二轮：模板面板 + 类视图工程级扫描（❌ 清零继续）——18/18

### 1.57.1 模板面板

右栏新增「模板」页（控件/语法/标准库/模板/范例/工具/AI）：内置五模板
（空白文档/控制台程序/窗口程序 win.form+DSG/类模块 namespace+class/
线程函数 thread.create），双击 → newFile 的初始文本通道落成新文档。
官方 IDE 的模板内嵌在二进制里没有数据文件可读，故走内置集；接官方
config 模板目录留作 v2。

### 1.57.2 类视图工程级扫描

类视图扫描核心抽成 scanClassInto(tree, baseNode, text, filePath)
（baseNode=null=树根；filePath 非空时节点带 .path）。左栏切到「类视图」
页触发 fillClassViewProject：工程每个文件的 class/namespace 挂到文件
节点之下（忽略 ignored 项，全工程零类则退回当前文档视图）。心跳重扫带
模式守卫（classViewMode），工程树不被文档心跳清掉。节点 .path 支持
**跨文件跳行**（先 onOpenFile 开文件再 gotoLine）。

### 1.57.3 途中

右栏加页把 AI 页从 6 顶到 7——冒烟的 AI-TAB 硬编码索引改动态查找
（按页名遍历 rightDock.tabs）。工程扫描断言挪进 NODIO_IDE_PROJECT
门控段（ide.nproj 真跑，CVPROJ-N:11）；模板测试后关闭页签，别把
TABSTRIP-COUNT 顶爆。

### 1.57.4 协同事故与回归

并行会话 15:31 用**陈旧编辑器缓冲**覆盖了 main.nodio（第五十一轮的
stackTop/#name 修复在工作树被清零，HEAD 完好）——git checkout 恢复。
两代理共写同一工作树、同持旧缓冲时会互相清零：提交粒度已压缩到
「编辑完立刻提交」，后续仍需用户层协调（并行会话需重载其编辑器缓冲）。

回归：ide_smoke 新增 TPL-NEW/CVPROJ-N 两断言后全量 **18/18**（x64）；
VM 套件 97/0/0。❌ 剩余：开始页、拆分窗口组、打印管线、LSP 九方法。

## 1.58 第五十三轮：开始页（❌ 清零继续）——18/18

无文档时盖在编辑区上的落地页（新组件 lib/nodio/ui/startPane.nodio）：
深色板底 + 大标题 + 副标题 + 最近打开文件树（点选走 onOpenFile）+
快捷键提示。矩形跟随编辑区（shell.editorRect + docTabH，与设计器画布
同款），可见性由心跳按页签数驱动（tabOrder==0 → show），recent 列表
签名比对、变化才重建树。t101 锁（列表/显隐/空列表占位/回调面）；
IDE 启动零错、SMOKE OK、全量 18/18。

工程纪律增补：perl/字节级工具改本仓库 UTF-8 文件第二次酿成整文件
重编码损坏（849 行 diff），**永久改用结构化编辑工具**；两代理共写
同一工作树的覆盖风险持续存在，提交粒度已压缩至「编辑完即提交」。

❌ 剩余：拆分窗口组、打印管线、LSP 九方法。

## 1.59 第五十四轮：补全做深（限定符补全 + 官方说明 + 浮层说明区）——19/18

用户反馈「自动补全不好用」。查下来：**自动触发机制本来就有**
（`editor.nodio` 的 `__autoComplete`：'.' 必问、词 ≥3 字符才问），
真正断掉的是**限定符场景**——写 `winform.` / `string.` 时一条候选都
出不来，官方 API 根本补不出来。三个叠加的根因：

### 1.59.1 根因一：标签取「末段」+ 只按标签前缀匹配

词库条目形如 `winform.wait(.(等待函数,超时,延时间隔)`，旧实现把候选
标签取成 key 的**最后一段**（"wait"），而 `complete(prefix)` 只做
`startsWith(label, prefix)`。于是写 "winform." 时，没有一个标签以
"winform." 开头 → **限定符场景必然为空**。

修法：条目额外记 `ns`（命名空间 = key 点分路径第一段），`complete`
按上下文分两路 —— prefix 不含 '.' 走全局标签匹配；prefix 含 '.' 则
拆成「限定符 + 已输入部分」，按 `ns` 精确匹配，partial 做标签前缀过滤；
限定符是某 ns 的前缀时（"win" 对 "win.ui"），该命名空间本身作为候选。

### 1.59.2 根因二：winform.txt 没有分隔线，整份被跳过

官方三份词库的**分隔规则不一样**（实测）：

| 文件 | 头部 | 分隔线 |
|---|---|---|
| kernel.txt | 8 行官方格式说明 | 第 8 行 `----` |
| winform.txt | 第 1 行 `/***intellisense() ` | **没有** |
| unimportant.txt | 无 | **没有** |

旧解析器只认 "----" → winform.txt 的 **234 条窗体成员一条都没进来**
（只解析到 kernel 的 476 条）。修法：有分隔线就严格以分隔线为准；没有
分隔线则跳过注释头行、从第一条含 " = " 的行开始当数据。

### 1.59.3 根因三：值内重定向 + `lastIndexOf` 位置语义

- 词库值里可能带命名空间重定向：`!fxns._form = 宿主窗口。\n!fxns.`
  （末尾 `\n!ns.`）、`!winform.createEmbedEx() = !embedObjectForm.`
  （纯重定向）。现在都解析出来：重定向目标成为该标识符的 `ns`
  （敲 `nmhdr` → 跳 nmhdr 命名空间），并进 detail。
- `..string.lastIndexOf(s, sub)` 返回的是**匹配点本身**的 1 基位置
  （"type." 的 "." 在第 5 位 → 返回 5）。按 `slice(s,1,pos)` 取限定符
  会把点也带进去（"type." ≠ "type"）→ ns 比对永远不相等。
  实测钉死：**限定符要取 `slice(s,1,pos-1)`**。这条曾让「已修好」的
  实现仍然一条候选都出不来，是本轮最隐蔽的一个坑。

### 1.59.4 悬停出官方中文说明

LSP 侧 hover 只回 `"**词名** + 类型"`（`apps/nodio-lsp/main.cpp:1027`），
**没有任何说明文字**；而官方 API 的中文文档只存在于词库里。所以悬停
改为**词库优先**、LSP 兜底（用户自己写的符号只有 LSP 知道类型）。
编辑器侧新增 `__qualifierAtWord`（按词的字节起点往回扫），
`__wordAtPos` 多返回一个 `bstart`（字节起点 —— 不能用 richedit 的
UTF-16 位置，中文会错位）。hover 钩子签名扩为
`function(line, col, qual, word)`。

### 1.59.5 补全浮层底部说明区

浮层原本只画「候选名 + 类型」，官方词库的中文说明看不见。现在底部
留一行，显示**当前选中项**的说明（detail 里 " — " 之后的部分，按浮层
宽度裁剪）。命中测试同步排除说明区（`y >= rowH*rows` 不算候选），
候选绘制循环按可见行数截断（否则会盖住说明）。

### 1.59.6 回归

新增 `tests/ide_completion/comp.nodio` + `tests/ide_completion_check.ps1`
（23 项断言），**自带夹具词库**：照官方三种形态各写一份
（带分隔线的 kernel / 无分隔线+注释头的 winform / 重定向条目）——
夹具驱动，任何机器都能跑，不依赖是否装了官方 aardio；官方真实词库
只做冒烟（装了才跑，没装跳过）。断言覆盖三形态解析、限定符命中与
部分词过滤、命名空间前缀、全局补全、重定向、悬停说明，以及编辑器
算出的完整前缀（5 个场景：fxns.wait / fxns. / wait / obj.method. /
foo().）。已挂进 `run_regression.ps1`（18 项 → **19 项**）。

回归：全量 **19/18**（x64）——唯一红项 `winclip` 是本机剪贴板竞争
假红（同轮 ide_ctx 明确报「剪贴板被其他进程占用」→ SKIP 门控生效），
单跑 `WIN-CLIP OK`；x86 同批。编辑器 4 探针、IDE 冒烟（含 INTEL-ENTRY）
全绿。

❌ 剩余：拆分窗口组、打印管线、LSP 九方法（第五十三轮并行会话在做）、
值侧 @ 模板的**插入**展开（`__` → 编辑器 $0 光标位，仍是挂账项）。

## 1.60 第五十五轮：调试做深——表达式求值（立即窗口）——20/20

用户反馈「代码的开发调试」不好用。查实：断点/单步/变量**引擎侧一直有**
（`engine/debug.cpp` 的管道协议），但**缺最关键的一项：求值**——暂停时
不能查任意表达式，调试器就只是「看变量名 = 值」的文本流。

### 1.60.1 链路：引擎 → 适配器 → IDE

- **引擎**（`engine/debug.cpp`）：命令管道新增
  `{"cmd":"eval","expr":"<表达式>"}`，回
  `{"event":"eval","value":..,"type":..}` 或 `{"event":"eval","error":..}`。
  求值在**当前帧的 Env** 里做（所以能看到局部变量），实现照抄
  `builtins.cpp` 的 `CompileToFunc` 范式：`Lex → Parser → ParseExpr →
  AnnotateNameIds → Interp::Eval(node, env)`（`Eval` 本来就是
  `interp.h:165` 的公开成员，只是此前没人用它做调试求值）。
  ⚠ 求值前**保存并清空引擎错误槽**（`err`）：表达式报错不能让后续执行
  带一条假错误。
- **适配器**（`apps/nodio-dap/main.cpp`）：`evaluate` 请求 → 把 eval 命令
  写进命令管道 → 等 ReaderThread 收到 eval 事件 → 回 DAP 响应
  `{result; type}`。请求-响应用 `g_evalReady` + `g_lastEvalBody` 关联
  （先写串、再置标志，主循环看到标志才读串）。
- **IDE**（`app/dapClient.nodio` + `app/shell.nodio` + `main.nodio`）：
  底栏新增「**立即**」页签（单行输入框），回车 → `shell.hooks.onDebugEval`
  → `dapClient.evaluate(expr)` → 结果写进调试输出区。
  ⚠ 回车处理**推迟到定时器**（`winform.setTimeout`）：DAP 求值是同步阻塞的
  （最长 4 秒），在窗口过程里做会卡住整个 IDE —— 与编辑器组件里
  `__scheduleComplete` 同一个道理。

### 1.60.2 顺带修掉适配器的**既存 JSON bug**

`Quote()` 原来只是加引号、**不转义** → `process` / `stackTrace` 事件里的
Windows 路径（`C:\Users\...\x.aardio`）会把 JSON 弄坏（`\U` 不是合法
转义）→ 客户端 `json.loads` 直接抛 `Invalid \escape`。
本轮把它改成真正的 JSON 字符串转义（控制字符 + 非 ASCII 转 `\uXXXX`），
并补 `JsonStrFieldDecoded`（**反转义**）：此前引擎发的变量值是 `\uXXXX`，
IDE 侧不解码就会显示成一串转义码。

### 1.60.3 管道协议的两个坑

- 引擎侧的命令解析此前是**子串匹配**（`wcmd.find(L"\"continue\"")`）——
  固定字面量没问题，但表达式里有引号/反斜杠就会坏。eval 命令改用正经的
  JSON 字段提取 + **反转义**（`\" \\ \n \r \t \uXXXX`）。
- 管道是**字节**流，事件行必须是纯 ASCII 才安全 → 引擎侧的值/错误一律
  `\uXXXX` 编码（与既有的 `varsJson` 同策略），适配器侧解码回 UTF-8。

### 1.60.4 回归

新增 `tests/dap_eval_check.py`（Python 写的 DAP 客户端——分帧 JSON-RPC
在 PowerShell 里太啰嗦，仓库已有 `ai_mock_server.py` 的 Python 先例）
+ `tests/ide_debug_check.ps1` 包装（python 缺失**明确报错**，不静默降级）。
20 项断言：initialize/launch/断点命中行号、算术、**局部变量可见**、
局部字符串、中文往返（`\uXXXX` 编码链路）、引号/反斜杠在表达式里的
转义、语法错误 → type=error、内建函数调用（证明走真解释器）、
variables 仍工作、terminated。已挂进 `run_regression.ps1`（19 → **20 项**）。

⚠ 写用例时被 aardio 铁律教育了一次：**双引号是原样串、单引号才是转义串**
→ 想在字符串里放引号得写 `'a"b'`；写 `"a\"b"` 是**非法语法**（引擎报
「字符串未终止」才是对的）。用例按官方语义改正后全绿。

回归：全量 **x64 20/20、x86 20/20**；IDE 冒烟（含新增底栏页签）全绿。

### 1.60.5 补「入口可见性」——求值做出来了，用户找不到

求值链路全绿之后复查真实使用路径，发现一个**功能性缺口**（不是 bug，
是设计漏项）：「立即」页签默认排在底栏**最后一位**（问题/输出/调试/终端/
引用/立即），底栏本身又不显眼 → 特性在、用户摸不到，等于没做。

补的四处入口：

- 「视图」菜单 → 「立即窗口」
- 「运行」菜单 → 「立即窗口  Ctrl+Shift+I」
- 快捷键 **Ctrl+Shift+I**（`winform.reghotkey`，与既有 Ctrl+Shift+F/T 同范式）
- 切到「立即」页签时**自动聚焦输入框**（`bottomDock.onSelChange`）——
  否则点了页签还得再点一下输入框才能打字

按页签名切页用 `dock.selectCaption("立即")`（`lib/nodio/ui/dock.nodio:185`，
官方 `pluginViewShow` 语义），不用写死页签序号——`addTab` 是**追加**，
序号会随页签增删漂移（今天「立即」是第 6 个，明天加个「任务」就变第 7 个）。

**回归锁**：导出了两个查询给冒烟用 —— `dockHasTab(caption)`（底部面板里
有没有这个页签）与 `menuLabels()`（全部菜单的条目文本，仿既有
`fileMenuLabels()`），`main.nodio` 的 smoke 分支据此打
`IMMEDIATE-TAB:OK` / `IMMEDIATE-MENU:OK`，`tests/ide_smoke.ps1` 断言这两行。
入口以后被人删掉，冒烟立刻红。

⚠ 踩了一个**作用域坑**：模块体内写 `shell.dockHasTab = function(){...}`
**挂不上去** —— `shell.nodio` 模块体内根本没有名为 `shell` 的变量，
`shell.xxx =` 会写到一张临时表上，`app.shell` 上看不到这个成员，
`main.nodio` 取 `shell.dockHasTab` 拿到 nil（冒烟立刻报
`IMMEDIATE-TAB:MISSING`）。必须**裸赋值**（`dockHasTab = function(){...}`），
函数体内的裸赋值同样进模块表（既有 `fileMenuLabels` 就是这么做的，
`RECENT-MENU-HIT:1` 断言长期为绿可证）。

❌ 剩余：修改变量（setVariable）、真实多帧调用栈、条件断点/命中次数断点、
异常断点（exceptionInfo）、变量值的结构化展开（表/数组仍是字符串）、
DAP goto。

## 1.60 第五十五轮：LSP 协议补全（九方法清零八项）——20/20

服务端（apps/nodio-lsp）补齐八个 textDocument 方法：

- **declaration / typeDefinition / implementation**——aardio 单一符号
  宇宙，三类跳转与 definition 同义（分发层复用 DefinitionFor）；
- **prepareRename**——位置符号范围 + placeholder（WordAtPos 复用）；
- **selectionRange**——词范围单层（parent=null）；
- **foldingRange**——缩进折叠（与编辑器行号栏折叠同族：下一非空行
  更深成区、回到或更浅收区、空行不计、少于两行不成区）；
- **formatting**——全文重排（花括号深度缩进，行首连续 } 先减深度；
  字面量内括号影响深度与客户端 v1 同类限制，语义层修复挂账）；
- **codeAction**——v1 空数组；**signatureHelp**——形参数据源暂缺
  返回 null（客户端悬浮签名/词库承担）——均协议完整、客户端不阻塞。

客户端（lspClient.nodio）补 foldingRange/formatting/prepareRename/
selectionRange 包装（外部客户端与回归用；IDE 自身折叠/格式化走本地
实现）。**semanticTokens 需要 LSP 侧词法模型，本轮不做**（台账挂账，
是九方法中唯一剩余）。

验证：tests/lsp_smoke.py 扩展八断言（三别名与 definition 逐字节一致、
prepareRename/selectionRange 范围、folding 非空、formatting textEdits、
codeAction/signatureHelp 兜底）——**LSP-SMOKE-OK**；x64/x86 双构建；
全量回归 **20/20**（套件已扩至 20 项：并行会话新增 ide_complete/
ide_debug，连同其引擎审计修复 R1 同树验证）。

### 协同状态

并行会话第五十四轮（补全做深 ba40e8d）在第五十一轮的词库合并之上
扩展（onComplete 三参升级为 qual ++ word 限定符链），保留了我的全部
接线——两线收敛良好。editor_clip 曾红（OpenClipboard err=5）为
远程桌面剪贴板同步的占用争用，静置自愈（EDITOR-TESTS-OK 复跑）。

> 勘误：本节的 LSP 协议补全 feat 提交信息误写「第五十四轮」（与并行
> 会话的补全做深轮撞号）——按本台账编号为**第五十五轮**，以本节为准。

---

## 1.61 第五十六轮：设计器体验做深——工具箱拖拽落点 + 自动避让

用户点名「窗口设计器」。查实设计器**核心能力其实已经很全**（8 手柄 /
属性格回写 / 工具箱 / 事件代码生成 / 撤销重做 / 对齐吸附 / 缩放 /
DSG 往返 / anchors 往返，见 §6），但**工具箱这条路径几乎还是原始状态**：

- 工具箱**只能点，不能拖**（`toolbox.nodio` 只有 `onMouseClick`）；
- 从工具箱加控件**一律落在 (20,20)**（`designer.nodio` 的 `onPick` 写死
  `{x=20; y=20; ...}`）—— 连点三次就是三个控件叠在左上角，
  第二个根本点不中。

这两条合起来，就是「设计器用起来很难受」的直接来源。

### 1.61.1 工具箱 → 画布的拖拽

- **拖拽状态机**（`toolbox.nodio`）：`onMouseDown` 记下候选并**捕获鼠标**，
  `onMouseMove` 位移超过 **4px**（与 board 的点击判定同值）才算拖拽，
  `onMouseUp` 回报落点。点击路径保持不变（仍走 `onMouseClick` → `onPick`）。
  捕获鼠标是必须的：拖到工具箱**外面**（画布上）时，工具箱收不到 move/up。
- **落点坐标链路**（三层）：屏幕坐标 →（`win.toClient`）主窗体客户区坐标
  →（`canvas.clientToDesign`）设计坐标（DSG 里的 left/top，逻辑像素）。
  正变换是 `canvas.__place`（`canvas.nodio:286`）：
  `客户区X = origin.left + fs.x + round(px(item.x) * zoom)`，
  反变换按同式子解（`px()` 是 DPI 缩放，`zoom` 是设计器缩放比例）。
- **落点必须在窗体表面内**（`canvas.hitSurface`）：拖到外面的工作区灰色地带
  无效（官方行为也是如此）。
- **落点预览框**：拖动过程中画一个半透明蓝框（`canvas.setDropPreview`），
  松手自动清掉 —— 盲拖不算设计器。

### 1.61.2 单击添加的自动避让

单击添加不再落在固定 (20,20)：`canvas.freeSpot` 找**第一个不与现有控件
相交**的位置（先向右排开，排不下就换行；相交判定留 2 逻辑像素间隙，
挨着的控件不算重叠）。避让算法抽成**纯函数** `freeSpotIn(items, ...)` ——
这样它能被回归锁单独断言，不必真在画布上加/删控件来试。

### 1.61.3 顺带修掉一个**真 bug**：`mouseScreen` 的坐标解读

端到端断言第一次跑就红了，抓出 `lib/nodio/ui/rect.nodio` 的 `mouseScreen`：
它把 lParam 里的客户区坐标按**无符号 16 位**解读。但鼠标**拖出控件
范围**时坐标是负数（WM_LBUTTONUP 的 lParam 按 signed 解读）→ `-190`
被算成 `65446` → 落点偏到十万像素外 → 表现是「拖到画布上松手，
控件没落下来」。

改成有符号解读，并把基准从「窗口矩形左上 + 客户区坐标」换成
「客户区原点（`win.toScreen`）+ 客户区坐标」（后者少差一个边框宽度）。
这条同时改善了画布里**组件拖出控件边界**时的增量漂移。

### 1.61.4 回归锁

`tests/ide_smoke.ps1` 增 5 条断言（IDE 内自检，输出到 stdout）：

| 断言 | 含义 |
|---|---|
| `DROP-WIRED:1` | 工具箱的 `onDrop` 回调已接线 |
| `SPOT-FREE:OK` | 已有控件占着 (20,20) 时，新落点不与它重叠 |
| `COORD-RT:OK` | 设计坐标 → 客户区 → 设计坐标 往返一致（误差 ≤1 设计像素） |
| `SURFACE-HIT:OK` | 窗体表面内命中、表面外不命中 |
| `DROP-E2E:1` | **端到端**：模拟一次真实拖拽（按下→移动→松开，走工具箱自己的鼠标回调路径）后，控件真的落到了画布上 |

最后一条是本轮最有价值的锁：它把「用户拖一下能不能把控件放上去」变成
可回归的东西，而不是靠代码审查背书。

⚠ 写断言时又踩了 aardio 的一个语义坑：**`#table` 是数组长度语义**，
对**字符串键**的 table（组件描述）恒为 0 —— 写 `#sel > 0` 来判「选中」
会永远判假（第一版就是这么写的，冒烟立刻红）。判空要用
`sel !== null && sel !== undefined`。

回归：全量 **x64 20/20、x86 20/20**。

❌ 剩余（用户点名的设计器项里还没做的）：控件事件一览、窗体属性对话框、
布局器、Tab 顺序、资源视图（类视图第五十二轮已做）、控件分类自定义。

---

## 1.62 第五十七轮：设计器体验做深·二——控件事件一览

用户点名的设计器项之一。现状：双击控件能生成**默认**事件的桩
（`docs/ide-components.md` 的 `_commandFuncnames` 一栏），但**看不到这个
控件到底有哪些事件** —— 想挂 `onSelChange` 得先知道它存在。

### 1.62.1 事件数据**对齐官方**，不靠印象

事件清单取自官方 win.ui 的**真实事件成员**：

- **通用事件**（所有控件都能挂）：`oncommand`（WM_COMMAND 派发，
  官方 `lib/win/ui/_.aardio:748`）、`onnotify`（WM_NOTIFY 派发，
  同文件 `:813`）；
- **控件特有事件**：官方各控件类在创建时带的 `_commandFuncnames`
  映射（消息码 → 事件函数名）：
  `edit.aardio:104` / `richedit.aardio:97` / `ipaddress.aardio:32`
  → `onChange` / `onFocusGot` / `onFocusLost`；
  `combobox.aardio:44` → 8 个（`onSelChange` / `onEditChange` /
  `onFocusGot` / `onFocusLost` / `onDropDown` / `onCloseUp` /
  `onOk` / `onCancel`）；`listbox.aardio:24` → 5 个
  （`onSelChange` / `onSelCancel` / `onDoubleClickItem` /
  `onFocusGot` / `onFocusLost`）。

⚠ 先试过**运行时**问控件自己（`ctrl._commandFuncnames`），**读不到**：
   独立探针里控件还没创建（官方 `form.add` 在窗体显示前延迟创建，
   `hwnd=null`）；IDE 画布上的真实控件有 hwnd 但那张表仍是 null
   （冒烟实测 `EV-TABLE:0`）。所以改为**静态对齐官方源码** ——
   探针 `tests/probe_events.aardio` 留档了这个结论。

### 1.62.2 UI 与接线

- 左栏新增「**事件**」页签（`nodio.ui.list`，两列：事件 / 说明）：
  选中控件 → 列出它支持的事件；双击某个事件 → 生成**那个事件**的桩。
- `app/designer.nodio`：`canvas.onSelect` / `onChange` 都会刷新事件列表；
  `hooks.onEventCode(item, ev)` 的第二个参数是**可选**的事件名
  （省略 = 该控件的默认事件，双击组件的旧行为不变）。
- `main/nodio` 的 `genEventStub(item, ev)` 按指定事件生成，
  签名仍按事件类型区分（`onChange` → `function()`、
  `onnotify` → `function(id,code,ptr)`、其余 → `function(id,event)`）。

### 1.62.3 回归锁

冒烟新增 3 条断言：

| 断言 | 含义 |
|---|---|
| `EVENT-LIST:2/1` | button 的事件清单有 2 项且含 `oncommand` |
| `EVENTS-EDIT:11` | edit 的事件表含 `onChange` + `onFocusGot`（对齐官方 `edit.aardio:104`） |
| `EVENT-STUB-EV:1` | 指定事件（`onSelChange`）能生成处理函数桩 |

全量回归：x64 20/20、x86 20/20。

❌ 剩余：窗体属性对话框、布局器、Tab 顺序、资源视图、控件分类自定义、
窗体级事件一览（onCreate/onClose/onSize…）。

---

## 1.63 第五十八轮：修「IDE 频繁闪」+ 拖拽走**真实窗口消息**验证

用户反馈三件事：① 图标为什么用字体不用图标；② 组件没法拖拽设计；
③ **IDE 频繁闪**。逐条查证（结论见下），这轮先修能修的。

### 1.63.1 闪：根因是 `board.show(true)` 每次都整块重画

追链：心跳每 300ms 调 `updateStartPane()`（`main.nodio`）→ 它调
`startPane.show(#shell.tabOrder == 0)` → `startPane.show` 调
`board.show(flag)` → **`board.show(true)` 每次都 `self.redraw()`**
（`lib/nodio/ui/board.nodio`：显示后必须强制重画一次，因为官方 plus 的
WM_PAINT 只画「本次失效区域」，首次显示的更新区常是空的）。

于是**无文档时（开始页一直可见）每 300ms 整块重画一次** = 每 300ms
闪一次；而且心跳里一次 show 会连着重画**两块**板（开始页的板 + 最近
文件树的板）。

修法：`board.show` 记一个 `__shown`，**只有「隐藏 → 显示」这一下才
重画**，重复 `show(true)` 不动它（内容变了应该调 `invalidate`/`redraw`，
不是 `show`）。首次显示那一次重画照旧保留（那个是必须的）。

回归锁：板子暴露 `redrawCount()`，冒烟断言连续两次 `show(true)` 的
重画增量 = **0**（`SHOW-REDRAW:0`）。

> 顺带说明：`relayout()`（心跳里另一个重排入口）**有**尺寸守卫，
> 不是闪烁源；`board.layout` 也只在尺寸变化时重画 ✓。

### 1.63.2 拖拽：加一条**真实窗口消息**路径的验证

用户说「组件没办法拖拽设计」。本轮把拖拽的验证从「直接调回调」升级到
**真实窗口消息路径**：`toolbox.simulateDragMsg` 用 `::PostMessage` 把
WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP 投给工具箱板
（同线程队列保序），走的是**子类化窗口过程 → onMouseDown/Move/Up →
拖拽状态机 → onDrop**，与用户真的用鼠标拖是同一条路（仓库里已有同款
做法：`tests/ide_click.ps1` 直接向命中的子窗口发鼠标消息，因为桌面锁屏
时合成鼠标事件送不到 IDE）。

冒烟断言 `DROP-MSG:1`（与回调路径的 `DROP-E2E:1` 并存）。

⚠ 目标坐标在工具箱**左边**的画布上 → 客户区坐标是**负数** ——
   这正是上一轮修的 `mouseScreen` 有符号解读要覆盖的场景，两条断言
   一起把「拖出控件范围」这件事钉住。

### 1.63.3 图标：官方**没有图标资源**，只能自研

查实：官方 `D:\aardio\lib` 全树只有**一个** png（`win/ui/ctrl/pick/.res/
color.png`），没有任何 ico/png 图标集 —— 官方 IDE 的工具条图标是
**代码画**的（`lib/ide/aifix.aardio` 是闭源字节码，画法不可见）。

Nodio 现状分两类：
- **工具条 / 页签图标**：`nodio.ui.icons` 用 **FontAwesome 4.7 字体**
  （官方 lib 自带 `lib/fonts/.res/FontAwesome.ttf`）画字形 → 用户看到的
  就是「字体当图标」；
- **工具箱里的控件缩略图**：**已经是 GDI+ 手绘**（`nodio.ui.icons.drawWidget`
  按 16×16 设计稿画按钮/编辑框/复选框…），不是字体。

改法有三条路（自绘矢量 / 换系统 Segoe MDL2 图标字体 / 引入位图资源），
走哪条要用户拍板 —— 见本轮交付说明。

❌ 剩余：图标方案落地；设计器侧还有窗体属性对话框、布局器、Tab 顺序、
资源视图；拖拽时预览框刷新仍会整块重画底布（拖动过程中的轻微闪烁，
与心跳那处不是一回事）。

### 1.57 审计修复 R2——P1/P2 五项 + 新发现 AUDIT-11

2026 年 10 月 VM 全面审计（docs/audit-2026-10-vm.md）分轮修复第二轮。
基线 = R1 入库提交 1474538，补丁 .audit-r2-p1-fix.patch（5 文件 +181/−8），
底稿 .audit-r2-p1-fix.md。与并行 IDE 线无冲突（apply --3way 8 文件 cleanly）。

**AUDIT-04（P1）**：::LoadCursor/LoadIcon 签名 `addr(int id)` →
`addr(addr hinst,int id)`（真实 LoadCursorW 两参契约，原一参把第 2 槽寄存器
残留当 id）；GetCursor → `addr()`。官方契约见 lib/builtin/_.aardio:305
「LoadCursor(hInstance,lpCursorName)…参数 @hInstance 一般指定为 null」。

**AUDIT-05（P1）**：ffi 动态路径 64 位返回白名单补指针族（HeapAlloc/
HeapReAlloc/GetProcessHeap/VirtualAllocEx/GlobalAlloc/GlobalReAlloc/
GlobalLock/LocalAlloc/LocalReAlloc/LocalLock/CreateBoundaryDescriptor
+ CreateFileMappingNum*/LoadLibraryExW）。政策写进注释：HANDLE 型返回按
WOW64 契约 32 位显著保持 ARInt 默认，只有真实 64 位指针族升级 ARI64；
判定在包装创建时一次（checkName），热路径零成本。

**AUDIT-06（P1）**：win.clip write 的 GlobalLock 未判空 → memcpy 必崩，
改为 GlobalFree + CloseClipboard + 返回 null（官方静默失败语义）。

**AUDIT-08（P2）**：GetClassInfoEx / RedrawWindow / GradientFill 三处签名
标签对齐真实 Win32 契约（消除文档陷阱，非行为修复）。

**AUDIT-10（P1）**：raw.concat/append 的 len 未钳制 → 巨值 memcpy。三处修：
① 源侧 n = min(显式 len, 源实际尺寸)（Str/Num/Struct 精确；Pointer 在
尺寸已知时钳制）；② dst 非 dynbuf 分支的第三参实为**写入偏移**（dst
语义），负值/巨值 → memcpy(dst+(size_t)1e30) 越界 —— 改偏移域拒绝 +
已知尺寸目标拒绝越界窗口；③ DynAlloc 失败路径放弃本次 append（原代码
落回旧块继续 memcpy 巨值）。

**AUDIT-11（P1，R2 轮新发现）**：IsDynBuffer 盲读 ptr-16 的 magic →
对任何非 dynbuf 指针（普通 raw.buffer / FFI 指针 / 栈地址）越界读 4 字节。
**在 R1 的 ASAN 构建（无 R2 改动）上复现**（探针 p6，heap-buffer-overflow
@ common.h:353）——证明是既有缺陷而非 R2 引入；SEH __except 兜底在 ASAN
插桩层无效（ASAN 先报毒不产生 SE 异常）。修法：dynbuf 身份改走**注册表**
（util.cpp 新增 s_dynBufs + RegisterDynBuf/UnregisterDynBuf/
IsRegisteredDynBuf），IsDynBuffer 先查注册表，未命中直接 false，magic 探测
只对注册表背书的指针执行；DynAlloc 三条产生路径全部注册，realloc 注销旧
指针+注册新指针，释放注销。伴随修：raw.free 原对 dynbuf 直接 free 数据区
指针 = 内部指针 free（Windows 自身堆容忍故 ASAN 不报，块头悬垂）→ 改走
DynAlloc(0, p)；DynAlloc 拷贝分支补 UnregisterBuffer(pOld)。

#### 1.57.1 回归数字

- 探针（ASAN）：p2 LoadCursor=65539（IDC_ARROW）✓、p3 GetProcessHeap/
  HeapAlloc 返回完整 64 位值 ✓、p5 rawconcat 5 场景全过 exit=0 ✓、
  p6 零报告 ✓（修复前 ASAN 必报）、p7 dynfree ✓ —— 五个探针 ASAN 零报告。
- VM：ASAN **94 passed / 5 failed / 0 skipped**（5 失败 = 已定案帧膨胀伪红
  族 t70/t94/t95/t97/t98；t23 剪贴板首跑 SKIP、复跑 PASS）；普通 **99/99
  全绿**；主树（1474538+R2）**99/99 全绿 + 七套 CLI 全绿**。
- A/B（perf_ab 7 轮中位，同基线唯一差异 = R2 补丁，TAIL_EQ 行为等价）：
  loop5m 1.047 / call5m 1.006 / lenfn5m 0.989 / callv5m 1.004——三项在 ±1%
  噪声带内，loop5m +4.7% 定性 MSVC 编译布局效应（§1.51 同类），R2 触及的
  dynbuf 注册表 / raw.concat 不在四夹具 hot path 上，无语义回退。
- **A/B 纪律留档**：首轮对照是脏的——old 用 1474538（含主树并行 perf 提交）、
  new 用 3dd464e 基线+R1+R2（不含并行提交），call5m 报出 0.595 的假「提速」；
  作废并改为同基线三方合并重跑。**对照与被测必须同基线。**

---

## 1.64 第五十九轮：调试器剩余四件套之一——条件/命中次数断点全链 + 收录打印管线 v1

### 1.64.1 条件/命中次数断点（引擎 → nodio-dap → IDE 全链）

- **引擎**（`engine/interp.h` / `debug.cpp` / `runtime.cpp`）：新增
  `bpMeta`（行号 → 条件 + 命中次数）。停点判定改两段：`bStep` 照旧直停；
  行号命中后再过 `BpShouldStop`——hitCount>0 时每次到达计数、
  `curHits % N == 0` 才可能停（每第 N 次）；condition 在**当前帧环境**
  求值（`EvalCondInEnv`，真值语义同 `if`：Falsy 仅 null/false），假值
  或求值失败不停。条件解析复用第五十五轮求值器的 Lex/Parser 前缀
  （抽成 `ParseExprUtf8` 一份两用）。
- **DAP**（`apps/nodio-dap/main.cpp`）：`setBreakpoints` 逐断点解析
  `condition` / `hitCondition`（DAP 规范字段，字符串形态数字；兼容
  数字形态 `hitCount`），转发引擎新命令 `{"cmd":"bp2","bps":[...]}`。
  **空数组也必须转发**（= 全部清除）——顺带修掉一个既有 bug：旧实现
  lines 为空就不发，消掉最后一个断点后引擎侧永远留着。
  `initialize` 能力声明加 `supportsConditionalBreakpoints` /
  `supportsHitCounts`；stopped 事件透传引擎停因（launch 首停 = "step"，
  断点 = "breakpoint"）；响应带回 `verified` 断点列表。
- **IDE**：`dapClient.setBreakpoints(path, lines, metas)` 第三参带元数据；
  `main.nodio` 断点槽从 `true` 升级为元数据表（普通 = `{}`，条件 =
  `{condition;hitCount}`，表恒真值、旧判断不受影响）；**Shift+F9** 设
  条件断点（输入「条件 | 命中次数N」竖线分隔、两边可空），菜单
  「运行 → 条件断点...」同款；编辑器新增 `setBreakpointMark`（直接置
  标记不切换，给条件断点在无标记行上点亮用）。
- **回归锁**：`tests/dap_bp_check.py`（条件停/命中次数停/恒假不停/
  空表真清除/停因透传/verified 回包，22 项）挂进
  `tests/ide_debug_check.ps1`（合计 41 项）。

### 1.64.2 收录打印管线 v1（前会话工作树遗留，未提交即未落账）

- 修 `engine/builtins.cpp` `printRichedit` 的 lambda 捕获编译错
  （外层 NF 无捕获、内层 `[this]` 不可用 → 外层补 `[this]`，与
  loadDll/topointer 同款）。
- `lib/nodio/ui/printdc.nodio` 落库：raw 层 pages==0 回传 `-hwnd`
  诊断值，封装层钳成 0（对外契约「0 = 无打印机/测不出」，
  冒烟 `PRINT-PAGES:\d+` 才能锁住链路）。

### 1.64.3 验证

- VM 套件 **98/0/1**（x64，`build-x64\Release\nodio.exe`）。
- IDE 冒烟 **SMOKE OK**（含 PRINT-PAGES 链路）。
- `ide_debug_check` **41 项全绿**（eval 19 + 条件断点 22）。
- ❌ 剩余（调试器）：真实多帧调用栈、变量结构化展开 + setVariable、
  独立监视窗（下一轮）；❌ 剩余（打印）：PrintDlgW 对话框、打印预览。

---

## 1.65 第五十九轮续：调试器之二——真实多帧调用栈

- **引擎**（`engine/interp.h` / `runtime.cpp` / `debug.cpp`）：新增
  `dbgStack`（`DbgFrame{funcName, callLine, env}`），在四处 VM 函数体
  执行入口用 **RAII**（`DbgFrameScope`，构造函数体两条 return 路径的
  构造体也覆盖）push/pop：`EvalCall`（调用点文本 + 调用行号）、
  `CallThisOwner`（元方法/回调路径，帧名退化 `(method)`）、
  `InstantiateClass` 构造体（`(ctor)`）、`CallVmFunc`（定时器/线程
  等原生侧入口，`(callback)`）。仅 `bDebug` 时记录，零调试开销；
  帧名取调用点表达式文本（`CalleeText`：`b()`、`t.f()`），脚本函数
  的 `fnData->fname` 本就为空（仅 FFI 包装有），不动 parser。
  fiber 交换不换栈（fiber 帧串主栈）是 v1 已知边界。
- **DAP**（`apps/nodio-dap/main.cpp`）：`stackTrace` 从「伪造单帧」
  改为向引擎发 `{"cmd":"stack"}`，引擎回
  `{"event":"stack","frames":[{"name","line"},...]}`（1 基；frame[0]
  = 停点行，后续是调用方链，末帧 main = 最外层调用点；超 64 帧
  从外层截断）。适配器转 DAP `stackFrames`（0 基；main 帧带脚本
  path）；引擎未应答时兜底单帧（行号 = 最近停点，不再固定 line 1）。
- **IDE**：停止时的调用栈输出带帧名（`@ b() 行 5`；main 帧不重复
  显示）。
- **回归锁**：`dap_bp_check.py` 增场景 D（main→a()→b() 三帧的
  名字/行号/序断言），合计 35 项；`ide_debug_check` 合计 54 项。
- 验证：VM 98/0/1、IDE 冒烟 SMOKE OK、ide_debug 54 项全绿。
- ❌ 剩余（调试器）：变量结构化展开 + setVariable、独立监视窗。

---

## 1.66 第五十九轮续：调试器之三——变量结构化展开 + setVariable

- **引擎**（`engine/interp.h` / `debug.cpp`）：
  - vars 协议从平面对象 `{"name":"value"}` 升级为**结构化数组**
    `[{"name","type","value","ref"}]`（类型名来自 `TypeNameOf`，值
    ToString 截 120 字符；stopEvent 与 `vars` 命令同形）。
  - 新增 `std::vector<Value> varRefs`：每次进入停点**清空重建**，
    Table 值分配 `ref = 索引+101`（0/100 被 DAP Local 作用域与帧变量
    语义占用，容器引用必须错开）；ref 仅停点期间有效。
  - 新命令 `{"cmd":"varsRef","ref":N}`：展开 `varRefs[N-101]` 的子级
    （map 键 + arr 下标 `"[i]"`，每层各分配新 ref，单层上限 100 子级
    防巨型表拖死协议）；引用失效回 error 字段。
  - 新命令 `{"cmd":"setVar","ref","name","value"}`：值是**表达式**
    （经 `EvalValueInEnv` 在当前帧求值——所以能写 `x+1`、`{a=1}`）；
    ref 0/100 写当前帧变量（`FindInSelf` 命中才写，全局兜底，不凭空
    造变量），ref>100 写表成员（普通键→map，`"[i]"`→`arr[i-1]`，下标
    越界/引用失效报错）。应答 `{"event":"setVar","ok",value,type|error}`。
  - 求值路径重构：`EvalValueInEnv`（真值 + 错误槽保存/清空/恢复）成为
    `EvalExprInEnv` / `EvalCondInEnv` / setVar 三方共用的底层。
- **DAP**（`apps/nodio-dap/main.cpp`）：capabilities 增
  `supportsSetVariable`；`variables` 请求 ref>100 时转发 `varsRef`
  （等回包最长 2 秒），ref=100 仍走停点快照；新增 `setVariable` 请求
  → `setVar` 命令（应答带写入后的新值）。`VarsBody` 重写：字符串感知
  配平花括号逐对象解析（值串里可能有 `{}`/引号），name/type/value 解码
  后重新 Quote、ref 透传为 `variablesReference`；stopped 事件的 vars
  数组按字符串感知配平 `]` 提取（值串里可能有 `]`，rfind 会吃错）。
- **IDE**（`app/dapClient.nodio`）：`getVars` 条目升级
  `{name;value;type;ref}`；新增 `getVarChildren(ref)`（逐层展开，子表
  同样带 ref）与 `setVariable(ref,name,value)`（返回
  `{ok,value,type}|{ok,error}`）。UI 消费（变量树/监视窗）留给下一轮。
- **回归锁**：新增 `tests/dap_vars_check.py` 34 项——结构化分型、
  ref 分配（>100 且两表不同）、表/数组子级展开、setVariable 三路
  （帧变量/表成员/数组元素）求值复读、不存在变量必须失败、失效引用
  不崩、`supportsSetVariable` 能力声明；`ide_debug_check` 合计 88 项。
  ⚠ 停点快照是**语句执行前**的状态（与条件断点 x==3 同一语义）。
- 验证：VM 98/0/1、IDE 冒烟 SMOKE OK、ide_debug 88 项全绿。
- ❌ 剩余（调试器）：独立监视窗（消费本轮协议）。

---

## 1.67 第五十九轮续：调试器之四——独立监视窗 + 变量树

- **新面板模块**（`ide/nodio/app/watch.nodio`，仿 app/ai.nodio 的组合
  面板套路：dock 只认一个 ctrl → 可见性代理，layout() 自己摆）：
  - 「监视」页：表达式列表（表达式/值/类型三列）+ 底部输入行。回车 =
    添加/更新同名监视（求值同步阻塞 → 推迟到消息循环，与「立即」窗口
    同招）；右键菜单 删除选中/清空/立即刷新；双击行 = 立即重估。
    求值走 DAP evaluate（main 侧 `setEvaluator` 注入，能看到局部变量、
    支持 `x+1`/`t.a` 表达式）——替换旧版「变量名 = 值」名字匹配输出。
    未暂停时显示「（未暂停）」占位，表达式列表不丢。
  - 「变量」页：当前帧变量树（nodio.ui.tree）。顶层来自停点快照
    （`getVars`，main 侧 `setProvider` 注入）；表/数组节点按需经
    varsRef（`setChildren` → `getVarChildren`）懒展开。⚠ 引擎引用每
    停点清空重建 → 每次停点整树重建，会话结束整树清空。
- **树控件懒加载扩展**（`lib/nodio/ui/tree.nodio`）：节点新增
  `hasChildren` 标记（子级未填但箭头先画）；新增 `onToggle(node,
  expanding)` 钩子（用户点击路径触发，返回 false 取消翻转；程序化
  expand/collapse 不经过）；懒节点必须初始 `expanded=false`（addNode
  默认 true，否则第一次点箭头是「收起」）。
- **入口**：视图菜单「监视窗口 Ctrl+Shift+W / 变量窗口」；Ctrl+Shift+W
  热键；调试菜单的 添加监视表达式/清空监视列表 改挂面板
  （showPrompt → watchPanel.add）。pollDebug 停点自动刷新两面板
  （沿用既有 refreshWatchList 裸名钩子）。
- **回归锁**（`ide_smoke.ps1` 增 6 断言）：页签（监视/变量）+ 菜单项
  + 面板逻辑链——假求值器 add→求值→计数（WATCH-ADD）、假数据源
  树重建（VARS-TREE）、懒展开经 onToggle 真正拉子级（VARS-LAZY）。
- 验证：VM 98/0/1、IDE 冒烟 SMOKE OK（含新 6 断言）、ide_debug 88 项
  全绿。
- ❌ 剩余（调试器五件套已齐）；⚠ setVariable 的 UI 消费（右键「修改
  值...」）随设计器轮次或变量面板增强另行挂账。
