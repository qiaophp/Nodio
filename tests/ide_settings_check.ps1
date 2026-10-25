# IDE 设置模块回归锁（app.settings：主题 / 编辑器字号 / 外部工具）。
#
# 无头：直接跑 tests\ide_settings\sett.nodio，断言逐条 PASS。
# 配置写到临时文件（NODIO_IDE_CFG），**不碰**用户的 %APPDATA%\Nodio\ide.cfg。
#
#   pwsh -File tests/ide_settings_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script  = 'D:\Nodio\tests\ide_settings\sett.nodio',
    [string]$LibDir  = 'D:\aardio\lib',
    [string]$Overlay = 'D:\Nodio\lib',
    [int]$TimeoutSec = 60
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
# 隔离的配置：临时目录 + 固定文件名，测完删掉
$cfgDir  = Join-Path $env:TEMP 'nodio_ide_settings_cfg'
if (-not (Test-Path $cfgDir)) { New-Item -ItemType Directory -Path $cfgDir | Out-Null }
$env:NODIO_IDE_CFG = Join-Path $cfgDir 'ide.cfg'
if (Test-Path $env:NODIO_IDE_CFG) { Remove-Item $env:NODIO_IDE_CFG -Force }

$log = Join-Path $env:TEMP ("nodio_ide_settings_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: settings fixture timed out after ${TimeoutSec}s"
    exit 1
}
Start-Sleep -Milliseconds 200
$out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue
Remove-Item $env:NODIO_IDE_CFG -Force -ErrorAction SilentlyContinue

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

if ($out -notmatch '== IDE SETTINGS OK ==') {
    Write-Host "  FAIL 缺少收尾标记 '== IDE SETTINGS OK =='"
    $fail++
}
if ($fail -eq 0) {
    Write-Host "== IDE-SETTINGS OK ($pass 项) =="
    exit 0
}
Write-Host "== IDE-SETTINGS FAIL: $fail =="
exit 1
