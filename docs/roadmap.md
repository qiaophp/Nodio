# Nodio roadmap — 100% aardio source compatibility, x64 first

Decisions locked with the project owner:

1. **Retire the MFC shell.** The official aardio IDE is an MFC application
   (confirmed via RTTI imports of the original `aardio.exe`). Rebuilding
   that shell in C++/MFC is the largest failure mode of the previous
   reconstruction: instead, the VM engine is extracted MFC-free (this
   repo), and the IDE will be re-built on **WTL + Scintilla** (Phase 3).
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

### Library import coverage (50 top-level namespaces)
| arch | importing | note |
|---|---|---|
| x64 | **41 / 50** | remainder: 32-bit-only DLLs, `global.import`/`(debug)` introspection |
| x86 | **44 / 50** | same module set, minus the x64 arch limit |

The remainder is specific, not vague — and the earlier "environmental
ceiling" claim was wrong: it hid real engine defects (`~` path resolution,
the `io.exist` return contract, custom API types).

| namespace | cause | next step |
|---|---|---|
| `base64`, `sqlite`, `zlib` | bundle 32-bit-only DLLs | OS limit on x64; all three import on x86 |
| `golang`, `nodeJs` | x64: chain reaches the 32-bit `zlibwapi.dll`; x86: missing primitive | add the missing primitive |
| `py2`, `py3` | x64: 32-bit `python27/38.dll` (OS limit); x86: missing primitive at `_.aardio:141` | add the missing primitive |
| `System` | `dotNet.import(...)` — no CLR host | needs the CLR hosting layer |
| `debug`, `ide` | `global.import("(debug)")` — no VM introspection API | Phase 3 |

Current state in one line: **source parsing 100%**, **import 44/50 (x86) /
41/50 (x64)**, residue enumerated above.

## Phase 2 — publisher (this repo, `publish/`)
- [x] Binary-safe payload embedder + `.aproj` embed/ignored/local rules
- [x] PE stamping: icon / version info / DPI-aware manifest / UAC
- [x] Publish-time syntax gate + dependency closure for `lib/`
- [x] Publish fixtures (`tests/m7/proj`, t49) + memory-DLL-from-payload
      assertions (x64 + x86 green)

## Phase 3 — IDE shell (`ide/`, WTL + Scintilla)
- [ ] VM introspection API for `(debug)` (`callcount`/`queryinfo`/`hook`/
      `getlocalvar`/`traceback`) — blocks `import debug`
- [ ] Tabbed MDI frame, docking panes (project tree / output console)
- [ ] Scintilla integrated with a custom aardio lexer (UTF-8 native)
- [ ] Real-time run/debug output plumbing over the `NODIO_*` contract

## Phase 4 — visual form designer (`designer/`)
- [ ] Native HWND design canvas + 8-handle resize tracker
- [ ] DSG `/*DSG{{*/ ... /*}}*/` round-trip parser/serializer
- [ ] Toolbox + property grid
