# 插件视图回归锁（官方 pluginViewCreate/pluginViewShow 契约）。
#
# 官方右侧栏页签 = 独立 aardio 脚本（extensions/plugin/pane/*.aardio），
# IDE loadcode 后以容器窗体调用，parent=... 由 IDE 注入；尾部
# win.loopMessage() 由宿主机械剥离（子窗体消息由主循环派发）。
#
# 复用 IDE 的 headless smoke 通道（NODIO_IDE_SMOKE=1）：main.nodio 的
# smoke 块经 app.ideCommand.dispatch 走 pluginViewCreate/Show，
# 断言 PLUGIN-* 标记。命令面接线必须先于 smoke 块（本轮把 ideCommand
# 初始化整体前移到 smoke 之前）。
#
#   pwsh -File tests/ide_plugin_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Ide     = 'D:\Nodio\ide\nodio\main.nodio',
    [int]$TimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$env:NODIO_PAUSE            = '0'
$env:NODIO_CONSOLE          = '1'
$env:NODIO_LIBDIR           = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR   = 'D:\Nodio\lib'
$env:NODIO_EXE              = $Exe
$env:NODIO_RUN              = $Ide
$env:NODIO_IDE_SMOKE        = '1'
$proj = Join-Path (Split-Path -Parent $Ide) 'ide.nproj'
if (Test-Path $proj) { $env:NODIO_IDE_PROJECT = $proj }

$log = Join-Path $env:TEMP ("nodio_ide_plugin_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: IDE timed out after ${TimeoutSec}s"
    exit 1
}

Start-Sleep -Milliseconds 200
$out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

$fail = 0
function Check([string]$marker, [string]$want) {
    $m = [regex]::Match($out, [regex]::Escape($marker) + ':(\S*)')
    $got = $m.Success ? $m.Groups[1].Value : '<missing>'
    if ($got -eq $want) { Write-Host "  ok   $marker = $got" }
    else { Write-Host "  FAIL $marker = '$got' (期望 '$want')"; $script:fail++ }
}

# 与 smoke 块同跑的基础标记（确保 IDE 启动本身没坏）
if ($out -notmatch 'SMOKE-BRANCH') {
    Write-Host "  FAIL SMOKE-BRANCH missing (IDE 未进入 smoke 模式)"
    $fail++
} else {
    Write-Host "  ok   SMOKE-BRANCH present"
}
Check 'PLUGIN-CREATE' 'true'
Check 'PLUGIN-SHOW'   'true'
Check 'PLUGIN-TAB'    'true'
if ($out -match 'PLUGIN-ERR:(.*)') {
    Write-Host "  FAIL PLUGIN-ERR: $($Matches[1])"; $fail++
}
if ($out -match 'PLUGIN-REG-ERR:(.*)') {
    Write-Host "  (内置插件注册提示: $($Matches[1]))"
}

if ($fail -eq 0) { Write-Host '== IDE-PLUGIN OK =='; exit 0 }
Write-Host "== IDE-PLUGIN FAIL: $fail =="
exit 1
