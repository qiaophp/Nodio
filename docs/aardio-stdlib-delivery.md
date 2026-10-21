---
title: aardio 标准库「全量可用」交付说明
---

# aardio 标准库「全量可用」交付说明

> **一句话结论**：官方公开库 **969 个模块**，Nodio **x64 `ok=899` / x86 `ok=933`**；
> 非 OK 项里**属于 Nodio 自身缺陷的是 0 项** —— 剩余 70 / 36 项全部落在
> **物理边界 / 官方契约 / 官方库自身缺陷 / 环境限制** 四类。
> 官方唯一样本 `lib/ide/aifix.aardio`（`=aardio` 预编译块）**已被真正执行跑完**。

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

## 8. 变更记录

* **第二十一轮**：`=aardio` 官方预编译块可真正执行（owner 调用约定 + 多返回值 +
  `0x2F` 返回槽与空结果跳过 `0x22`），标准库 Nodio 侧自有缺口**归零**。
  语义取证 `docs/aardio-bytecode.md` §7.5.10。
* **第二十轮**：`0x2F` / `0x30` / `0x31` / `0x3C` 四种动态调用变体落地，
  记录 VM 收口 **61/61**。
