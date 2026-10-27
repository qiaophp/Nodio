# VM 全面审计总报告（2026-10）

- **审计基线**：3dd464e（worktree `Temp\nr26\wt-audit`）；主树同日已推进至
  2a09515（IDE 线 §1.54，engine 4 行 WIP 不属本审计范围）。
- **修复方案底稿**：`.audit-2026-10-fixdesign.md`（行级修法 + 验证门禁）。
- **状态**：审计完成，**修复未动手**——分轮执行等用户授权（§7）。

## 0. 执行摘要

定性沿用性能线守则：不认「极差」总括，认具体缺陷清单。**VM 核心本体
（词法/编译/字节码/执行/GC 环回收）干净**：97 项 ASAN 全量两轮一致
84 PASS / 12 FAIL / 1 SKIP，12 项失败定案后仅 **2 个真实缺陷根**
（7 项 UAF 全部归 AUDIT-01/02）+ 5 项已证伪的环境伪红。

问题集中在**「原生资源所有权与调用边界」层**：

| 级别 | 编号 | 一句话 | 证据（3dd464e 基准） |
|---|---|---|---|
| P0 | AUDIT-01 | 回调 CbCtx 析构绕过 g_cbs 注销 → GC 踩尸（t91） | common.h:551 / gc.cpp:499 / runtime.cpp:3209 |
| P0 | AUDIT-02 | FormState 双重所有权 → 模态循环读尸（t34-38/t46 六项） | gdi.cpp:381-382 / builtins.cpp:9710 |
| P0 | AUDIT-03 | `::DrawText` 签名缺参 → 非确定性 AV（探针两跑一好一崩） | sysapi.cpp:80 |
| P1 | AUDIT-04 | `::LoadCursor/LoadIcon` 缺参 → 100% 错结果（三跑全 0） | sysapi.cpp:86-88 |
| P1 | AUDIT-05 | ffi 动态返回白名单缺指针族 → 截断句柄进 ntdll AV | ffi.cpp:1964-1975 |
| P1 | AUDIT-06 | 剪贴板 write 的 GlobalLock 未判空 → NULL 必崩 | builtins.cpp:5710 |
| P1 | AUDIT-09 | 动态 FFI 裸函数包装永不释放 → 闭包+4KB 页永久钉死 | ffi.cpp:2151-2158 |
| P1 | AUDIT-10 | raw.concat/append 第三参 len 未钳制 → 巨值 memcpy | builtins.cpp:7573-7652 |
| P2 | AUDIT-08 | sysapi 签名标签与真实契约错位（文档陷阱，位置恰好对齐） | sysapi.cpp:56/104/203 |
| 关闭 | AUDIT-07 | /analyze C6001 候选 → 定案误报（bufLen 恒 0，循环不执行） | builtins.cpp:6931/6364 |

全部发现带行级修法与验证门禁，见底稿。安全剩余面（71 处 memcpy 逐条
分级、编码边界、载荷解析）**未再发现新的 P0/P1 内存缺陷**（§3）。

## 1. 方法论与可复现性

1. **ASAN 全量过筛（主力）**：全新 build 目录构建（CMake 覆盖默认 flags
   的教训已记档：CMake 默认 `/DWIN32 /D_WINDOWS /EHsc`，覆盖需显式带上
   `/EHsc`，否则出 C4530 假发现）。97 用例两轮完全一致。
   已知 ASAN 伪象两个，均以变体实验隔离：①帧膨胀 × 16MB 栈 → 深递归
   伪 stack-overflow（t70/t94/t95/t97/t98），64MB 栈变体终审全绿；
   ②ASAN 的 RtlAllocateHeap 拦截器不认 OS 进程默认堆句柄 → 截断类
   探针（AUDIT-05）必须用非 ASAN 正规构建验数字。
2. **定向探针双环境交叉**：ASAN 验崩溃形态、非 ASAN 验数字/确定性
   （AUDIT-03 两跑一好一崩 → 非确定性 AV 坐实；AUDIT-04 三跑全错 →
   100% 错结果坐实；AUDIT-05 双环境：ASAN 下崩溃无效伪象，非 ASAN
   实测 GetProcessHeap 返回截断值）。探针可能翻案：GetClassInfoEx /
   RedrawWindow / GradientFill 经探针 + 重读降级为 P2 文档陷阱；
   16 槽字符串缓冲重入探针阴性降级静态风险。
3. **/analyze 构建与 144 条告警全裁决**：真问题 1（AUDIT-06）、候选 1
   （AUDIT-07，深读定案误报）、其余 142 条全部归因（C6239×32 源自
   common.h:462 有意架构判断、C6297×6 移位已掩码、C6001 系判空齐全、
   C6246×18 / C28159×3 记现代化注记、DoImport 70KB 栈记风险注记）。
4. **危险 C 函数普查与逐条深读**：strcpy/strcat/sprintf/gets 在 engine
   **零使用**；71 处 memcpy/memmove 逐条分级（§3.1），脚本可控长度
   现场深读 10 处。
5. **自我纠错记档**（假发现必须自我撤回）：①C4530「/EHsc 缺失」系
   自己覆盖 CMAKE_CXX_FLAGS 所致，CMakeCache 证伪撤回；②dumpbin
   /HEADERS 输出为十六进制（0x1000000=16MB，曾误读为 1MB）；③P3
   探针在 ASAN 下崩溃是 ASAN 堆拦截器伪象，非引擎缺陷。

## 2. 真实缺陷根因模式（重构方向）

12 项 ASAN 失败 + 5 项探针实证，收敛为三个根因模式——重构轮按模式
治理而非逐点打补丁：

1. **原生注册表持裸指针，脚本侧引用计数直达析构**（AUDIT-01）。
   治理方向：跨 GC 边界的注册表一律 weak_ptr 化（fixdesign 已给
   4 触点改法）。同类结构排查：g_forms（AUDIT-02 同模式）、g_cbs、
   ComRawSink::cbs。
2. **同一状态两条生命周期线并存**（AUDIT-02：g_forms 的 WM_NCDESTROY
   delete vs doModal 消息循环）。治理方向：唯一所有权归 shared_ptr，
   事件回调只做「摘注册」，不做 delete。
3. **签名表/白名单与真实 Win32 契约脱节**（AUDIT-03/04/05/08）。
   治理方向：签名表补 dumpbin/MSDN 核对流程；返回值政策固化为
   「HANDLE 型可 ARInt / 指针型必须白名单」（fixdesign AUDIT-05）。

## 3. 安全剩余面定案（71 memcpy 深读轮）

### 3.1 危险 C 函数 71 处分级（最终）

strcpy/strcat/sprintf/gets 在 engine 零使用；71 处全部 memcpy/memmove：

| 类别 | 数量 | 定案 |
|---|---|---|
| 定长类型双关（float/double/int64 装箱等） | ~30 | 安全 |
| 显式 min() 有界拷贝 | 8 | 安全 |
| ffi 代码发射（跳板/桩生成） | 9 | 构造受限，留池尺寸断言注记 |
| memdll PE 装载 | 2 | 有界（`off + SizeOfRawData > buf.size()` 检查） |
| com SafeArray/BSTR 区 | 若干 | SEH 护卫 |
| 脚本可控长度现场（深读 10 处） | 10 | 3 处 by-design raw 足枪（§3.4）+ 1 处 AUDIT-10 + 6 处有界 |

深读明细（有界确认）：内存流 read（builtins.cpp:3912 got=min(want,avail)）、
raw.buffer 注册缓冲分支（7195 min 有界）、TryReadBStr（com.cpp:3744，
len 限 0x3FFF + SEH）、raw.realloc 初始数据两分支（7720/7731 min 有界）、
ffi 结构数组打包（ffi.cpp:1138 `min(subTotal, elemSz)` 钳制）。

### 3.2 编码边界

engine 内 WideCharToMultiByte / MultiByteToWideChar 约 60 处，全部
「查询尺寸→分配→转换」两段式，抽读守卫齐全（失败返回 0/负均被
`w>0`/`n<=0` 拦截）。两处无守卫写法（builtins.cpp:3047/3145）在失败
路径实际良性，留注记。无新缺陷。

### 3.3 载荷解析

import.cpp 本体零 memcpy；aardio_chunk 反序列化统一走 ChunkReader
（aardio_chunk.cpp:350：`Need(n)` 界检 + bad 标志逐字节读取），
全文件唯一 memcpy（:562）源为固定 8 字节栈数组。memdll PE 装载边界
检查已确认。字节码/块格式解析面**安全**。

### 3.4 raw 家族 by-design 足枪（文档注记，非缺陷）

raw 语义允许脚本自担风险，以下行为按 by-design 记录（修 AUDIT-10 时
顺带在 raw 库注释块集中声明）：

- `raw.write/concat/copy` 对整数地址或未注册指针目标 = 任意写
  （builtins.cpp:7629/7199）；
- `raw.tostring` 类 `(int)len` 截断会把巨值 len 变成 NUL 终止语义读
  （builtins.cpp:5962）；
- 对未注册指针的 `GetBufferSize` 失败分支按调用方给的 n 整段拷
  （builtins.cpp:7199）。

### 3.5 跳板页驻留（设计权衡定案）

每回调独占 4KB 页（kTrampSlot=4096，ffi.cpp:250）是 W^X 页粒度的
必然（同页多 stub 会迫使整页保持可写+可执行，ffi.cpp:244-247 注释
明示）；释放时中性化桩（`xor eax,eax; ret`）而不 VirtualFree，是为
保 native 侧迟调用安全返回 0（官方契约警告迟调用）。代价：每释放
一个回调永久驻留 4KB 提交内存，g_trampPages 只增不读。**接受现状**，
内存台账记录；AUDIT-09 修的是其上 CbCtx/闭包的永久钉死。

## 4. 质量审计（巨型 TU 拆分预案 + 注释失真 + 死代码）

### 4.1 巨型 TU（行数普查 @3dd464e，engine 共 37,706 行）

builtins.cpp 11,170 / com.cpp 5,314 / runtime.cpp 4,392 / parser.h 2,906。

**拆分原则**：零行为变化，物理分文件 + 内部头；一刀一轮、全门禁、
可单独 revert；匿名命名空间静态状态先盘点再移动。

- **builtins.cpp**（最大收益）：按域切 `b_string_encoding`（编码/
  base64/split 系）、`b_raw_memory`（DynAlloc/raw 家族/struct 序列化）、
  `b_io_process`（io/process/环境）、`b_win_glue`（win.form/菜单/
  剪贴板，与 gdi 交界）、共享助手进 `builtins_internal.h`（WToU8 等）。
- **com.cpp**：`com_core`（初始化/对象包装）、`com_safearray_bstr`、
  `com_dispatch_events`（ComRawSink 与 ffi 交界单列）。
- **runtime.cpp**：`rt_value_ops` / `rt_exec` / `rt_gc_glue`。
- **parser.h**：仅拆纯数据表到 `parser_tables.h`，保守处理（内联
  实现为主，激进拆分收益低风险高）。

收益：编译并行度、审计/评审可定位性、/analyze 单 TU 噪声下降
（import.cpp DoImport 单函数 70KB 栈帧也在 builtins/com 级 TU 中）。
风险与缓解：包含环（先零改动移动、后清死代码）、静态初始化顺序
（盘点匿名空间状态）。门禁：97 项 VM 回归 + 七套 CLI + /analyze
零新增告警 + 构建时长对比。

### 4.2 注释失真清单（质量轮修正）

- sysapi.cpp:104 GetClassInfoEx / :56 RedrawWindow / :203 GradientFill
  ——签名标签与真实契约错位（位置恰好对齐，探针实证，纯文档陷阱）；
- ffi.cpp 裸函数包装注释声称对齐官方契约，但生命周期策略违反之
  （AUDIT-09，修函数本身时同步改注释）；
- builtins.cpp:7572 raw.concat 注释声称官方契约 `(dst,src,len)`——
  官方侧行为未探测（官方 exe 为 GUI 进程，受 IDE 线窗口纪律限制），
  注释补「官方行为未验证」标注，诚实优先；
- builtins.cpp:6363-6364 ApiArgRef bNoFree/bufLen——bufLen 设计意图
  从未实现（见 4.3）。

### 4.3 死代码

- `ApiArgRef.bufLen`（builtins.cpp:6364）：全文件仅声明与读取，
  从未赋值——AUDIT-07 定案的副产品；
- `g_trampPages`（ffi.cpp:251）：只 push 不读（纯登记簿，见 §3.5，
  保留但加注释说明用途）；
- 全量死代码扫描未执行（本轮未引入工具），质量轮补做并如实报数。

## 5. 性能审计

**纪律**：性能测量受安静窗口约束（IDE 线共享机器，python mock
server/probe 活跃时窗口污染），本轮**不重跑测量**，以下为静态热点
观察 + 既有性能线收官数据汇总，未测项如实标注。

静态观察（供质量/重构轮参考，均非本轮缺陷）：

- 编码两段式转换（§3.2）每趟转换 2 次系统调用；热点路径
  （string.split/tostring/ffi 参数打包）存在 WToU8/BStr 往返——
  性能线已有量化基线，重构轮不动语义的前提下可做单趟缓存；
- GC：GcMarkNative 遍历 g_cbs 为 O(回调数)/轮；AUDIT-01 的 weak_ptr
  化增加每回调每轮一次原子增减（fixdesign 已评估：回调量级小，可忽略）；
- AUDIT-02 的 shared_ptr 化不影响消息循环热路径（查找在注册时，
  非每消息）；
- DynAlloc 增长策略 `max(2x, len+1024)` 合理；增长条件 `>=` 仅触发
  略早，无性能问题；
- 跳板页 4KB/回调：VirtualAlloc 每回调一次系统调用，回调创建非热路径；
  内存驻留见 §3.5；
- DoImport 70KB 栈帧（/analyze）：有 import 缓存不深嵌，风险注记保留。

未测项（等安静窗口）：池化修复合入后（≥2a09515）的启动时间/执行
吞吐复测——性能线收官数据仍为最新基线；ide_* 12 项 GUI 回归同步挂起。

## 6. 台账挂接与承诺补录（四条）

1. **Temp 诊断产物存活台账**：现存 7 个旧 worktree + 11 个旧构建目录
   + 本轮 wt-audit（含 build-asan2/3/4、build-analyze 4 个构建目录）
   + audit-probes（p1~p4 探针）。多轮存活未清理（用户已知晓），
   清理决策仍挂起，等用户指令（deletion 走 mavis-trash）。
2. **巨型 TU**：普查与拆分预案见 §4.1（本轮补录完成）。
3. **发布体积欠账**：发布 exe 体积优化（LTCG/strip 等）从未排期，
   本轮未评估（/analyze 构建 2,445,312B 仅为诊断构建尺寸，非发布
   配置，不可引用）。挂重构轮后的工程化轮。
4. **UPX 否决（记录在案）**：官方 aardio 使用 UPX 壳
   （D:\aardio\lib\ide\upx\.upx\upx.exe 实证）；Nodio 不跟进——
   ①壳解压与 ASAN/调试器/采样工具链不兼容；②安全软件误报率上升；
   ③体积收益对开发工具场景不划算（解压拖慢冷启动）。维持否决。

## 7. 重构分轮提案（等用户授权）

流程沿用 98e5f94 事故后定型的门禁：每轮 = patch + md 底稿 + ASAN
双栈回归（97 用例）+ 普通构建回归 + 七套 CLI + A/B + 用户确认入库；
回滚 = 单轮 git revert。

| 轮次 | 范围 | 预期门禁变化 |
|---|---|---|
| **R1（P0 三项）** | AUDIT-01（g_cbs weak_ptr 化，4 触点）、AUDIT-02（FormState shared_ptr 化，~40 捕获机械迁移）、AUDIT-03（DrawText 补参） | t91 与 t34-38/t46 七项 UAF 转绿；GC A/B（call5m/callv5m + collectgarbage 高发）无回退；ide_* 12 项 GUI 回归（等 IDE 线窗口） |
| **R2（P1 五项）** | AUDIT-04/05/06/09/10 | 探针 p1~p5 全绿（p5 新建，验 AUDIT-10）；winclip 回归；1 万次动态裸函数调用常驻数恒定 |
| **R3（P2/质量）** | AUDIT-08 签名标签、注释失真修正、死代码清理（bufLen 等）、C6246/C28159 注记落地；TU 拆分如获批准另立小轮序列（每轮一刀） | 行为零变化：97/97 + 七套 CLI + /analyze 零新增 |

顺序理由：P0 先行消除仅有的两个真实 UAF 根与唯一非确定性崩溃；
R2 全部为崩溃/错结果面且相互独立；质量轮与行为无关殿后。

## 8. 证据索引

- worktree：`Temp\nr26\wt-audit`（基线 3dd464e；含 64MB 栈 CMakeLists
  临时改动，仅 ASAN 变体实验用）
- 构建目录：build-asan2（ASAN+16MB，主力）/ build-asan4（ASAN+64MB，
  伪红终审）/ build-analyze（/analyze，144 条日志 analyze.log）
- 回归日志：sweep2.log（97 用例 84/12/1，两轮一致）
- 探针：`Temp\nr26\audit-probes\p1_drawtext / p2_loadcursor /
  p3_heapalloc / p4_reentry`（p1/p3 双环境、p2 ×3、p4 阴性）
- 修复底稿：`.audit-2026-10-fixdesign.md`（10 项发现 + 阴性结论 +
  行级修法）
- 自我纠错记档：C4530 假发现撤回 / dumpbin 十六进制 / ASAN 堆拦截
  伪象（§1.5）
- 行号基准：全部以 3dd464e 为准；主树 2a09515 起入库需 patch --check
  核对漂移（common.h 已知漂移，builtins.cpp 暂未漂）。
