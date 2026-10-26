# DAP 表达式求值回归锁（nodio-dap evaluate → 引擎求值）。
#
# 无头：直接跑 tests\dap_eval_check.py（Python 写的 DAP 客户端），
# 断言逐条 PASS。
#
#   pwsh -File tests/ide_debug_check.ps1 [-Exe <nodio.exe>]
#
# ⚠ **Python 是硬依赖**：DAP 是 Content-Length 分帧的 JSON-RPC，
#   PowerShell 里读写分帧流太啰嗦（用 Python 客户端，见 ai_mock_server.py
#   的先例）。没有 Python 就**明确报错**，不做静默降级 ——
#   静默降级 = 锁不住东西，却看起来是绿的。
param(
    [string]$Exe    = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$script = Join-Path $PSScriptRoot 'dap_eval_check.py'
if (-not (Test-Path $script)) { "SCRIPT NOT FOUND: $script"; exit 2 }

# 找 python：先 PATH，再固定路径（与 tests/run_vm_tests.ps1 一致）
$pyCmd = Get-Command 'python' -ErrorAction SilentlyContinue
$py = ''
if ($pyCmd) {
    if ($pyCmd -is [System.Management.Automation.ApplicationInfo]) { $py = $pyCmd.Path }
    else { $py = 'python' }
}
if (-not $py -and (Test-Path 'C:\Python311\python.exe')) { $py = 'C:\Python311\python.exe' }
if (-not $py) {
    Write-Output 'FAIL: python not found (neither PATH nor C:\Python311\python.exe) - the DAP evaluate regression needs it'
    exit 1
}

$out = & $py $script $Exe 2>&1 | Out-String
$code = $LASTEXITCODE
Write-Output $out

$fail = 0
foreach ($l in ($out -split "`r?`n")) {
    if ($l -match '^FAIL ') { Write-Host "  FAIL $l"; $fail++ }
}
$pass = @($out -split "`r?`n" | Where-Object { $_ -match '^PASS ' }).Count

if ($out -notmatch '== DAP EVAL OK ==') {
    Write-Host '  FAIL 缺少收尾标记 ''== DAP EVAL OK =='''
    $fail++
}

# 第五十九轮：条件/命中次数断点协议锁（dap_bp_check.py）
$script2 = Join-Path $PSScriptRoot 'dap_bp_check.py'
if (Test-Path $script2) {
    $out2 = & $py $script2 $Exe 2>&1 | Out-String
    $code2 = $LASTEXITCODE
    Write-Output $out2
    foreach ($l in ($out2 -split "`r?`n")) {
        if ($l -match '^FAIL ') { Write-Host "  FAIL $l"; $fail++ }
    }
    $pass += @($out2 -split "`r?`n" | Where-Object { $_ -match '^PASS ' }).Count
    if ($out2 -notmatch '== DAP BP OK ==') {
        Write-Host '  FAIL 缺少收尾标记 ''== DAP BP OK =='''
        $fail++
    }
    if ($code2 -ne 0) { $fail++ }
} else {
    Write-Host '  FAIL 缺少 dap_bp_check.py（条件断点回归锁）'
    $fail++
}

if ($fail -eq 0 -and $code -eq 0) {
    Write-Host "== IDE-DEBUG OK ($pass 项) =="
    exit 0
}
Write-Host "== IDE-DEBUG FAIL: $fail =="
exit 1
