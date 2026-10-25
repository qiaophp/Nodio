# AI 面板回归锁（nodio.ai.chat / nodio.ai.cred / nodio.ai.http）。
#
# 无头：直接跑 tests\ide_ai\ai.nodio，断言逐条 PASS。
#
#   pwsh -File tests/ide_ai_check.ps1 [-Exe <nodio.exe>]
#
# ⚠ 第 3 层（WinINet 实发）要联网；没网时 fixture 打印 SKIP 而不是 FAIL。
# ⚠ 凭据测试会往 Windows 凭据管理器写一条测试凭据，fixture 结尾会删掉。
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script  = 'D:\Nodio\tests\ide_ai\ai.nodio',
    [string]$LibDir  = 'D:\aardio\lib',
    [string]$Overlay = 'D:\Nodio\lib',
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe))    { "EXE NOT FOUND: $Exe"; exit 2 }
if (-not (Test-Path $Script)) { "SCRIPT NOT FOUND: $Script"; exit 2 }

$env:NODIO_PAUSE          = '0'
$env:NODIO_CONSOLE        = '1'
$env:NODIO_LIBDIR         = $LibDir
$env:NODIO_OVERLAY_LIBDIR = $Overlay
$env:NODIO_EXE            = $Exe
$env:NODIO_RUN            = $Script
# ⚠ API Key 的用例会 **写 + 删** 凭据 —— 必须用隔离的凭据名，
#   否则会把用户自己配的那条 "NodioIDE/ai/apiKey" 抹掉。
$env:NODIO_AI_CRED_TARGET = 'NodioIDE/test/ide_ai_check'

$log = Join-Path $env:TEMP ("nodio_ide_ai_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: ai fixture timed out after ${TimeoutSec}s"
    exit 1
}
Start-Sleep -Milliseconds 200
$out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

Write-Output $out

$fail = 0
$lines = $out -split "`r?`n"
foreach ($l in $lines) {
    if ($l -match '^FAIL ') {
        Write-Host "  FAIL $l"
        $fail++
    }
}
$pass = @($lines | Where-Object { $_ -match '^PASS ' }).Count
$skip = @($lines | Where-Object { $_ -match '^SKIP ' }).Count

if ($out -notmatch '== IDE AI OK ==') {
    Write-Host "  FAIL 缺少收尾标记 '== IDE AI OK =='"
    $fail++
}
if ($fail -eq 0) {
    Write-Host "== IDE-AI OK ($pass 项, 跳过 $skip 项) =="
    exit 0
}
Write-Host "== IDE-AI FAIL: $fail =="
exit 1
