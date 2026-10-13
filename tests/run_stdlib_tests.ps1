# Phase 1.3 regression: import every real official library and assert the
# expected log line. The official library tree is never redistributed —
# point -LibDir at a local aardio installation (default D:\aardio\lib).
#
# Usage: powershell -File run_stdlib_tests.ps1 [-Exe <nodio.exe>] [-LibDir <lib>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$LibDir = 'D:\aardio\lib'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }
if (-not (Test-Path $LibDir)) { "LIB DIR NOT FOUND: $LibDir"; exit 2 }

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $dir 'm7\test_import_stdlib.aardio'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_RUN'] = $script
$psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'

$p = [System.Diagnostics.Process]::Start($psi)
if (-not $p.WaitForExit(60000)) { $p.Kill() }
$out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
# drop parser debug chatter, keep assertion lines
$lines = $out -split "`r?`n" | Where-Object { $_ -notmatch '^\[DEBUG' -and $_.Trim() -ne '' }

$expectOk = @('win', 'win.ui', 'gdi', 'gdip', 'JSON', 'process', 'inet',
              'thread', 'fsys', 'crypt', 'fonts', 'mouse', 'winex', 'autos',
              'dotNet')
$fail = 0
foreach ($name in $expectOk) {
    if ($lines -contains "$name OK") { Write-Host "PASS import $name" }
    else { $fail++; Write-Host "FAIL import $name" }
}
# these ship 32-bit-only dlls: OK natively, SKIP-32bit-only under x64
foreach ($name in @('sqlite', 'zlib', 'base64')) {
    if ($lines -contains "$name OK" -or $lines -contains "$name SKIP-32bit-only") {
        Write-Host "PASS import $name"
    }
    else { $fail++; Write-Host "FAIL import $name" }
}

if ($fail) { "== $fail failed =="; exit 1 }
"== stdlib imports all green =="
