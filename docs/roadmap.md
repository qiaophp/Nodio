# Nodio roadmap — 100% aardio source compatibility, x64 first

Decisions locked with the project owner:

1. **Retire the MFC shell.** The official aardio IDE is an MFC application
   (confirmed via RTTI imports of the original `aardio.exe`). Rebuilding
   that shell in C++/MFC is the largest failure mode of the previous
   reconstruction: instead, the VM engine is extracted MFC-free (this
   repo), and the IDE is re-built on **native Win32 + Scintilla**
   (Phase 3; the v0 shell retired the WTL idea — no framework dependency).
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

### Library import coverage (51 top-level namespaces, enumerated by
### `tests/ns_sweep.sh` from the official tree; measured 2026-10-14)
| arch | importing | note |
|---|---|---|
| x64 | **42 / 51** | residue table below |
| x86 | **45 / 51** | same module set, minus the x64 arch limit |

The remainder is specific, not vague — and the earlier "environmental
ceiling" claim was wrong: it hid real engine defects (`~` path resolution,
the `io.exist` return contract, custom API types).

| namespace | cause | next step |
|---|---|---|
| `base64`, `sqlite`, `zlib`, `golang`, `nodeJs`, `py2`, `py3` (x64 only) | bundle 32-bit-only DLLs (`zlibwapi.dll`, `python27/38.dll`, MZ payloads) | OS limit on x64; all seven import on x86 |
| `System` | `dotNet.import(...)` — no CLR host | needs the CLR hosting layer |
| `ide` (both arch) | the engine-provided `(debug)` module lacks `getMainId` (`@ 行 24` of the chain) | add the API |
| `py2`, `py3` (x86) | `string.trimRight` primitive missing (`_.aardio:141`) | add the primitive |
| `nodeJs` (x86) | hits the `var in_addr = in_addr` self-alias engine defect via its `wsock` chain | engine fix below |
| `golang` (x86) | its own import guard fires during `import golang` (self-reference) | verify the root-binding contract |

Current state in one line: **source parsing 100%**, **import 42/51 (x64) /
45/51 (x86)**, residue enumerated above; behaviour-level completion is
tracked in the ledger below.

### Completion ledger (denominator pinned 2026-10-14)

Aggregation weights are for reporting only and may be re-weighted:
engine 40 / library 25 / publisher 15 / IDE 15 / designer 5.

| area | weight | score | measured evidence |
|---|---|---|---|
| engine (language + VM + FFI) | 40 | **85%** | parse 868/868 both arch; VM regression 60/60 both arch; imports 42/51 x64, 45/51 x86 with the residue above; open semantic defect clusters listed below |
| official library (behaviour) | 25 | **27%** | headless sweep of the pinned 632-example manifest (`tests/sweep_examples.sh`): 157 clean / 444 error / 31 hang — 24.8% raw, 27% after removing the OS-physical 32-bit-DLL set from the denominator (157/572) |
| publisher (Phase 2) | 15 | **100%** | closed; fixtures green x64 + x86 |
| IDE shell (Phase 3) | 15 | **25%** | native Win32 + Scintilla v0: edit / aardio highlight / project tree / F5 run / output pane; black-box smoke test drives the real window. No MDI tabs, completion or debug UI yet |
| form designer (Phase 4) | 5 | **0%** | not started |

**Overall ≈ 59.5 / 100.** Per decision 5, the engine and the official
library must reach 100% — the defect list below is that path:

1. `var x = x;` self-alias inside a namespace loses the class value:
   bare `in_addr()` at `wsock/_.aardio:132` calls a non-function
   (`Network/wsock/*`, `Network/Manage/inet.adapter.aardio:476`, and
   x86 `nodeJs` through its `wsock` chain — 15+ examples)
2. `import process` returns a table that lost the class members declared
   in the trailing `namespace process{}`; `process.PROCESS_INFORMATION` /
   `.STARTUPINFO` read null (13 examples). `BindModule` hands TableT,
   not Class; suspected re-entry via `process/job/limitKill.aardio:1`
3. runaway recursion dies with 栈溢出 and no depth-guard contract (7 examples)
4. `(debug)` API gaps: `getMainId` missing → `import ide` fails (both arch)
5. `string.trimRight` primitive missing → x86 `py2`/`py3` fail at `_.aardio:141`
6. CLR host for `System` (`dotNet.import`)
7. language-host primitives: py2/py3 (x86), `golang` import self-guard

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

## Phase 3 — IDE shell (`ide/`, native Win32 + Scintilla)
- [x] VM introspection API for `(debug)` (`queryinfo`/`traceback`/
      `callcount`/`hook`/`open`/`close`, engine-provided named module) —
      `import debug` green; `getMainId` still missing and blocks `import ide`
- [ ] Tabbed MDI frame, docking panes (v0 has a fixed project-tree +
      output-pane layout)
- [x] Scintilla integrated with a custom aardio lexer (container lexer,
      UTF-8 native, per-line block-comment state)
- [x] Real-time run/output plumbing over the `NODIO_*` contract
      (pipe reader thread → output pane; smoke test drives the real window)

## Phase 4 — visual form designer (`designer/`)
- [ ] Native HWND design canvas + 8-handle resize tracker
- [ ] DSG `/*DSG{{*/ ... /*}}*/` round-trip parser/serializer
- [ ] Toolbox + property grid
