# run_input_tests.ps1 — 键盘输入链路回归（引擎级）
#
#   pwsh -NoProfile -File D:\Nodio\tests\run_input_tests.ps1
#
# 跑两个探针，都必须打出 PROBE-OK：
#   probe_keysim.aardio — 按键消息（WM_KEYDOWN/WM_CHAR）发到各类控件不得崩
#                          （修复前：IsDialogMessageW 拿到 NULL 的 MSG → 进程崩）
#   probe_type.aardio   — 真实键盘输入（keybd_event）落到 richedit，不得崩
#
# 探针必须用 Start-Process + 重定向输出跑：直接跑会挂在消息循环里等输入。
param(
    [string]$Exe    = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$LibDir = 'D:\aardio\lib',
    [int]   $TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }

$probes = @(
    'D:\Nodio\tests\probe_keysim.aardio',
    'D:\Nodio\tests\probe_type.aardio'
)

$env:NODIO_LIBDIR        = $LibDir
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_PAUSE         = '0'

$failed = 0
foreach ($probe in $probes) {
    if (-not (Test-Path $probe)) { Write-Output "FAIL: probe not found: $probe"; $failed++; continue }
    $name = [IO.Path]::GetFileNameWithoutExtension($probe)
    $out  = Join-Path $env:TEMP ("nodio_{0}_{1}.out" -f $name, $PID)
    $err  = "$out.err"
    $env:NODIO_RUN = $probe

    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
            -RedirectStandardOutput $out -RedirectStandardError $err
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        Write-Output "FAIL: $name timed out after ${TimeoutSec}s"
        $failed++
        continue
    }

    $text = if (Test-Path $out) { [IO.File]::ReadAllText($out, [Text.Encoding]::UTF8) } else { '' }
    $errText = if (Test-Path $err) { [IO.File]::ReadAllText($err, [Text.Encoding]::UTF8) } else { '' }
    Remove-Item $out, $err -Force -ErrorAction SilentlyContinue

    if ($text -match 'PROBE-OK' -and $errText -notmatch 'FATAL CRASH') {
        Write-Output "ok: $name"
        if ($text.Trim()) { $text.TrimEnd() -split "`r?`n" | ForEach-Object { Write-Output ("   " + $_) } }
    } else {
        $failed++
        Write-Output "FAIL: $name"
        if ($text.Trim()) { $text.TrimEnd() -split "`r?`n" | ForEach-Object { Write-Output ("   " + $_) } }
        if ($errText.Trim()) { Write-Output ("   stderr: " + $errText.Trim()) }
    }
}

if ($failed -gt 0) {
    Write-Output "INPUT-TESTS-FAIL ($failed)"
    exit 1
}
Write-Output 'INPUT-TESTS-OK'
exit 0
