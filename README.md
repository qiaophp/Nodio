# Nodio

Clean-room rebuild of the aardio ecosystem: a pure engine-first, x64-first
rewrite whose IDE is **self-hosted** — written in aardio and running on the
Nodio VM — replacing the retired MFC shell.

Module map lives in `docs/architecture.md`; the compatibility ledger in
`docs/roadmap.md`; the **"compatible *and* better than official aardio"**
checklist in `docs/surpass.md` (every item states what official aardio does,
what Nodio does instead, and how it is verified).

## Repository layout

| Path | Contents |
|---|---|
| `engine/` | `nodio_engine` — the self-contained tree-walking aardio interpreter (Win32 + CRT only, **zero MFC**): unified x64 ABI call path, per-signature JIT call stubs, callback trampolines, FFI/GDI/COM/CLR hosts |
| `publish/` | `nodio_publish` — binary-safe resource embedder, `.nproj`/`.aproj` rules, PE icon/version/manifest/UAC stamping |
| `apps/nodio/` | `nodio.exe` — the standalone host (run / debug / check / publish) |
| `apps/nodio-lsp/` | `nodio-lsp.exe` — Language Server Protocol over stdio |
| `apps/nodio-dap/` | `nodio-dap.exe` — Debug Adapter Protocol over stdio |
| `ide/nodio/` | the self-hosted IDE (aardio), modularized, with its own `ide.nproj` |
| `tests/` | the VM regression suite, publish/m7 fixtures, sweeps, the IDE smoke |
| `docs/` | architecture, roadmap, reverse-engineering audit trail |

## Build

```
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release

cmake -S . -B build-x86 -A Win32      # retained for legacy DLL authors
cmake --build build-x86 --config Release
```

All executables are written to one directory: `build-x64/Release/`
(`nodio.exe`, `nodio-lsp.exe`, `nodio-dap.exe`).

## Test

```
powershell -File tests\run_vm_tests.ps1 -Exe <full\path\to\nodio.exe>
powershell -File tests\ide_smoke.ps1   -Exe <full\path\to\nodio.exe>
```

The VM suite expects the official aardio library tree at `D:\aardio\lib` for
the library-import cases (never redistributed here).

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

Scripts themselves may be `.aardio` or `.nodio` interchangeably. Projects are
`.nproj` (Nodio-native) or `.aproj` (official aardio), same XML schema.
