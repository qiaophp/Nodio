# IDE 编辑器字体回归锁（nodio.ui.editor 的字体是不是真的设上了）。
#
# 无头：直接跑 tests\ide_font\font.nodio，断言逐条 PASS。
#
# 背景（这轮修的真 bug）：
#   官方 LOGFONT 的字体名字段是 **name**，不是 faceName。传 faceName 会被
#   静默忽略 → 字体落回 Segoe UI（比例字体）→ 代码不等宽、汉字走回退字体
#   显得更大 → 同一行里中英文「时大时小」。
#   详见 docs/capability-gap.md §1.45。
#
#   pwsh -File tests/ide_font_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script  = 'D:\Nodio\tests\ide_font\font.nodio',
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

$log = Join-Path $env:TEMP ("nodio_ide_font_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: font fixture timed out after ${TimeoutSec}s"
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

if ($out -notmatch '== IDE FONT OK ==') {
    Write-Host "  FAIL 缺少收尾标记 '== IDE FONT OK =='"
    $fail++
}
if ($fail -eq 0) {
    Write-Host "== IDE-FONT OK ($pass 项) =="
    exit 0
}
Write-Host "== IDE-FONT FAIL: $fail =="
exit 1
