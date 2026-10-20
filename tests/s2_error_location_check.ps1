# S-2 回归锁：运行期错误必须带「文件:行号」
#   · 官方 aardio 把 文件 / 错误行号 放在一张结构化错误报告里（aardio.exe 内置的
#     中英文字段表：错误/文件/错误行号/字段/失败操作 …），但那是 IDE 弹窗式报告，
#     不是一行能被脚本/LSP/CI 读走的文本；发布时关掉调试（dstrip="true"，官方文档
#     docs/guide/ide/file.md）就再也拿不到。
#     Nodio 把它放进消息本身，任何时候都在。
#   · 归属规则：报**定义帧**所在的文件 —— 模块 A 的函数被模块 B 调用 → 报 A。
#   · 官方兼容回退：确实拿不到文件时回退成裸行号 "@ 行 N"。
# 用法: pwsh -File tests/s2_error_location_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dir = Join-Path $here 's2'
$pass = 0; $fail = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { $script:pass++; Write-Host "PASS $name" }
    else { $script:fail++; Write-Host "FAIL $name :: $detail" }
}

function Run-Nodio([string]$rawArgs, [hashtable]$envs) {
    # A file watcher / scanner can hold a script open for a moment; the
    # engine then reports "无法打开脚本文件".  That is an environment
    # hiccup, not a behaviour under test — retry instead of failing.
    for ($try = 1; $try -le 6; $try++) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
        $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
        $psi.Arguments = $rawArgs
        $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
        if ($LibDir) { $psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir }
        if ($OverlayDir) { $psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $OverlayDir }
        foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] }
        $p = [System.Diagnostics.Process]::Start($psi)
        $o = $p.StandardOutput.ReadToEndAsync()
        $e = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(20000)) { $p.Kill(); $p.WaitForExit(5000) }
        $text = $o.Result + $e.Result
        if ($text -notlike '*无法打开脚本文件*') {
            return @{ out = $text; code = $p.ExitCode }
        }
        Start-Sleep -Milliseconds 400
    }
    return @{ out = $text; code = $p.ExitCode }
}

# ---- 1. 被 catch 捕获的错误同样带位置，且归属「定义帧」 ----
$r = Run-Nodio '' @{ NODIO_RUN = (Join-Path $dir 'caught.nodio') }
$out = $r.out.Replace($dir, '<DIR>')
$lines = @($out -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })
$want = @(
    'A=运行错误: main boom @ <DIR>\caught.nodio:3',
    'B=运行错误: s2 module boom @ <DIR>\s2mod.aardio:4',
    'C=运行错误: cb boom @ <DIR>\caught.nodio:9'
)
$got = ($lines -join ' | ')
Check 'caught: 主脚本错误归属主脚本' ($lines.Count -ge 1 -and $lines[0] -eq $want[0]) $got
Check 'caught: 模块内错误归属模块文件' ($lines.Count -ge 2 -and $lines[1] -eq $want[1]) $got
Check 'caught: 回调按定义帧归属主脚本' ($lines.Count -ge 3 -and $lines[2] -eq $want[2]) $got
Check 'caught: 恰好三条' ($lines.Count -eq 3) $got

# ---- 2. 未捕获的错误：模块内的错误归属到模块文件，且位置只出现一次 ----
$r2 = Run-Nodio '' @{ NODIO_RUN = (Join-Path $dir 'uncaught.nopio') }
$out2 = $r2.out.Replace($dir, '<DIR>')
Check 'uncaught: 退出码非 0' ($r2.code -ne 0) "code=$($r2.code)"
Check 'uncaught: 归属到模块文件' ($out2.Contains('运行错误: s2 module boom @ <DIR>\s2mod.aardio:4')) $out2
Check 'uncaught: 位置标记只出现一次' (([regex]::Matches($out2, '@')).Count -eq 1) $out2

# ---- 3. 拿不到文件时回退成裸行号（官方兼容形态） ----
$r3 = Run-Nodio '-e "error(''boom'')"' @{}
$out3 = $r3.out
Check 'fallback: 无文件时用裸行号' ($out3.Contains('运行错误: boom @ 行 1')) $out3

Write-Host "== $pass passed, $fail failed =="
if ($fail -gt 0) { exit 1 }
exit 0
