---
title: aardio 标准库「全量可用」交付说明
---

# aardio 标准库「全量可用」交付说明

> **一句话结论**：官方公开库 **969 个模块**，Nodio **x64 `ok=899` / x86 `ok=933`**；
> 非 OK 项里**属于 Nodio 自身缺陷的是 0 项** —— 剩余 70 / 36 项全部落在
> **物理边界 / 官方契约 / 官方库自身缺陷 / 环境限制** 四类。
> 官方唯一样本 `lib/ide/aifix.aardio`（`=aardio` 预编译块）**已被真正执行跑完**。
>
> ⚠️ **口径提醒**：上面这句只覆盖 **import 层面**（模块能不能导进来）。
> **函数 API 层面**原先有 **17 个真缺口**（官方内建库 156 个成员里缺 15 个 +
> 2 个未声明但被官方库调用的引擎原生），其中 **8 个有官方库实际调用点** ——
> 详见 **§8**。
>
> ✅ **第二十三轮已把这 17 个缺口收口到 1 个**（详见 **§10**）：
> 官方内建库 **149 / 150 个成员被覆盖（99.3%）**，剩 1 个
> `com.ImplInterfaceFromTypelib` 需要「按 ITypeInfo 动态构造原始接口 vtable」
> 的设施（官方库零调用点）。
> 另有一处**能力边界**：COM 事件接收器只支持 **dispinterface 型连接点**
> （原始接口连接点同样卡在 vtable 设施上）。

本文是**验收口径**的落点；逐条取证见 `docs/aardio-bytecode.md`（§7 语义、§7.5.10
调用约定定案）与 `docs/capability-gap.md`（§3 现状、§7 归因）。

---

## 1. 验收口径

「可以使用 aardio 的所有标准库」定义为：

> 官方公开库 969 个模块中，**凡不受物理边界 / 官方契约 / 官方库自身缺陷 /
> 环境限制约束的，Nodio 全部能 import 并使用**。

因此「非 OK」本身不是缺陷，**「非 OK 且归因到 Nodio」才是**。本文把两者分开记账。

## 2. 实测数字

| 架构 | 全部 OK | 非 OK | 相对第十九轮基线 | 日志 |
|---|---:|---:|---|---|
| x64 | **899** | 70 | `+2` | `.module-sweep-x64.log` |
| x86 | **933** | 36 | `+3` | `.module-sweep-x86.log` |

第十九轮基线：x64 `ok=897` / 72，x86 `ok=930` / 39。
**逐模块对比：新增失败集为空**（x64 / x86 皆然），增量只有：

* `ide.aifix`、`autos.tools.handlers` —— 官方 `=aardio` 预编译块，
  两模块指向**同一个** `lib/ide/aifix.aardio`，一次修复同时解掉两个；
* `winex.desktop`（仅 x86）—— 上一轮 TIMEOUT，本轮通过（偶发）。

**复现**：

```powershell
pwsh -NoProfile -File D:\Nodio\tests\module_sweep.ps1 -Exe D:\Nodio\build-x64\Release\nodio.exe -Log D:\Nodio\.module-sweep-x64.log
pwsh -NoProfile -File D:\Nodio\tests\module_sweep.ps1 -Exe D:\Nodio\build-x86\Release\nodio.exe -Log D:\Nodio\.module-sweep-x86.log
```

> sweep 工具必须用 `build-x64\Release\nodio.exe` 或**同目录**改名副本
> （`nodio-sweep.exe`）—— overlay 资源按 `~` 相对路径解析，换目录会让
> `zlib.*` 一批假失败（见 `docs/capability-gap.md` §1.10）。

## 3. 四类交付表

| 归因 | x64 | x86 | 是不是 Nodio 缺陷 | 证据 / 说明 |
|---|---:|---:|---|---|
| **物理边界**：官方只带 32 位 DLL（内嵌内存映像 32 + 文件路径 2） | 34 | 0 | ❌ 物理不可能 | 官方库只提供 32 位 PE 映像，x64 进程装不下；**x86 构建下这 34 项全部通过**。代表：`string.markdown*`、`av.*`、`gdip.feather`、`sevenZip.lzma*`、`web.layout`（HTMLayout.dll）、`com.autoComplete` |
| **官方库自身**：`.api` / DLL 导出符号不是可调用值 | 17 | 17 | ❌ 官方库缺陷 | `py2.*` / `py3.*` 的 CPython 导出（`.PyImport_AddModuleP` 等）、`web.layout.valueObject` / `web.layout.element*` 的 HTMLayout 导出、`win.ui.ctrl.splitter`（`.registerClassEx`）、`win.ui.ctrl.thread`（`metaProperty`） |
| **环境限制**：官方库自带**安装器 / 下载器** | 6 | 6 | ❌ 环境限制 | 见 §4 |
| **官方契约**：必须先导入父命名空间 / 禁止直接导入 | 6 | 6 | ❌ 官方行为 | `sqlite.base`、`sqlite.backup`、`web.form.query`、`web.acme.httpClient`、`web.view.dev`、`winex.accObject.element` |
| **官方库自身**：依赖的官方库缺失 | 3 | 3 | ❌ 官方缺陷 | `crypt.jwt.algorithm.EdDSA`（BouncyCastle）、`process.ruby`、`process.ruby.simpleHttpServer`（`sevenZip\decoder2\httpFile`） |
| **官方契约**：`py3.pip` 只允许开发环境 | 1 | 1 | ❌ 官方行为 | 官方库自检 |
| **官方库自身**：断言失败 | 1 | 1 | ❌ 官方缺陷 | `web.layout.event` |
| **官方契约**：类命名空间必须先导入 | 1 | 1 | ❌ 官方行为 | `web.layout.debug`（要求先 `import web.layout` 创建类命名空间） |
| **清单问题**：伪模块 `win.#DEFINE` | 1 | 1 | ❌ 清单修正 | 官方文件名带 `#`，不能出现在 import 名里 |
| **Nodio 自身缺口** | **0** | **0** | — | **已归零**（第二十一轮修复 `=aardio` 预编译块） |
| **合计** | **70** | **36** | | |

> 逐模块明细见两份 sweep 日志的 `FAIL` / `TIMEOUT` 行。

## 4. 那 6 个 TIMEOUT 是什么

**官方库自带安装器，import 时会触发下载 / 安装 / 交互** —— 官方行为，Nodio 同样如此。
源码级证据（`D:\aardio\lib`）：

| 模块 | 官方源码里的动作 |
|---|---|
| `process.php.7.4` / `process.php.8.3` | `import zlib.httpFile` → `zlib.httpFile.download(url, "正在下载本程序必须组件：PHP 7.4", ...)`，下 `php-7.4.33-nts-Win32-vc15-*.zip`（~25 MB）并解压 |
| `java.jdk.v6ora` / `java.jdk.v8ora` / `java.jre.v8ora` | `import inet.installer` → `inet.installer("JDK 6", "https://javadl.oracle.com/webapps/download/AutoDL?BundleId=49184", "/s")`，下 Oracle JDK/JRE 并静默安装 |
| `fastcgi.iisInstall` | 注册 IIS 的 `*.aardio` 处理程序映射并写默认首页 —— 需要管理员权限 + 改 IIS 配置 |

下载链路走 `zlib.httpFile` → `inet.downBox`（**带下载进度窗口**），无头脚本环境里
这个窗口等不到人操作，sweep 的 10 秒上限自然判 TIMEOUT。

> 这 6 项在 `inet.installer` / `inet.downBox` / `zlib.httpFile` 三个**官方模块本身
> import OK**（见 sweep 日志 `[356/969] inet.installer : OK` 等），
> 即：Nodio 侧依赖链是通的，只是这一步要人参与。

## 5. 怎么用：两套构建各覆盖什么

| 需求 | 用哪个构建 | 原因 |
|---|---|---|
| 常规开发 | **x64** | 官方 969 个模块中 899 个可用 |
| 必须用 32 位 DLL 的模块（`string.markdown*`、`av.*`、`web.layout`、`gdip.feather` 等 34 项） | **x86** | 官方这些模块只带 32 位 PE 映像，x86 构建下**全部通过**（x86 `ok=933`） |

即：**x86 构建 = 官方标准库的完整覆盖**（除 §3 里那三类官方自身/环境项），
x64 构建 = 除 32 位 DLL 那 34 项之外的全覆盖。

## 6. 回归锁

| 脚本 | 覆盖 | 状态 |
|---|---|---|
| `tests/aardio_run_check.ps1` | 记录 VM 语义（含官方预编译块**真正执行**一节：4 项断言） | x64 / x86 双绿 |
| `tests/aardio_chunk_check.ps1` | 容器层：解析 / 反汇编 / 可执行性（第 1 节断言 import **真正成功**且块内函数可执行） | x64 / x86 双绿 |
| `tests/module_sweep.ps1` | 969 个公开模块逐个 import | x64 899 / x86 933 |

## 7. 明确的「不是缺陷」备忘

记录在此，避免以后重复排查：

* **`io.exist(path)` 返回绝对路径（不存在时返回 null），不是布尔值** ——
  这是**官方契约**（官方库大量依赖这个用法：`av/audio/_.aardio` 的
  `var full = ..io.exist(path)`、`zlib` / `golang` / `nodeJs` / `py2` / `py3`
  靠真值即路径来定位随包 DLL）。不要按「官方应该返回 boolean」去改。
* **官方「先导入父命名空间 / 禁止直接导入」报错** —— 官方同样报错，
  Nodio 已删除「import 自动预载父命名空间」的旧行为（`docs/capability-gap.md` §2 E-12）。
* **官方库缺依赖 / `.api` 导出不可调用 / 断言失败** —— 官方自己也不通。
* **`win.#DEFINE`** —— 伪模块名，官方文件名带 `#`。

## 8. 内建 API 层面的缺口（第二十二轮实测）

> ⚠️ **§2 的「缺口归零」只覆盖 import 层面**（969 个模块能不能导进来）。
> 下面这一节量的是**另一个维度**：官方**内建库**（8 个命名空间）的**函数级 API**
> 是否在 Nodio 里存在。结论：**import 层面 0 缺口；函数层面还有 17 个真缺口。**

### 8.1 量法

1. 从官方 `D:\aardio\lib\builtin\*.aardio` 抽出**成员声明**（官方把引擎原生函数
   也用 `name( = 文档` / `name(.(签名) = 文档` 写在内建文件里）→ 官方内建命名空间
   成员清单 **156 个**（已剔除 `struct.*` 的 RECT 方法、`_get`/`_set` 元方法、
   `if`/`elseif` 关键字等正则误匹配）；
2. 运行期枚举 Nodio 的 8 个内建命名空间实际成员 → 逐项比对；
3. 对缺口再回官方库源码统计**实际调用点**（`..com.NewObject` 这类带 `..` 前缀的
   写法要一并计入），得出影响面。

### 8.2 覆盖率

| 命名空间 | 官方成员 | Nodio 覆盖 | 缺 |
|---|---:|---:|---:|
| `com` | 29 | 18 | 11 |
| `io` | 13 | 13 | 0 |
| `math` | 12 | 10 | 2 |
| `raw` | 15 | 15 | 0 |
| `string` | 28 | 27 | 1 |
| `table` | 44 | 43 | 1 |
| `thread` | 15 | 15 | 0 |
| **合计** | **156** | **141** | **15** |

（另有 2 个官方引擎原生、未写进内建文件但官方库代码实际调用：
`io.libpath` / `io.lines` → **真缺口共 17 个**。）

### 8.3 缺口清单与影响面

**有官方库实际调用点（8 个）—— 这些会真的跑挂：**

| 官方 API | 调用点 | 官方调用方 |
|---|---:|---|
| `io.libpath` | 10 | `autos.skills.*`、`autos.tools.handlers`、`py2/py3` 构建 |
| `io.lines` | 6 | `debug`、`fsys.cookies`、`fsys.update.app` |
| `com.Connect` | 4 | `com.wmPlayer`、`com.wmi`、`web.form` |
| `com.GetPointer` | 2 | `dotNet.appDomain`、`web.form` |
| `com.ReleaseConnection` | 2 | `win.ui.ctrl.atlax`、`win.ui.ctrl.metaProperty` |
| `string.splitLiteral` | 2 | `builtin.string`、`fsys.openXml.docx.editor` |
| `com.NewObject` | 1 | `com.activeX` |
| `com.NewControl` | 1 | `com.activeX` |

**零调用点（9 个）—— 只影响用户按官方风格写的代码：**
`com.AddConnection`、`com.DoObjectVerb`、`com.ImplInterfaceFromTypelib`、
`com.IsIUnknown`、`com.IsMember`、`com.SafeArray`、`math.fmod`、`math.lerp`、
`table.setAttr`。

### 8.4 另有一类「命名不兼容」（不是缺，是改了名）

Nodio 的 COM 宿主（`engine/com.cpp`）**函数名与官方不同**，
官方库按官方名调用会报「尝试调用非函数值」：

| 官方名 | Nodio 现有 | 备注 |
|---|---|---|
| `com.NewObject(tab, progId)` | `com.CreateObject(progId)` | **签名也不同**：官方第 1 参是接收结果的表，不能直接加别名 |
| `com.SafeArray` | `com.SafeArrayV` | |
| `com.ReleaseConnection` | `com.Release` | |

### 8.5 复现

```powershell
# 官方成员清单
python <抽取脚本> D:\aardio\lib\builtin\*.aardio
# Nodio 实际成员（运行期枚举 8 个命名空间）
D:\Nodio\build-x64\Release\nodio.exe <枚举脚本>
```

## 9. 变更记录

* **第二十三轮**：§8 量出的 **17 个真缺口收口到 1 个**，3 处 **命名不兼容**
  全部按官方名补齐；顺带修正 `%` 运算符的 floor 语义与 `string.split` 的官方契约。
  详见 **§10**。
* **第二十二轮**：把「全量可用」的验收从 **import 层面**扩到 **函数 API 层面**，
  量出官方内建库 **17 个真缺口**（其中 8 个有官方库实际调用点）与 3 处
  **命名不兼容**（`com.NewObject` / `com.SafeArray` / `com.ReleaseConnection`）。
* **第二十一轮**：`=aardio` 官方预编译块可真正执行（owner 调用约定 + 多返回值 +
  `0x2F` 返回槽与空结果跳过 `0x22`），标准库 Nodio 侧自有缺口**归零**。
  语义取证 `docs/aardio-bytecode.md` §7.5.10。
* **第二十轮**：`0x2F` / `0x30` / `0x31` / `0x3C` 四种动态调用变体落地，
  记录 VM 收口 **61/61**。

## 10. 第二十三轮：官方内建 API 缺口收口

### 10.1 收口结果

| 项 | 第二十二轮 | 第二十三轮 |
|---|---:|---:|
| 官方内建成员覆盖 | 141 / 156 | **149 / 150（99.3%）** |
| 真缺口（函数不存在） | 17 | **1** |
| 命名不兼容（官方名不存在） | 3 | **0** |

> 两个口径的分母不同（156 / 150）：§8 的抽取把若干**嵌套表成员**
> （`thread._serialize`、`thread.release`、`raw._json` …）也计进了成员数，
> 第二十三轮的核对脚本按官方源码逐条核对了这些噪声并剔除。
> **可复现的核对脚本**：`tests/builtin_api_coverage.ps1`
> （官方成员清单固化为 `tests/official_builtin_members.json`；
>   **已知缺口白名单** = `com.ImplInterfaceFromTypelib`，出现新缺口即失败 ——
>   它同时是回归闸门）：
>
> ```powershell
> pwsh -NoProfile -File D:\Nodio\tests\builtin_api_coverage.ps1
> # 官方内建成员 150 个，Nodio 覆盖 149 个（99.3%），缺 1 个
> #   MISSING com.ImplInterfaceFromTypelib
> ```

逐个落地情况：

| 官方 API | 落点 | 备注 |
|---|---|---|
| `string.split` | `engine/builtins.cpp` | **重写为官方 4 种形式**：1 参按 UTF-8 码点逐字符；2 参是**多个单字节分隔符的集合**；`'<整词>'` 形式按整词切；第 3 参 `maxCount` 是结果元素数上限（剩余并入最后一项） |
| `string.splitLiteral` | `engine/builtins.cpp` | 新增，整词分隔 |
| `io.lines(pathOrFile)` | `engine/builtins.cpp` | 原生迭代器（`Value::Native_`），逐行读、去 CR/LF、EOF 自动关闭；支持路径与文件对象 |
| `io.libpath(name)` | `engine/builtins.cpp` + `engine/import.cpp` | 返回**（库文件完整路径, 所在目录）**，找不到返回 null；**复用 import 的解析**（`Interp::ResolveModulePath`），不另造一套查找规则 |
| `math.fmod(x,y)` | `engine/builtins.cpp` | C `fmod`，余数符号随**被除数 x** |
| `math.lerp(from,to,t)` | `engine/builtins.cpp` | `from+(to-from)*t`，`t` 不夹取 |
| `table.setAttr(obj,key,value)` | `engine/builtins.cpp` | 对象含该键则直改；否则走 `_meta._set` 元方法（`ownerAttr=true`）；数字下标命中数组部分 |
| `com.NewObject(tab,progId[,tlb])` | `engine/com.cpp` | 官方**三值返回** `(appObject, event, e)`；失败时 `e` 是错误信息 |
| `com.NewControl(tab,progId)` | `engine/com.cpp` | 同上三值形状 |
| `com.SafeArray(vt,...)` | `engine/com.cpp` | `{_safearray=元素表, _type=vt}`，与官方 `com.toSafeArray` 同形态 |
| `com.IsIUnknown(v)` / `com.IsMember(obj,name)` / `com.GetPointer(obj[,guid])` | `engine/com.cpp` | `IsMember` 走 ITypeInfo 枚举 func/var desc，名称不区分大小写 |
| `com.DoObjectVerb(obj,verb)` | `engine/com.cpp` | `IOleObject::DoVerb` |
| `com.Connect` / `com.AddConnection` / `com.ReleaseConnection` | `engine/com.cpp` | COM 事件接收器，见 §10.2 |

### 10.2 COM 事件接收器（官方 `builtin/com.aardio` 的硬依赖）

官方 `builtin/com.aardio` 里的 `ConnectWeak` / `CreateEmbedEx` 都是 **aardio 源码**，
但它们都调用**引擎原生**的 `Connect` —— 所以 `Connect` 必须由内核提供，
否则官方这批 aardio 源码在 Nodio 里会直接「尝试调用非函数值」。

实现（`engine/com.cpp`）：

* `com.Connect(comObject, 事件监听表)` → `(sink, cookie)`；
  `com.AddConnection(comObject, 事件接收器)` → `cookie`；
  `com.ReleaseConnection(comObject[, sink, cookie])`。
* 连接点通过 `IConnectionPointContainer` 取得，**只取 dispinterface 型**
  （`IID_IDispatch`，或枚举连接点后取源接口是 `TKIND_DISPATCH` 的那个）；
  源接口的类型信息从**类型库**里反查（连接点只给 IID）。
* 事件接收器是一个 `IDispatch` 实现：COM 服务端用源接口的 DISPID 回调，
  引擎用源接口类型信息把 DISPID 还原成**事件名**，再到 aardio 事件表里取同名成员
  调用（官方 `ConnectWeak` 的接收表正是用 `_meta._get` 转发的）。
* 连接点与 cookie 登记在 COM 对象包装表的隐藏成员上，供 `com.ReleaseConnection` 解绑。

**实测**（`tests/builtin_api_check.ps1` 第 5 段）：

* `MSXML2.DOMDocument.6.0`、`Shell.Explorer` —— 连接成功（`cookie>0`），
  `sink.onreadystatechange(1,2)` 走通 `GetIDsOfNames → Invoke → DISPID → 事件名 →
  aardio 处理函数` 全链路，事件**触发 1 次**；
* 官方 `com.ConnectWeak`（aardio 源码，内部调用引擎原生的 `Connect`）同样可用；
* `com.ReleaseConnection` 解绑计数正确，重复调用静默返回 0；
* 没有连接点的对象（`Scripting.FileSystemObject`）→ **明确报错**，不静默失败。

### 10.3 顺带修正的契约偏差

* **`%` 运算符**：官方是**向下取整（floor）**语义，余数符号跟**除数 y**
  （`docs/language-reference/operator/arithmetic.md:46`）。
  Nodio 原先用 C `fmod`（符号跟被除数）—— `engine/runtime.cpp` 3 处 +
  `engine/aardio_vm.cpp` 0x1C 1 处改为 `AardioMod()`。
  实测：`-7%2=1`、`7%-2=-1`、`-7%-2=-1`、`-7.5%2=0.5`。
  注意官方 `math.fmod` 是 **C fmod**（符号跟被除数），两者语义**不同**，都已落地。
* **`com.NewObject` 失败路径的错误信息丢失**：多返回值的 `mrets` 里**首值之外的
  附加值**才进 `mrets`，失败分支把首值重复塞进 `mrets` 导致 `e` 取到 null ——
  已修（`engine/com.cpp`）。
* **Nodio 自有代码跟着官方契约走**：`ide/nodio/main.nodio` 的 3 处
  `string.split(line, "= function")` 之类**多字节字面量**必须改用 `<>` 形式
  （官方 2 参是单字节集合语义）—— 本轮改了，但该文件属并发会话，**未提交**。

### 10.4 仍未闭合的两项（诚实记账）

| 项 | 状态 | 原因 |
|---|---|---|
| `com.ImplInterfaceFromTypelib(implTable, typelibPath, interfaceName, coclassName)` | **未实现** | 需要「按 ITypeInfo **动态构造原始接口 vtable**」的设施（官方 `com.interface` 的机制）。**官方库零调用点**，只影响用户按官方风格写的代码 |
| 原始（非 IDispatch）事件连接点 | **不支持** | 同上：`com.wmi` 的 `SWbemSink`（`IWbemObjectSink`）、`WMPlayer.OCX` 的默认源接口都是**原始接口**，需要按 ITypeInfo 生成 vtable。实测 `WMPlayer.OCX` 返回 `hr=0x80040202`（`EVENT_E_ALL_SUBSCRIBERS_FAILED`）——连接点找到了，但接收器不被接受 |

> **下一轮的方向**：原始接口 vtable 设施一旦落地，这两项一起闭合。
> 在此之前，`com.Connect` 对 **dispinterface 型连接点**（HTML / ActiveX / OCX 默认
> 事件接口）已经可用。

### 10.5 回归锁

本轮全部断言固化在 **`tests/builtin_api_check.ps1`**（5 段）：

| 段 | 覆盖 |
|---|---|
| 1 | `string.split` / `string.splitLiteral` 官方 4 种形式（20 项断言） |
| 2 | `io.lines` / `io.libpath`（10 项） |
| 3 | `math.fmod` / `math.lerp` / `table.setAttr` / `%` floor 语义（15 项） |
| 4 | `com.NewObject` / `NewControl` / `SafeArray` / `IsIUnknown` / `IsMember` / `GetPointer` + 多返回 null 首值护栏（14 项） |
| 5 | `com.Connect` / `ConnectWeak` / `ReleaseConnection` 事件链路（9 项） |

```powershell
pwsh -NoProfile -File D:\Nodio\tests\builtin_api_check.ps1
pwsh -NoProfile -File D:\Nodio\tests\builtin_api_check.ps1 -Exe D:\Nodio\build-x86\Release\nodio.exe
```

x64 / x86 **两套构建均全绿**；既有 7 套回归（`aardio_run_check` /
`aardio_chunk_check` / `run_vm_tests` 90 passed / `run_stdlib_tests` /
`import_contract_check` / `sysapi_check` / `s2_error_location_check`）全绿。

## 11. 第二十四轮（前半）：杀软误报的代码层根治

### 11.1 现象

2026-10-21 起，本机火绒把 **32 位** `nodio.exe` 直接隔离，报 `HEUR:Backdoor/Farfli.e`；
同一份源码编出的 **64 位** `nodio.exe` 从未被拦。x86 产物因此反复消失，
`tests\run_editor_tests.ps1` 在加载 exe 时被中断，x86 回归链断在同一点。

火绒自己的日志（`C:\ProgramData\Huorong\Sysdiag\log.db` → `HrLogV3_60`）
给出了 5 条 `filemon` 记录，路径全部是 `D:\Nodio\build-x86\Release\nodio.exe`，
父进程是跑 x86 编辑器回归的 `pwsh.exe`。5 条记录的 `hashsig` 完全相同
（`0xbc090e8b,0x96b16b66b2186c9a`）而 SHA1 各不相同 —— 说明命中的是
**规则特征集**，不是某个已知样本的哈希，所以改掉特征就能改判。

另有一条独立告警：Defender 的 `VirTool:Win32/DefenderTamperingRestore`
（12:36:40），`Path` 是注册表项
`HKLM\...\Real-Time Protection\DisableBehaviorMonitoring`，检测源为系统、用户为
`NT AUTHORITY\SYSTEM`、进程未知。它针对的是「有人改了 Defender 实时保护开关」，
与 nodio 无关，写入者从日志查不到（**未知**）。

### 11.2 二分定位

用「独立 Win32 构建目录 + 跑一次看隔离区文件数是否增加」建立了可复现的判定回路，
逐项开关嫌疑能力（每次只动一处，重新编译 x86，跑一次，读火绒隔离区）：

| 配置 | 判定 |
|---|---|
| trampoline 关 + memdll 关 + 异常过滤器 关 | CLEAN |
| trampoline 关 + memdll 开 + 异常过滤器 关 | CLEAN |
| trampoline **开** + memdll 开 + 异常过滤器 关（RWX 内存） | **QUARANTINED** |
| trampoline **开** + memdll 开 + 异常过滤器 关（改 RW） | CLEAN |
| trampoline **开** + memdll 开 + 异常过滤器 **开**（RW→RX 封页） | **QUARANTINED** |

结论：**命中需要两个特征同时存在**

1. **可写且可执行的内存**（FFI 跳板池原本用 `PAGE_EXECUTE_READWRITE` 申请，
   运行时在里面生成原生回调机器码）；
2. **32 位未处理异常过滤器转储原始寄存器**
   （`EIP/EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI`）—— 这正是 SEH 反调试/反分析的教科书形态。

单改其中一项都不够：只有把两者一起去掉才干净。x64 一直不被拦，
是因为 64 位没有 `ep->ContextRecord` 的寄存器转储分支。

顺带排除的方向（都做了实测，不是推测）：

- 最小复现程序（`VirtualAlloc` RWX + 写 x86 跳板模板 + 调用，x86/x64 各 4 个变体）
  **全部 CLEAN** —— 跳板模板本身不是触发条件；
- PE 结构对比：两架构的节表、导入表、熵分布、DEP/ASLR 标志均无实质差异；
- 全仓字符串差分：x86 独有字符串 1932 条，逐条筛过，**无 C2/URL/下载器/注入器痕迹**；
- 全仓搜索：无 PEB 访问、无 `IsDebuggerPresent`、无 `rdtsc`/`cpuid` 等反调试特征。

### 11.3 修复（能力不减，只改形态）

- `engine/ffi.cpp`：跳板页改为 **先可写申请 → 写入桩代码 → 降权为
  `PAGE_EXECUTE_READ`**。页面从不以「可写 + 可执行」形态存在。
  代价是每个回调独占一页（保护属性是最小页粒度，共享页无法再写），
  即回到池化改造之前的内存占用水平。
- `engine/ffi.cpp`：`EmitX64CallStub` 同样改为 RW → RX。
- `engine/memdll.cpp`：内存 PE 映射改为 **先可写申请 → 打完重定位和 IAT → 按节降权**
  （代码节 RX、数据节 RW、其余 R），不再整块 RWX。
- `engine/entry.cpp`：32 位崩溃过滤器不再转储整组原始寄存器，
  只保留 `EIP/ESP/EBP`；异常码、出错地址、模块内偏移、访问违例信息都还在。

### 11.4 结果与残留

- x64 / x86 两套构建重编后，**火绒判定均为 CLEAN**（隔离区无新增记录）；
- 9 套既有回归（`aardio_run_check` / `aardio_chunk_check` / `run_vm_tests` 90 passed /
  `run_stdlib_tests` / `import_contract_check` / `sysapi_check` /
  `s2_error_location_check` / `builtin_api_check` / `run_publish_tests`）
  在两套构建上**全绿**，`run_publish_tests` 覆盖了内存 DLL 加载路径；
- 残留：`memdll` 的「可执行且可写」节（`IMAGE_SCN_MEM_EXECUTE|MEM_WRITE`）
  仍需 `PAGE_EXECUTE_READWRITE`，这是该节自身可自修改所必需，
  属已知取舍，不影响当前判定。

### 11.5 回归锁

新增 `tests/av_heuristic_check.ps1`：扫描 `engine/*.{cpp,h}`，断言

1. 任何 `VirtualAlloc` 调用都**不**申请 `PAGE_EXECUTE_READWRITE`；
2. `engine/entry.cpp` 不再出现整组 x86 寄存器转储格式串。

脚本对两处都做了反向验证（改回旧写法即报 FAIL）。这是**源码级**断言，
不依赖任何杀软，因此可以在任何机器上作为 CI 闸门。
## 12. 第二十四轮（后半）：comTypeInfo 类型信息契约补齐

### 12.1 缺口

官方 `comTypeInfo`（`com.GetTypeInfo` / `com.LoadTypeLibrary(...).GetTypeInfo(i)` 的返回对象）
在 Nodio 里只有 `GetTypeLib` / `GetDocumentation` / `DumpTypeInfo` 三个成员，
**缺 `GetTypeAttr()` / `GetFuncDesc(i)`**。这两个成员是官方标准库的直接依赖：

| 官方库文件 | 用到的成员 |
|---|---|
| `lib/com/typeAttrs.aardio` | `info.GetTypeAttr()` → `attr.typekind` / `attr.Funcs` / `attr.Vars` / `attr.GUID` / `attr.flags.*` |
| `lib/com/tlbDoc.aardio` | `typeinfo.GetTypeAttr()` → `typeattr.Funcs`；`typeinfo.GetFuncDesc(i)` → `func.name` / `func.invkind` / `func.memid` / `func.type` / `func.vt` / `func.parameters` / `func.ParamsOpt` / `func.description` |
| `lib/com/interface/_.aardio` | `info.GetTypeAttr()` → `attr.typekind` / `attr.Funcs` / `attr.GUID`；`info.GetFuncDesc(i)` → `f.name` |

补齐之前，这三个官方文件都是**死代码**（调用即报成员不存在）。

### 12.2 官方契约

契约形状取自 `D:\aardio\lib\builtin\com.aardio` 的 intellisense 段
（`!comTypeInfo` / `!comTypeAttr` / `!comTypeFunc` / `!comTypeParameter` / `!comTypeVar`）：

- `comTypeAttr`：`Funcs` / `Vars` / `typekind` / `ImplTypes` / `flags.{cancreate,dispatchable,control,appobject,hidden,oleautomation}` / `GUID`
- `comTypeFunc`：`dispatchable` / `restricted` / `memid` / `invkind`（`function` / `_get` / `_set` / `_setByRef`）/ `Params` / `ParamsOpt` / `name` / `description` / `helpfile` / `helpcontext` / `parameters` / `type` / `vt`
- `comTypeParameter`：`default` / `in` / `opt` / `out` / `type` / `vt`
- `comTypeVar`：`value` / `name`
- 索引一律**从 1 开始**（官方 intellisense 明写「注意超始索引为1」）

类型名字符串沿用官方 `tlbDoc` 已经在用的形状：`pointer/*VARIANT*/`（VARIANT*）、
`pointer/*VARIANT*/[]`（VARIANT 安全数组）、`string` / `int` / `double` / `bool` / `variant` 等。

### 12.3 实现要点

- 本机 Windows SDK 的 `oaidl.h` 是**旧式 Automation 头**：
  `FUNCDESC` 没有 `params[]` 数组和 `flags` 成员，参数描述在 `lprgelemdescParam` 里，
  `cParamsOpt` 直接就是官方语义上的 `ParamsOpt`（变长参数函数为 -1，
  官方 `tlbDoc` 正是用 `ParamsOpt==-1` 判断「不定个数参数」）。
  `TA_F*` / `FUNCFLAG_*` 头文件未定义，按 Automation ABI 固定取值在源码内补齐。
  既有代码里 `fd->invkind` 已在 MSXML 事件实测中验证过与运行期一致，
  沿用同一套布局。
- `GetImplType` / `GetImplTypeFlags` 同样按旧式原型实现
  （`GetRefTypeOfImplType` 取 HREFTYPE → `GetRefTypeInfo` 取类型信息）。

### 12.4 实测结果

以系统类型库 `C:\Windows\System32\stdole2.tlb`（42 个类型）为例：

- `GetTypeAttr()`：`typekind` 覆盖 record / interface / alias / enumeration /
  dispinterface / coclass / module 全部分支；GUID 形如
  `{00000000-0000-0000-C000-000000000046}`（IUnknown，正确）；
  `Funcs` / `Vars` / `ImplTypes` / `flags.*` 均正确。
- `GetFuncDesc(1)`（IUnknown 的 QueryInterface）：`name=QueryInterface`、
  `invkind=function`、`Params=2`、`ParamsOpt=0`、`type=int`（VT_HRESULT=25）、
  `vt=25`、`memid=1610612736`（0x60000000，正确）、
  `restricted=true`（IUnknown::QueryInterface 确实带 FUNCFLAG_FRESTRICTED）、
  `parameters[1].name=riid` / `type=pointer`（VT_PTR=26）/ `in=true`。
- 官方 `com.typeAttrs.summary` → 库名 stdole、8 个 interface、2 个 coclass、2 个枚举；
  官方 `com.typeAttrs.load` → 42 个类型；
  官方 `com.tlbDoc` → 输出 11121 字的类型库文档（此前完全是死的）。
- 官方 `com.interface.loadTypelibrary(tlb, "IFont")` → **动态生成了接口声明类**
  `com.interface.IFont`（IID `{BEF6E002-A874-101A-8BBA-00AA00300CAB}` 正确），
  可实例化。官方 `com.interface` 命名空间由此从「只有手写声明类」扩展到
  「可从任意类型库动态生成」。

### 12.5 `com.ImplInterface` / `com.ImplInterfaceFromTypelib` 与原始接口事件连接点（本轮闭合）

上一轮记为「仍未实现」的两项现已补齐：`com.ImplInterface(implTable, progID, interfaceName)`
（3 参形式）与 `com.ImplInterfaceFromTypelib(implTable, typelibPath, interfaceName, coclassName)`，
连同它们依赖的「原始（非 IDispatch）接口事件连接点 Advise」一并打通。

#### 实现要点

- **vtable 生成器**（`engine/com.cpp` `BuildRawSink`）：按 ITypeInfo 递归收集
  继承链的方法，槽位顺序 = IUnknown(3) + 继承接口 + 自身方法（`GetFuncDesc` 下标顺序，
  与官方 `com.interface.loadTypelibrary` 生成声明类的顺序一致）；
  每个槽位一个 stdcall 跳板，参数按 ELEMDESC 转换（BSTR / VARIANT / 接口指针…），
  方法按名字转发到 aardio 事件表的同名成员。
- **接口指针 vs 结构体指针的区分**：`VT_PTR|VT_USERDEFINED` 既可能是接口指针
  （要转成 COM 对象），也可能只是指向结构体（例如 REFIID 就是 GUID 结构体的指针）。
  靠类型信息的 typekind 判定：只有 `TKIND_INTERFACE` / `TKIND_DISPATCH` 才是接口。
- **IDispatch 四方法的参数码必须硬编码**：`IDispatch::Invoke` 的 `pVarResult`
  是 `VARIANT*` **出参**，按类型信息推导会被判成「按值 VARIANT」而读到未初始化内存
  （`VT_DISPATCH` + 垃圾指针 → `AddRef` 崩溃）。因此 IDispatch 的四个方法
  按真实签名写死参数码。

#### 实测证据（Windows Media Player）

官方文档示例 `com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")`
（`D:\aardio\docs\library-guide\builtin\com\ImplInterface.md:19-24`）端到端跑通：

- `com.AddConnection(wmp, sink)` 返回 cookie=1（Advise 成功）；
- 服务器真实派发事件，aardio 处理函数被调用：
  `OpenStateChange(1)` / `StatusChange` / `PlayStateChange(9)` / `OpenStateChange(7)` … 共 11 次；
- `com.ImplInterfaceFromTypelib(events,"C:\\Windows\\System32\\wmp.dll",
  "_WMPOCXEvents","WindowsMediaPlayer")` 同样收到 11 次事件；
- `com.Connect(wmp, events)` 自动路径同样收到 9 次事件。

#### 三个关键实测结论（与直觉相反，值得记住）

1. **服务器按「源接口 IID」查 sink，不是按 IID_IDispatch**。
   WMP 的 `_WMPOCXEvents` 在类型库里登记为 dispinterface，但连接点的源接口 IID
   是 `{6BF52A51-394A-11D3-B153-00C04F79FAA6}` —— 纯 IDispatch 接收器会被服务器拒绝，
   返回 `0x80040202 EVENT_E_ALL_SUBSCRIBERS_FAILED`。
   因此 `com.Connect` 自动路径必须判断源接口 IID：不是 `IID_IDispatch` 时
   改用原始 vtable 接收器（其 QI 同时回答源接口 IID 与 `IID_IDispatch`）。
2. **事件由 `IDispatch::Invoke` 派发**，服务器命中的是 vtable 的第 7 个槽
   （index 6）。所以 vtable 里**必须**包含 IDispatch 的四个槽位，少一个就崩。
3. **dispinterface 的 `cImplTypes` 在不同类型库副本里可能不一致**：
   有的副本登记了 IDispatch 基接口（vtable 自带四槽），有的为空。
   因此对 `TKIND_DISPATCH` 无条件补齐 IDispatch 四槽，不能依赖类型信息。

#### 未闭合

`com.ImplInterface` 的 **1 参形式**（把 aardio 对象转成 IDispatch 接口对象）此前已实现，
本轮未改动；`com.interface` 命名空间下的**静态**接口声明类路径（`com.interface` +
`raw.interface`）也维持原状 —— 官方 `raw.interface` 依赖 struct 字段顺序声明，
与本轮「按 ITypeInfo 动态生成 vtable」的路线互补而非替代。

### 12.6 回归锁

`tests/builtin_api_check.ps1` 新增第 7 段（`com.ImplInterface / 原始接口事件`），
覆盖：3 参形式的未知接口报错、`com.ImplInterface` / `com.ImplInterfaceFromTypelib` /
`com.Connect` 三条路径的真实事件派发、事件名与参数值正确、解绑正常。
本段与第 6 段（`comTypeInfo / 官方类型库消费者`，32 条断言）一起构成回归锁，
双架构（x64 / x86）11 套既有回归全绿。