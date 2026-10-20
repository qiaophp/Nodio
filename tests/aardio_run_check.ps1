# 官方 "=aardio" 预编译块的**记录 VM** 回归（S-1b 第二阶段）
#
# 背景（docs/aardio-bytecode.md §7.5.2）：
#   官方运行期没有 AST —— `=aardio` 预编译块就是「32 位 RISC 记录流 +
#   一个寄存器机解释器」（aardio.exe 的 FUN_004C4BC0，61 个 case）。
#   所以 Nodio 执行官方预编译块的路线不是「把记录流反推成树」，
#   而是**照官方做一个记录 VM**（engine/aardio_vm.cpp）。
#
# 官方 aardio 只能执行自己编译出来的块，**从不提供**「执行一个预编译块并
# 打印结果」的入口，官方样本 lib/ide/aifix.aardio 又依赖命名空间 / 闭包 /
# 原生函数，无法单独验证算术与分支语义 —— 因此本脚本用
#   tools/aardio_make_chunk.py
# 按**官方格式文法**合成最小预编译块来验证记录 VM 的语义。
#
# 本脚本验证：
#   1) 算术（3 + 4 = 7）、循环（1..10 求和 = 55）、位运算、字符串长度、
#      一元负号与真值 —— 五类语义各自的结果正确；
#   2) **左右操作数顺序**符合官方（SUB/DIV/SHL 的左操作数在 bits23..31）；
#   3) 成员访问族：模块命名空间成员、VM 暂存槽成员、对象/键/值三槽变体、
#      字符串下标、方法预备（METHOD_PREP）、数组构造（BUILD_ARRAY）；
#   4) 尚未实现的记录种类**明确报错**（种类 + 记录号），不静默跳过；
#   5) 合成块能被官方等价校验器（tools/aardio_validate.py）判为合法；
#   6) 官方真实样本（lib/ide/aifix.aardio）在记录 VM 上**优雅失败**
#      （报「尚未实现」而不是崩溃）。
#
# 用法: powershell -File aardio_run_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repo = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path $env:TEMP ("aardio_run_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

$fail = 0
function Expect([bool]$cond, [string]$label, [string]$got) {
    if ($cond) { Write-Host "PASS $label" }
    else { $script:fail++; Write-Host "FAIL $label got=[$got]" }
}

function Run-Chunk([string]$path) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "-runchunk `"$path`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(3000) }
    return @{ code = $p.ExitCode; text = (($o.Result + $e.Result) -replace "`r", '').Trim() }
}

# ---- 0) 合成夹具（用官方格式文法写出）----
$gen = Join-Path $repo 'tools\aardio_make_chunk.py'
if (-not (Test-Path $gen)) { "SKIP generator missing: $gen"; exit 0 }
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { "SKIP python not available"; exit 0 }
& $py.Source $gen $tmp | Out-Null
if ($LASTEXITCODE -ne 0) { "FAIL chunk generator"; exit 1 }

# ---- 1) 各语义的结果 ----
$cases = @(
    @{ name = 'add';    want = '7'  },
    @{ name = 'loop';   want = '55' },
    @{ name = 'bits';   want = '49' },
    @{ name = 'str';    want = '5'  },
    @{ name = 'neg';    want = '-7' }
)
foreach ($c in $cases) {
    $f = Join-Path $tmp ($c.name + '.aardio')
    if (-not (Test-Path $f)) { Expect $false "fixture $($c.name) generated" "missing"; continue }
    $r = Run-Chunk $f
    Expect ($r.code -eq 0) "run $($c.name) exits 0" $r.text
    Expect ($r.text -like "*结果: $($c.want)*") "run $($c.name) = $($c.want)" $r.text
}

# ---- 1b) 左右操作数顺序（第十五轮按官方反汇编修正）----
$order = @(
    @{ name = 'sub';  want = '7'  },   # 10 - 3
    @{ name = 'div';  want = '4.5' },  # 9 / 2
    @{ name = 'shl';  want = '16' }    # 1 << 4
)
foreach ($c in $order) {
    $f = Join-Path $tmp ($c.name + '.aardio')
    if (-not (Test-Path $f)) { Expect $false "fixture $($c.name) generated" "missing"; continue }
    $r = Run-Chunk $f
    Expect ($r.text -like "*结果: $($c.want)*") "operand order $($c.name) = $($c.want)" $r.text
}

# ---- 1c) 成员访问族（第十五轮）----
$member = @(
    @{ name = 'member';  want = '42'  },   # ns.answer = 42 → 读回
    @{ name = 'member2'; want = '7'   },   # scratch.k = 7 → 读回
    @{ name = 'member3'; want = '5'   },   # 对象在 A / 键在 B / 值在 C
    @{ name = 'index';   want = '101' },   # "hello" 的第 2 个字符 'e'
    @{ name = 'method';  want = '8'   },   # METHOD_PREP
    @{ name = 'array';   want = '2'   }    # BUILD_ARRAY 追加 2 个元素
)
foreach ($c in $member) {
    $f = Join-Path $tmp ($c.name + '.aardio')
    if (-not (Test-Path $f)) { Expect $false "fixture $($c.name) generated" "missing"; continue }
    $r = Run-Chunk $f
    Expect ($r.code -eq 0) "run $($c.name) exits 0" $r.text
    Expect ($r.text -like "*结果: $($c.want)*") "run $($c.name) = $($c.want)" $r.text
}

# ---- 1d) 调用帧与闭包（第十六轮）----
$call = @(
    @{ name = 'call';    want = '10' },   # NEW_CLOSURE + CALL：参数1 + 参数2
    @{ name = 'params';  want = '7'  },   # 被调帧槽 0 = self，槽 1.. = 显式参数
    @{ name = 'upvalue'; want = '99' }    # 上值按引用：闭包改写外层局部 10 → 99
)
foreach ($c in $call) {
    $f = Join-Path $tmp ($c.name + '.aardio')
    if (-not (Test-Path $f)) { Expect $false "fixture $($c.name) generated" "missing"; continue }
    $r = Run-Chunk $f
    Expect ($r.code -eq 0) "run $($c.name) exits 0" $r.text
    Expect ($r.text -like "*结果: $($c.want)*") "call frame $($c.name) = $($c.want)" $r.text
}

# ---- 2) 尚未实现的记录种类必须明确报错 ----
$r = Run-Chunk (Join-Path $tmp 'unsupported.aardio')
Expect ($r.code -ne 0) 'unsupported kind fails' $r.text
Expect ($r.text -like '*尚未在记录 VM 中实现*') 'unsupported kind reported' $r.text
Expect ($r.text -like '*记录号 1*') 'unsupported kind locates pc' $r.text

# ---- 3) 合成块必须能被官方等价校验器判为合法 ----
$val = Join-Path $repo 'tools\aardio_validate.py'
if (Test-Path $val) {
    $all = @($cases + $order + $member + $call) | ForEach-Object { $_.name }
    foreach ($n in $all) {
        $f = Join-Path $tmp ($n + '.aardio')
        $vout = & $py.Source $val $f 2>&1 | Out-String
        Expect ($vout -like '*-> OK*') "official-equivalent validator accepts $n" $vout
    }
} else {
    Write-Host 'SKIP validator script missing'
}

# ---- 4) 官方真实样本：优雅失败，不崩溃 ----
$sample = Join-Path $LibDir 'ide\aifix.aardio'
if (Test-Path $sample) {
    $r = Run-Chunk $sample
    Expect ($r.code -ne 0) 'official sample does not pretend to succeed' $r.text
    # 第十六轮：调用族落地后，官方样本的失败点前移到「调用的对象不是函数」
    #（记录 3 处调用的是官方内建函数，Nodio 还没有内建表）—— 仍须是**明确原因**。
    Expect ($r.text -like '*执行失败*') 'official sample fails with clear reason' $r.text
    Expect ($r.text -like '*位置: 记录号*') 'official sample reports the record position' $r.text
    Expect ($r.text -notlike '*访问冲突*' -and $r.text -notlike '*异常*') 'official sample no crash' $r.text
} else {
    Write-Host "SKIP official sample not found under $LibDir"
}

& 'C:\Users\QQ\.minimax\bin\mavis-trash.cmd' -- $tmp | Out-Null
if ($fail -gt 0) { "== aardio record VM FAILED ($fail) =="; exit 1 }
"== aardio record VM OK =="
exit 0
