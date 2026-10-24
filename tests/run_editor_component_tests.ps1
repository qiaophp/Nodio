# run_editor_component_tests.ps1 — 编辑器组件级回归（IDE 里的编辑器，不含 IDE 装配）
#
#   pwsh -NoProfile -File D:\Nodio\tests\run_editor_component_tests.ps1
#
# 跑两个回归用例，都必须打出各自的 OK 标记：
#   reg_editor_input.aardio — 输入行为：回车续接缩进、括号/引号配对、闭合跳过、
#                             撤销/重做、Ctrl+Z/Ctrl+Y 快捷键、Ctrl+/ 注释保留缩进
#   reg_hover.aardio        — 悬停提示：算出 (line,col)、弹浮层、剥 markdown、
#                             空白处不打扰、收起
#
# 探针必须用 Start-Process + 重定向输出跑：直接跑会挂在消息循环里。
param(
    [string]$Exe    = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$LibDir = 'D:\aardio\lib',
    [int]   $TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }

# 用例 = @{ 脚本; 期望标记 }
$cases = @(
    @{ script = 'D:\Nodio\tests\reg_editor_input.aardio'; mark = 'REG-EDITOR-INPUT-OK' },
    @{ script = 'D:\Nodio\tests\reg_hover.aardio';        mark = 'REG-HOVER-OK' }
)

$env:NODIO_LIBDIR        = $LibDir
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_PAUSE         = '0'

$failed = 0
foreach ($c in $cases) {
    $script = $c.script
    $mark   = $c.mark
    if (-not (Test-Path $script)) { Write-Output "FAIL: case not found: $script"; $failed++; continue }
    $name = [IO.Path]::GetFileNameWithoutExtension($script)
    $out  = Join-Path $env:TEMP ("nodio_{0}_{1}.out" -f $name, $PID)
    $err  = "$out.err"

    $p = Start-Process -FilePath $Exe -ArgumentList $script -PassThru -NoNewWindow `
            -RedirectStandardOutput $out -RedirectStandardError $err
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        Write-Output "FAIL: $name timed out after ${TimeoutSec}s"
        $failed++
        continue
    }

    $text = if (Test-Path $out) { [IO.File]::ReadAllText($out, [Text.Encoding]::UTF8) } else { '' }
    $errText = if (Test-Path $err) { [IO.File]::ReadAllText($err, [Text.Encoding]::UTF8) } else { '' }

    if ($text -match [regex]::Escape($mark) -and $text -notmatch 'FAIL ' -and $errText -notmatch 'FATAL CRASH') {
        Write-Output "ok: $name"
    } else {
        $failed++
        Write-Output "FAIL: $name"
    }
    if ($text.Trim()) { $text.TrimEnd() -split "`r?`n" | ForEach-Object { Write-Output ("   " + $_) } }
    if ($errText.Trim()) { Write-Output ("   stderr: " + $errText.Trim()) }
}

if ($failed -gt 0) {
    Write-Output "EDITOR-COMPONENT-TESTS-FAIL ($failed)"
    exit 1
}
Write-Output 'EDITOR-COMPONENT-TESTS-OK'
exit 0
