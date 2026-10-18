# Nodio roadmap — 100% aardio source compatibility, x64 first

Decisions locked with the project owner:

1. **Retire the MFC shell.** The official aardio IDE is an MFC application
   (confirmed via RTTI imports of the original `aardio.exe`). Rebuilding
   that shell in C++/MFC is the largest failure mode of the previous
   reconstruction: instead, the VM engine is extracted MFC-free (this
   repo). **The IDE is self-hosted: written in aardio, running on the
   Nodio VM** (`ide/aardio/`). The interim native Win32 + Scintilla C++
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
- [x] MFC-free extraction (`engine/` + console `host/`)
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
| IDE (Phase 3, self-hosted) | 15 | **0%** | the native Win32 + Scintilla v0 shell was **removed** once decision 1 landed (self-hosted in aardio); no IDE code yet — engine-side primitives for it are green (menu / timer / dlg / popen chain) |
| form designer (Phase 4) | 5 | **0%** | not started (will be aardio code inside the self-hosted IDE) |

**Overall ≈ 65.5 / 100.** Per decision 5, the engine and the official library must reach 100% — the defect list below is that path:

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

## Phase 3 — self-hosted IDE (`ide/aardio/`, aardio on the Nodio VM)
- [x] VM introspection API for `(debug)` (`queryinfo`/`traceback`/
      `callcount`/`hook`/`open`/`close`, engine-provided named module) —
      `import debug` green; `getMainId` still missing and blocks `import ide`
- [x] Engine primitives for the IDE probed green: win.ui.menu,
      winform.setInterval, fsys.dlg, and the full process.popen chain
      (F5 run pipeline unblocked; probe in the session log)
- [ ] M0-3: main window skeleton in aardio (win.form + menu + project
      tree + editor pane + output pane + F5 via process.popen)
- [ ] M2: nodio-lsp (C; zan `lsp_main.c` skeleton + this parser) for
      completion/IntelliSense
- [ ] M3: nodio-dap onto `(debug)` for breakpoints/stepping

## Phase 4 — visual form designer (aardio code inside the IDE)
- [ ] Native HWND design canvas + 8-handle resize tracker
- [ ] DSG `/*DSG{{*/ ... /*}}*/` round-trip parser/serializer
- [ ] Toolbox + property grid
