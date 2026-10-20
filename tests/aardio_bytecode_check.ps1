<#
    aardio_bytecode_check.ps1

    Regression for the reverse engineered '=aardio' precompiled chunk format.

    The check parses the official sample with tools\aardio_bytecode.py and
    requires a byte identical re-serialization (zero trailing bytes).

    Usage:
      pwsh -NoProfile -ExecutionPolicy Bypass -File tests\aardio_bytecode_check.ps1
      pwsh ... -Sample 'D:\aardio\lib\ide\aifix.aardio'
#>
param(
    [string]$Sample = 'D:\aardio\lib\ide\aifix.aardio',
    [string]$Tool = (Join-Path $PSScriptRoot '..\tools\aardio_bytecode.py')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Sample)) {
    Write-Host "AARDIO-BYTECODE SKIP: sample not found: $Sample"
    exit 0
}
if (-not (Test-Path $Tool)) {
    Write-Host "AARDIO-BYTECODE FAIL: tool not found: $Tool"
    exit 1
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Host "AARDIO-BYTECODE SKIP: python not available"
    exit 0
}

$out = & $python.Source $Tool $Sample --verify 2>&1
$text = ($out | Out-String)
Write-Host $text

if ($text -match 'identical=True' -and $text -match 'trailing bytes: 0') {
    Write-Host "AARDIO-BYTECODE OK"
    exit 0
}
Write-Host "AARDIO-BYTECODE FAIL: round-trip verification failed"
exit 1
