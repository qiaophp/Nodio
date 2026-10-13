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
exit $fail
