# 引擎回归锁：worker 线程退出后主线程再分配（GC 悬垂指针 bug）。
#
#   pwsh -File tests/ide_thread_gc_check.ps1 [-Exe <nodio.exe>]
#
# 崩了表现为**进程退出码非 0**（0xC0000005 → -1073741819），
# 所以这里断言的是退出码 + 两个标记行。
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script  = 'D:\Nodio\tests\ide_thread_gc.nodio',
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
# ⚠ 必须让自动 GC 开着 —— 这个 bug 只在收集器跑起来时才现形
$env:NODIO_GC_THRESHOLD   = ''

$log = Join-Path $env:TEMP ("nodio_thread_gc_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: thread/gc fixture timed out after ${TimeoutSec}s"
    exit 1
}
Start-Sleep -Milliseconds 200
$out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
$errOut = if (Test-Path $err) { [IO.File]::ReadAllText($err, [Text.Encoding]::UTF8) } else { '' }
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

Write-Output $out

if ($p.ExitCode -ne 0) {
    Write-Host ("  FAIL 进程退出码非 0：{0}（0xC0000005 = GC 摸到了已释放的 worker 栈）" -f $p.ExitCode)
    if ($errOut.Trim()) { Write-Host ('   stderr: ' + $errOut.Trim()) }
    exit 1
}
if ($out -notmatch 'TG-WORKER:') { Write-Output 'FAIL: worker thread did not run'; exit 1 }
if ($out -notmatch 'TG-MAIN-DONE') { Write-Output 'FAIL: main script did not finish'; exit 1 }

Write-Host '== IDE-THREAD-GC OK =='
exit 0
