# run_editor_tests.ps1 — 编辑器组件（nodio.ui.editor）的行为回归入口。
#
#   pwsh -NoProfile -File D:\Nodio\tests\run_editor_tests.ps1
#
# 跑三个探针，都必须打出 PROBE-OK：
#   editor_probe.aardio     —— 文本模型 / 缩进 / 注释 / 查找 / 撤销 / 脏标记 / 诊断波浪线
#   editor_fold_probe.aardio —— 代码折叠（识别 / 收起 / 文本不丢 / 折叠中定位 / 打字后保持）
#   editor_comp_probe.aardio —— 代码补全（前缀计算 / 浮层 / 上下移动 / 滚动 / 插入 / 关闭）
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
    'D:\Nodio\tests\editor_probe.aardio',
    'D:\Nodio\tests\editor_fold_probe.aardio',
    'D:\Nodio\tests\editor_comp_probe.aardio'
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

    if ($text -match 'PROBE-OK') {
        Write-Output "ok: $name"
    } else {
        $failed++
        Write-Output "FAIL: $name (no PROBE-OK)"
        if ($text.Trim()) { $text.TrimEnd() -split "`r?`n" | ForEach-Object { Write-Output ("   " + $_) } }
        if ($errText.Trim()) { Write-Output ("   stderr: " + $errText.Trim()) }
    }
}

if ($failed -gt 0) {
    Write-Output "EDITOR-TESTS-FAIL ($failed)"
    exit 1
}
Write-Output 'EDITOR-TESTS-OK'
exit 0
