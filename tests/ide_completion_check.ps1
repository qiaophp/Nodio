# 补全链路回归锁（nodio.intellisense 词库 + 编辑器限定符前缀）。
#
# 无头：直接跑 tests\ide_completion\comp.nodio，逐条 PASS。
#
#   pwsh -File tests/ide_completion_check.ps1 [-Exe <nodio.exe>]
#
# 词库数据源：**自带夹具**（tests\ide_completion\fixtures\intellisense\），
# 照官方三种形态各写一份 —— 带 "----" 分隔线的 kernel、无分隔线+注释头的
# winform、重定向条目 unimportant。夹具驱动 → 任何机器都能跑，不依赖
# 是否装了官方 aardio。官方真实词库只做**冒烟**（装了才跑，没装跳过）。
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script  = 'D:\Nodio\tests\ide_completion\comp.nodio',
    [string]$LibDir  = 'D:\aardio\lib',
    [string]$Overlay = 'D:\Nodio\lib',
    [int]$TimeoutSec = 90
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

$log = Join-Path $env:TEMP ("nodio_ide_completion_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: completion fixture timed out after ${TimeoutSec}s"
    exit 1
}
Start-Sleep -Milliseconds 200
$out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
$errText = if (Test-Path $err) { [IO.File]::ReadAllText($err, [Text.Encoding]::UTF8) } else { '' }
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

Write-Output $out

$fail = 0
foreach ($l in ($out -split "`r?`n")) {
    if ($l -match '^FAIL ') { Write-Host "  FAIL $l"; $fail++ }
}
$pass = @($out -split "`r?`n" | Where-Object { $_ -match '^PASS ' }).Count
$skip = @($out -split "`r?`n" | Where-Object { $_ -match '^SKIP ' }).Count

if ($out -notmatch '== IDE COMPLETION OK') {
    Write-Host "  FAIL 缺少收尾标记 '== IDE COMPLETION OK =='"
    if ($errText.Trim()) { Write-Host ("   stderr: " + $errText.Trim()) }
    $fail++
}
if ($fail -eq 0) {
    Write-Host "== IDE-COMPLETION OK ($pass 项, 跳过 $skip 项) =="
    exit 0
}
Write-Host "== IDE-COMPLETION FAIL: $fail =="
exit 1
