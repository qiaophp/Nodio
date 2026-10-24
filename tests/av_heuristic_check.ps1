# Regression lock for the antivirus heuristic false positive.
#
# Symptom (observed 2026-10-21): the 32-bit build of nodio.exe was reported
# and quarantined by 火绒 as "HEUR:Backdoor/Farfli.e", while the 64-bit
# build of the very same sources was never flagged. Bisection showed the
# report needs two things at once:
#
#   1. memory that is writable AND executable at the same time
#      (the FFI trampoline pool used to be allocated PAGE_EXECUTE_READWRITE,
#       and the in-memory PE loader mapped whole images that way too), and
#   2. a 32-bit unhandled-exception filter that dumps the raw register set
#      (EIP/EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI) - the textbook shape of an
#      SEH based anti-debug handler.
#
# The fix keeps the capability but removes the pattern: pages are mapped
# writable and demoted to PAGE_EXECUTE_READ once the code is written, and
# the 32-bit crash filter prints only the faulting instruction pointer.
#
# This script locks both halves so a later change cannot silently put the
# pattern back.
param()
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src = Get-ChildItem (Join-Path $root 'engine') -File -Include *.cpp,*.h -Recurse
if (-not $src) { "ENGINE SOURCES NOT FOUND"; exit 2 }

$fail = 0

# --- 1) no allocation may request a writable+executable page --------------
foreach ($f in $src) {
    $lines = Get-Content $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'VirtualAlloc(Ex)?\s*\(') {
            # look at the call itself (it may span a few lines)
            $chunk = ($lines[$i..([Math]::Min($i + 4, $lines.Count - 1))]) -join ' '
            if ($chunk -match 'PAGE_EXECUTE_READWRITE') {
                "FAIL: $($f.Name):$($i + 1) allocates PAGE_EXECUTE_READWRITE"
                $fail++
            }
        }
    }
}

# --- 2) the 32-bit crash filter must not dump the raw register set ---------
$entry = Join-Path $root 'engine\entry.cpp'
if (Test-Path $entry) {
    $text = Get-Content $entry -Raw
    if ($text -match 'EAX=%08X') {
        "FAIL: engine/entry.cpp still dumps the full x86 register set in the crash filter"
        $fail++
    }
}
else { "engine/entry.cpp NOT FOUND"; exit 2 }

if ($fail -gt 0) { "== av heuristic lock FAILED ($fail) =="; exit 1 }
"== av heuristic lock OK =="
exit 0
