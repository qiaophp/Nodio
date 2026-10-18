# Nodio architecture

Clean-room rebuild of the aardio ecosystem: an engine-first, x64-first core
with a **self-hosted IDE** (aardio source running on the Nodio VM) that
replaces the retired MFC shell. See `roadmap.md` for the compatibility
ledger; this file is the module map.

## Repository layout

| Path | Contents |
|---|---|
| `engine/` | `nodio_engine` (static lib) — the tree-walking interpreter: lexer/parser, value model, runtime, FFI (x64 ABI + JIT call stubs), GDI/COM/CLR hosts, `import`, builtins, `(debug)` introspection. Win32 + CRT only. |
| `publish/` | `nodio_publish` (static lib) — binary-safe payload embedder, `.aproj`/`.nproj` project rules, import-closure collection, PE icon/version/manifest/UAC stamping. |
| `apps/nodio/` | `nodio.exe` — the standalone host. |
| `apps/nodio-lsp/` | `nodio-lsp.exe` — Language Server Protocol over stdio. |
| `apps/nodio-dap/` | `nodio-dap.exe` — Debug Adapter Protocol over stdio. |
| `apps/jsonrpc_stdio.h` | shared Content-Length JSON-RPC transport for the two servers. |
| `ide/nodio/` | the self-hosted IDE (aardio), modularized; carries its own `ide.nproj`. |
| `docs/` | this file, `roadmap.md`, the audit trail. |
| `tests/` | VM regression suite, publish/m7 fixtures, sweeps, the IDE smoke. |

Discipline: the **component name is the include prefix** and is never nested
under a `src/` dir — `#include "engine/..."` / `"publish/..."` / `"apps/..."`
resolve from the repo root, which every target puts on its include path
(`nodio_common`). Adding a component means adding a dir + its own
`CMakeLists.txt` + one `add_subdirectory` in the root.

## Build

```
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

Top-level `CMakeLists.txt` only sets shared options and calls
`add_subdirectory` per component; each component owns its own `CMakeLists.txt`.

Targets:

| target | type | links | output |
|---|---|---|---|
| `nodio_engine` | static lib | Win32 DLLs | `build-<arch>/engine/<cfg>/nodio_engine.lib` |
| `nodio_publish` | static lib | `nodio_engine` | `build-<arch>/publish/<cfg>/nodio_publish.lib` |
| `nodio` | exe | `nodio_publish` | `build-<arch>/<cfg>/nodio.exe` |
| `nodio-lsp` | exe | `nodio_engine` | `build-<arch>/<cfg>/nodio-lsp.exe` |
| `nodio-dap` | exe | `nodio_engine` | `build-<arch>/<cfg>/nodio-dap.exe` |

All three executables land in one `build-<arch>/<cfg>/` dir
(`CMAKE_RUNTIME_OUTPUT_DIRECTORY`), so the IDE and the test scripts find them
by a single known path.

## Runtime contracts

**`nodio.exe`** (environment, see `apps/nodio/main.cpp`):

| var | meaning |
|---|---|
| `NODIO_RUN` / `argv[1]` | run a script headlessly |
| `NODIO_LIBDIR` | official-library root for `import` |
| `NODIO_CHECK` | parse-only sweep over a list file |
| `NODIO_DEBUG` / `NODIO_BREAK="3,7"` | console mini-debugger / breakpoints |
| `NODIO_CONSOLE` / `NODIO_PAUSE` | console allocation / hold open |
| `NODIO_PUBLISH` + `NODIO_PUB*` | publisher pipeline |

**`nodio-lsp` / `nodio-dap`**: LSP / DAP over stdio, `Content-Length` framed
(`apps/jsonrpc_stdio.h`). Both negotiate capabilities first; unimplemented
requests get a null/empty result rather than a hang, so clients can attach at
any stage.

## Self-hosted IDE

`ide/nodio/main.nodio` is only the assembly point: it builds the shell from
the DSG layout, wires the modules and enters `win.loopMessage()`. Everything
else is a module under `ide/nodio/app/`:

| module | responsibility |
|---|---|
| `app/shell.nodio` | main form (DSG `winform.add`), menu, status bar, hotkeys |
| `app/editor.nodio` | editor pane (`richedit`), highlighting, diagnostics marks |
| `app/output.nodio` | output pane |
| `app/project.nodio` | project model: read/write `.nproj` / `.aproj`, file tree, start file |
| `app/runner.nodio` | F5: run the start file via `process.popen` |
| `app/lspClient.nodio` | `nodio-lsp` client (completion / hover / diagnostics) |
| `app/dapClient.nodio` | `nodio-dap` client (breakpoints / stepping / variables) |

UI rule: the form and its controls are declared with **DSG**
(`/*DSG{{*/ … /*}}*/` + `winform.add`), never the direct
`win.ui.ctrl.xxx(wf, …)` constructor — the latter yields unrealized "fake"
windows (`hwnd = null`) whose `.text` silently no-ops. The editor is
`richedit` (CHARFORMAT2 coloring), not Scintilla.

## Project model (`.nproj` / `.aproj`)

One XML schema, two extensions: `.nproj` is Nodio-native, `.aproj` is the
official aardio project file — the IDE opens either, so aardio projects load
without conversion. Schema (superset of the official one):

```xml
<?xml version="1.0" encoding="utf-8"?>
<project start="main.aardio">
	<file path="main.aardio" />
	<file path="res\hello.txt" embed="true" />
	<file path="res\secret.txt" embed="true" ignored="true" />
	<file path="test" local="true" />     <!-- not embedded, copied local -->
</project>
```

- `start` — the launch file relative to the project root (defaults to
  `main.aardio` when absent, matching the official template).
- `path` — relative to the project root (the directory holding the project
  file).
- `embed` / `ignored` / `local` — the official per-file semantics; the
  publisher already honours them, the IDE mirrors them.

The IDE itself is an ordinary project: `ide/nodio/ide.nproj` lists the IDE
sources, so the IDE can open and run itself (self-hosting).

## Milestones

- **M0** engine + host — done (parse 868/868, VM regression green).
- **M1** self-hosted shell in DSG — done (form + tree/editor/output/status +
  menu + F5 via `process.popen`; `tests/ide_smoke.ps1`).
- **M2** `nodio-lsp` — skeleton up (capabilities + framing); wire parser to
  diagnostics / completion / symbols / hover.
- **M3** `nodio-dap` — skeleton up (capabilities + handshake); wire the
  debuggee control channel onto the engine `(debug)` API.
- **M4** project model — `.nproj`/`.aproj` open, file tree, start file, F5/F7
  on the project root.
- **M5** form designer (Phase 4) — DSG round-trip + HWND canvas.
