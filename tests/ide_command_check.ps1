# ide.command 官方外部命令面回归锁。
#
# 官方 aardio.exe 把 37 条命令（docs/official-ide-reference.md §1）注册进
# ide.command 共享注册表，外部进程 join + send(hwnd,"命令名",...) 驱动 IDE。
# Nodio 侧 app/ideCommand.nodio 挂同一张表（main.nodio 注入实现）。
#
# 本锁覆盖两段：
#   1. 跨进程总线：join → 注册 → WM_COPYDATA 按名分发 → 返回值回传
#      （曾因两个引擎缺陷全断：::Kernel32 裸调 API 返回截 32 位使
#      fsys.mmap 崩溃；table.tostring 非标识符键未括号化使注册表往返
#      静默丢失——见 capability-gap §1.38）
#   2. 命令名 → IDE 函数的注册路径（server.nodio 经 app.ideCommand.init
#      注入实现，与真实 IDE 的 main.nodio 接线同一模块）
#
#   pwsh -File tests/ide_command_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$TimeoutSec = 30
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot = Split-Path $PSScriptRoot -Parent
$hwndFile = Join-Path $env:TEMP ("idecmd_hwnd_{0}.txt" -f $PID)
if (Test-Path $hwndFile) { Remove-Item $hwndFile -Force }

$env:NODIO_PAUSE = '0'
$env:NODIO_CONSOLE = '1'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = Join-Path $repoRoot 'lib'
$env:IDECMD_HWND = $hwndFile

# ---- 1. 启动服务端 ----
$server = Join-Path $repoRoot 'tests\ide_command\server.nodio'
$srvLog = [string]::Format("{0}\idecmd_srv_{1}.log", $env:TEMP, $PID)
$srvErr = [string]::Format("{0}\idecmd_srv_{1}.err", $env:TEMP, $PID)
$pSrv = Start-Process -FilePath $Exe -ArgumentList "`"$server`"" -PassThru -NoNewWindow `
        -RedirectStandardOutput $srvLog -RedirectStandardError $srvErr

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$hwnd = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if (Test-Path $hwndFile) {
        $hwnd = ([IO.File]::ReadAllText($hwndFile)).Trim()
        if ($hwnd) { break }
    }
    if ($pSrv.HasExited) { break }
}
if (-not $hwnd) {
    try { $pSrv.Kill() } catch {}
    Write-Output "FAIL: server did not report hwnd within ${TimeoutSec}s"
    exit 1
}
Write-Output "ok   server hwnd = $hwnd"

# ---- 2. 跑客户端（同机另一进程，真实跨进程 COPYDATA）----
$client = Join-Path $repoRoot 'tests\ide_command\client.nodio'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$client`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
foreach ($k in @('NODIO_PAUSE', 'NODIO_CONSOLE', 'NODIO_LIBDIR', 'NODIO_OVERLAY_LIBDIR', 'IDECMD_HWND')) {
    $psi.EnvironmentVariables[$k] = [Environment]::GetEnvironmentVariable($k)
}
$cli = [Diagnostics.Process]::Start($psi)
$out = $cli.StandardOutput.ReadToEnd()
$err = $cli.StandardError.ReadToEnd()
if (-not $cli.WaitForExit($TimeoutSec * 1000)) { try { $cli.Kill() } catch {} }

# ---- 3. 收服务端（killOnExit 应已让它退出）----
if (-not $pSrv.WaitForExit(10000)) {
    try { $pSrv.Kill() } catch {}
    Write-Output "FAIL: server did not exit after killOnExit"
    exit 1
}

# ---- 4. 断言 ----
$kv = @{}
foreach ($line in ($out -split "`r?`n")) {
    $line = $line.Trim()
    if ($line -eq '') { continue }
    $sp = $line.IndexOf(' ')
    if ($sp -lt 0) { $kv[$line] = ''; continue }
    $kv[$line.Substring(0, $sp)] = $line.Substring($sp + 1)
}

$fail = 0
function Check([string]$key, [string]$want) {
    $got = if ($kv.ContainsKey($key)) { $kv[$key] } else { '<missing>' }
    if ($got -eq $want) { Write-Host "  ok   $key = $got" }
    else { Write-Host "  FAIL $key = '$got' (期望 '$want')"; $script:fail++ }
    if ($kv.ContainsKey('CMD-DONE') -eq $false -and $key -eq 'CMD-DONE') {
        Write-Host "  (stderr: $($err.Trim()))"
    }
}

Check 'CMD-SETCFG'    'true'
Check 'CMD-GETCFG'    'v1'
Check 'CMD-HWND'      'true'
Check 'CMD-PROJ'      'false'
Check 'CMD-GOTO'      'true'
Check 'CMD-GOTO-BAD'  'false'
Check 'CMD-SHOW'      'hello'
Check 'CMD-CODE'      'srv-active-code'
Check 'CMD-MISSING'   'null'
Check 'CMD-DONE'      ''

Remove-Item $hwndFile -ErrorAction SilentlyContinue
if ($fail -eq 0) { Write-Host '== IDE-COMMAND OK =='; exit 0 }
Write-Host "== IDE-COMMAND FAIL: $fail =="
exit 1
