# 全量回归跑批（18 项）
#
# 用法:
#   pwsh -File tests\run_regression.ps1 -Exe D:\Nodio\build-x64\Release\nodio.exe -Arch x64
#
# 与 %TEMP%\nodio_regress.ps1 的差别：
#   18 项**全部**显式传 -Exe。旧脚本对 vm / builtin / import 三项没传，
#   它们会落到各自的默认路径（D:\Nodio\build-x64\Release\nodio.exe），
#   于是「想测 A 二进制、实际测了 B 二进制」——A/B 对比时结论直接作废。
#
# 注意（宿主环境约束，2026-10-24 实测）:
#   宿主装了火绒安全（HipsDaemon），它按**程序路径**拦截写入：
#   从 build-x64\Release 之外的目录启动 nodio.exe 时，写 %TEMP% 与
#   %USERPROFILE% 会被拒（ERROR_ACCESS_DENIED），读却正常 —— 表现为
#   io.writeFile 返回 null、fsys.createDir 返回 false、t30/t59/iofile
#   等写盘用例整片变红，但引擎本身无 bug。故 Exe 建议用仓库的正规构建
#   目录（build-x64 / build-x86）。
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

# overlay 库目录（Nodio 自己的 lib/）。
#
# ⚠ **x86 必须显式设置**：engine/import.cpp:38-43 里 32 位进程只认
#   NODIO_OVERLAY_LIBDIR 环境变量，**没有** 64 位那条 D:\Nodio\lib 硬编码
#   回退（GetOverlayLibDir 的 else 分支只在 _WIN64 下编译）。
#   不设 → vm 套件里依赖 overlay 的用例（designer 等）拿不到库，
#   会报 "designer overlay lib unavailable" 并连带 2 项 failed ——
#   看着像引擎回归，其实是环境没给。
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'

$tests = @(
    @{ name = 'vm';          script = 'run_vm_tests.ps1';          args = @('-Exe', $Exe) },
    @{ name = 'ide_smoke';   script = 'ide_smoke.ps1';             args = @('-Exe', $Exe) },
    @{ name = 'ide_ctx';     script = 'ide_ctx_smoke.ps1';         args = @('-Exe', $Exe) },
    @{ name = 'ide_nav';     script = 'ide_nav_smoke.ps1';         args = @('-Exe', $Exe) },
    @{ name = 'ide_cmd';     script = 'ide_command_check.ps1';     args = @('-Exe', $Exe) },
    @{ name = 'ide_plugin';  script = 'ide_plugin_check.ps1';      args = @('-Exe', $Exe) },
    @{ name = 'ide_settings';script = 'ide_settings_check.ps1';    args = @('-Exe', $Exe) },
    @{ name = 'ide_ai';      script = 'ide_ai_check.ps1';          args = @('-Exe', $Exe) },
    @{ name = 'ide_font';    script = 'ide_font_check.ps1';        args = @('-Exe', $Exe) },
    @{ name = 'ide_complete';script = 'ide_completion_check.ps1';   args = @('-Exe', $Exe) },
    @{ name = 'ide_threadgc';script = 'ide_thread_gc_check.ps1';   args = @('-Exe', $Exe) },
    @{ name = 'editor';      script = 'run_editor_tests.ps1';      args = @('-Exe', $Exe) },
    @{ name = 'input';       script = 'run_input_tests.ps1';       args = @('-Exe', $Exe) },
    @{ name = 'iofile';      script = 'iofile_text_check.ps1';     args = @('-Exe', $Exe) },
    @{ name = 'winclip';     script = 'win_clip_check.ps1';        args = @('-Exe', $Exe) },
    @{ name = 'builtin';     script = 'builtin_api_check.ps1';     args = @('-Exe', $Exe) },
    @{ name = 'stdlib';      script = 'run_stdlib_tests.ps1';      args = @('-Exe', $Exe) },
    @{ name = 'publish';     script = 'run_publish_tests.ps1';     args = @('-Exe', $Exe) },
    @{ name = 'import';      script = 'import_contract_check.ps1'; args = @('-Exe', $Exe) }
)

$results = @()
foreach ($t in $tests) {
    $path = Join-Path 'D:\Nodio\tests' $t.script
    if (-not (Test-Path $path)) {
        $results += [pscustomobject]@{ name=$t.name; code=-1; sec=0; tail='MISSING SCRIPT' }
        continue
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = & pwsh -NoProfile -File $path @($t.args) 2>&1 | Out-String
    $code = $LASTEXITCODE
    $sw.Stop()
    $tail = ($out -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last 2) -join ' | '
    if ($tail.Length -gt 220) { $tail = $tail.Substring($tail.Length - 220) }
    $results += [pscustomobject]@{ name=$t.name; code=$code; sec=[int]$sw.Elapsed.TotalSeconds; tail=$tail }
    Write-Host ("[{0}] {1} exit={2} {3}s" -f $Arch, $t.name, $code, [int]$sw.Elapsed.TotalSeconds)
}

Write-Host ''
Write-Host "===== SUMMARY ($Arch) ====="
foreach ($r in $results) {
    $tag = if ($r.code -eq 0) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0,-4} {1,-12} exit={2}" -f $tag, $r.name, $r.code)
    if ($r.tail) { Write-Host ("      {0}" -f $r.tail) }
}
$bad = @($results | Where-Object { $_.code -ne 0 })
Write-Host ''
Write-Host ("TOTAL={0} PASS={1} FAIL={2}" -f $results.Count, ($results.Count - $bad.Count), $bad.Count)
if ($bad.Count -gt 0) { exit 1 }
exit 0
