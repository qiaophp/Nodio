# Nodio

Clean-room rebuild of the aardio ecosystem: a pure engine-first, x64-first
rewrite with the retired MFC IDE shell replaced by a modern WTL + Scintilla
front end (Phase 3).

## Repository layout

| Directory | Contents |
|---|---|
| `engine/` | `NodioVM.cpp/.h` — the self-contained tree-walking aardio interpreter (Win32 + CRT only, **zero MFC**), with: unified Microsoft x64 ABI call path (shadow space, XMM float/double marshalling), per-signature JIT call stubs, rewritten callback trampolines |
| `host/` | tiny console host (`NODIO_RUN` / `NODIO_DEBUG` / `NODIO_BREAK` / `NODIO_LIBDIR` / `NODIO_PAUSE` env contract) that stands in for the F5 runtime and the regression harness |
| `tests/vmtests/` | the 48-test language/stdlib regression suite (`run_vm_tests.ps1 -Exe <host>`) |
| `tests/m7/` | GUI/plus-control acceptance scripts (run against the official `D:\aardio\lib` via `NODIO_LIBDIR`) |
| `docs/` | roadmap and the reverse-engineering audit trail |

## Build

```
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release

cmake -S . -B build-x86 -A Win32      # retained for legacy DLL authors
cmake --build build-x86 --config Release
```

## Test

```
powershell -File tests\run_vm_tests.ps1 `-Exe <full\path\to\host.exe>
```

Expects the official aardio library tree at `D:\aardio\lib` for the
library-import cases (never redistributed here).

## Status

See `docs/roadmap.md` for the phase ledger (engine / publisher / IDE /
form designer) and the current 100%-compatibility boundary.

## File extensions

`import` resolves modules with both extensions, in this priority order
per search root (script directory first, then `NODIO_LIBDIR`):

1. `rel\_.aardio` (official folder-module layout)
2. `rel.aardio`
3. `rel\_.nodio`
4. `rel.nodio`

Scripts themselves may be `.aardio` or `.nodio` interchangeably.
