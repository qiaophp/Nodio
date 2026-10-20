# IDE-layer right-click regression (round 5).
#
# Exercises the whole right-click chain on top of the official aardio
# contracts — no bespoke API:
#   · official win.ui.tracker  -> 0x7B dispatches onContextMenu(x,y) in
#     **screen** coordinates (D:\aardio\lib\win\ui\tracker.aardio:150-166)
#   · official win.ui.popmenu  -> popId() pops at the cursor
#   · official richedit        -> getSel/selText/setSel/getLength
#   · official treeview        -> hitTest(x,y,screen) / getSelection
#
# The fixture also pins the engine fix that the whole chain depends on:
# once the official wndproc is installed, pointer-valued SendMessage
# results must survive as full 64-bit handles (the official
# win/api.aardio declares CallWindowProc/DefWindowProc as `int`, which
# truncates an LRESULT on 64-bit).
#
#   powershell -File tests/ide_ctx_smoke.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe      = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script   = 'D:\Nodio\tests\ide_ctx\ctx.nodio',
    [string]$Expect   = 'D:\Nodio\tests\ide_ctx\ctx.expect',
    [string]$LibDir   = 'D:\aardio\lib',
    [string]$Overlay  = 'D:\Nodio\lib',
    [int]   $TimeoutSec = 60
)

if (-not (Test-Path $Exe))    { Write-Output "FAIL: exe not found: $Exe";    exit 1 }
if (-not (Test-Path $Script)) { Write-Output "FAIL: script not found: $Script"; exit 1 }
if (-not (Test-Path $Expect)) { Write-Output "FAIL: expect not found: $Expect"; exit 1 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_RUN'] = $Script
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
$psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $Overlay

$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); $p.WaitForExit(5000) }
$outRaw = ($outTask.Result + $errTask.Result) -replace "`r", ''
$outLines = @($outRaw -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
$wantLines = @((Get-Content $Expect -Encoding UTF8) | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })

# 剪贴板是全局系统资源：被同机其它进程占用时 OpenClipboard 返回 0，
# 官方 win.clip 的读写会静默失败。夹具会打印 EDIT-CLIP-SKIP，此时只把
# 与剪贴板相关的断言跳过（右键链路的断言一条都不放松）。
if ($outLines -contains 'EDIT-CLIP-SKIP') {
    Write-Output 'SKIP clipboard assertions (clipboard locked by another process)'
    $outLines = @($outLines | Where-Object { $_ -ne 'EDIT-CLIP-SKIP' })
    $wantLines = @($wantLines | Where-Object { $_ -notmatch '^(EDIT-CLIP|EDIT-CUT|EDIT-PASTE|EDIT-ALL) ' })
}

$ok = ($outLines.Count -eq $wantLines.Count)
if ($ok) {
    for ($i = 0; $i -lt $outLines.Count; $i++) {
        if ($outLines[$i] -ne $wantLines[$i]) { $ok = $false; break }
    }
}

if ($ok) {
    Write-Output 'CTX-SMOKE OK'
    exit 0
}
Write-Output 'FAIL: IDE right-click chain mismatch'
Write-Output ("   got : " + ($outLines -join '|'))
Write-Output ("   want: " + ($wantLines -join '|'))
exit 1
