# Phase 2 regression: publish the fixture project and assert the standalone
# exe runs with the expected output. Exercises the whole pipeline —
# .aproj embed rules, import closure, byte-exact payload append, subsystem
# patch — plus the host's payload self-detection at startup.
#
# Usage: powershell -File run_publish_tests.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Join-Path $dir 'm7\proj'
$entry = Join-Path $proj 'main.aardio'
$dist = Join-Path $proj 'dist'
$out = Join-Path $dist 'main.exe'

if (-not (Test-Path $entry)) { "FIXTURE MISSING: $entry"; exit 2 }
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_PUBLISH'] = $entry
$psi.EnvironmentVariables['NODIO_PUBOUT'] = $out
$psi.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'

$p = [System.Diagnostics.Process]::Start($psi)
# drain both pipes before waiting (see run_vm_tests.ps1)
$pubTask = $p.StandardOutput.ReadToEndAsync()
$pubErrTask = $p.StandardError.ReadToEndAsync()
$p.WaitForExit()
$stdout = $pubTask.Result + $pubErrTask.Result

if (-not (Test-Path $out)) {
    "FAIL publish: output exe not created"
    "--- publisher output ---"
    $stdout
    exit 1
}

# The published exe must run standalone: no NODIO_* env, no lib tree.
$run = New-Object System.Diagnostics.ProcessStartInfo
$run.FileName = $out
$run.UseShellExecute = $false
$run.RedirectStandardOutput = $true
$run.RedirectStandardError = $true
$run.StandardOutputEncoding = [Text.Encoding]::UTF8
$run.StandardErrorEncoding = [Text.Encoding]::UTF8
$r = [System.Diagnostics.Process]::Start($run)
$runTask = $r.StandardOutput.ReadToEndAsync()
$runErrTask = $r.StandardError.ReadToEndAsync()
$r.WaitForExit()
$out2 = $runTask.Result + $runErrTask.Result

$lines = $out2 -split "`r?`n" | Where-Object { $_ -ne '' }
$want = @(
    'entry=main',
    'lib:mylib-embedded',
    'res:hello from embedded resource',
    'blob:776',
    'memdll:true',
    'memdll.add(100,23):123',
    'memdll.hello:hello-from-mem-dll'
)

$fail = 0
for ($i = 0; $i -lt $want.Count; $i++) {
    if ($lines.Count -gt $i -and $lines[$i] -eq $want[$i]) {
        "PASS $($want[$i])"
    } else {
        $got = if ($lines.Count -gt $i) { $lines[$i] } else { '<missing>' }
        "FAIL want=[$($want[$i])] got=[$got]"
        $fail++
    }
}

if ($fail -eq 0) { "== publish regression green ==" }
else {
    "--- full published output ---"
    $out2
    "--- publisher output ---"
    $stdout
}
if ($fail -gt 0) { exit $fail }

# ---- 2) Nodio-native manifest: default.nproj auto-probe --------------------
# The publisher must treat .nproj (same <file> XML shape as .aproj) as a
# project manifest when there is no default.aproj next to the entry.
$proj2 = Join-Path $dir 'm7\proj_nproj'
$entry2 = Join-Path $proj2 'main.aardio'
$out2f = Join-Path $proj2 'dist\main.exe'
if (-not (Test-Path $entry2)) { "FIXTURE MISSING: $entry2"; exit 2 }
New-Item -ItemType Directory -Force -Path (Join-Path $proj2 'dist') | Out-Null

$psi2 = New-Object System.Diagnostics.ProcessStartInfo
$psi2.FileName = $Exe
$psi2.UseShellExecute = $false
$psi2.RedirectStandardOutput = $true
$psi2.RedirectStandardError = $true
$psi2.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi2.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi2.EnvironmentVariables['NODIO_PUBLISH'] = $entry2
$psi2.EnvironmentVariables['NODIO_PUBOUT'] = $out2f
$psi2.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
$psi2.EnvironmentVariables['NODIO_PAUSE'] = '0'

$p2 = [System.Diagnostics.Process]::Start($psi2)
$pubTask2 = $p2.StandardOutput.ReadToEndAsync()
$pubErrTask2 = $p2.StandardError.ReadToEndAsync()
$p2.WaitForExit()
$pubOut2 = $pubTask2.Result + $pubErrTask2.Result

if (-not (Test-Path $out2f)) {
    "FAIL publish(nproj): output exe not created"
    "--- publisher output ---"
    $pubOut2
    exit 1
}

$run2i = New-Object System.Diagnostics.ProcessStartInfo
$run2i.FileName = $out2f
$run2i.UseShellExecute = $false
$run2i.RedirectStandardOutput = $true
$run2i.RedirectStandardError = $true
$run2i.StandardOutputEncoding = [Text.Encoding]::UTF8
$run2i.StandardErrorEncoding = [Text.Encoding]::UTF8
$r2 = [System.Diagnostics.Process]::Start($run2i)
$runTask2 = $r2.StandardOutput.ReadToEndAsync()
$runErrTask2 = $r2.StandardError.ReadToEndAsync()
$r2.WaitForExit()
$runOut2 = $runTask2.Result + $runErrTask2.Result

if ($runOut2 -match 'nproj-entry=main') { "PASS nproj-manifest-auto (entry runs)" }
else { "FAIL nproj-manifest-auto: payload did not run"; $runOut2; $fail++ }
if ($runOut2 -match 'nproj-res:note-from-nproj-manifest') { "PASS nproj-embed-res" }
else { "FAIL nproj-embed-res: manifest embed=true resource missing from payload"; $runOut2; $fail++ }

# ---- 3) explicit manifest via NODIO_PUBPROJ --------------------------------
$out3f = Join-Path $proj2 'dist\custom.exe'
$psi3 = New-Object System.Diagnostics.ProcessStartInfo
$psi3.FileName = $Exe
$psi3.UseShellExecute = $false
$psi3.RedirectStandardOutput = $true
$psi3.RedirectStandardError = $true
$psi3.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi3.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi3.EnvironmentVariables['NODIO_PUBLISH'] = $entry2
$psi3.EnvironmentVariables['NODIO_PUBOUT'] = $out3f
$psi3.EnvironmentVariables['NODIO_PUBPROJ'] = Join-Path $proj2 'custom.nproj'
$psi3.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
$psi3.EnvironmentVariables['NODIO_PAUSE'] = '0'

$p3 = [System.Diagnostics.Process]::Start($psi3)
$pubTask3 = $p3.StandardOutput.ReadToEndAsync()
$pubErrTask3 = $p3.StandardError.ReadToEndAsync()
$p3.WaitForExit()
$pubOut3 = $pubTask3.Result + $pubErrTask3.Result

if (-not (Test-Path $out3f)) {
    "FAIL publish(--proj): output exe not created"
    "--- publisher output ---"
    $pubOut3
    exit 1
}

$run3i = New-Object System.Diagnostics.ProcessStartInfo
$run3i.FileName = $out3f
$run3i.UseShellExecute = $false
$run3i.RedirectStandardOutput = $true
$run3i.RedirectStandardError = $true
$run3i.StandardOutputEncoding = [Text.Encoding]::UTF8
$run3i.StandardErrorEncoding = [Text.Encoding]::UTF8
$r3 = [System.Diagnostics.Process]::Start($run3i)
$runTask3 = $r3.StandardOutput.ReadToEndAsync()
$runErrTask3 = $r3.StandardError.ReadToEndAsync()
$r3.WaitForExit()
$runOut3 = $runTask3.Result + $runErrTask3.Result

if ($runOut3 -match 'nproj-res:note-from-nproj-manifest') { "PASS proj-explicit-manifest" }
else { "FAIL proj-explicit-manifest: --proj manifest resource missing"; $runOut3; $fail++ }

if ($fail -eq 0) { "== publish regression green (nproj + --proj) ==" }
exit $fail
