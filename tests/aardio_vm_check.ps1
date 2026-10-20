# 官方 "=aardio" 运行期解释器：语义表与跳转模型回归
#
# 背景（docs/aardio-bytecode.md §7）：
#   · 官方运行期记录解释器本体 = FUN_004C4BC0（aardio.exe image VA 0x004C4BC0，
#     函数体 7263 字节），是整个二进制里唯一一处对 32 位记录做
#     `switch (word & 0x3F)` 的函数，case 覆盖 0x00..0x3C 全部 61 种；
#   · 记录字位布局、16 字节槽位布局、8 种类型标签、VM 状态与调用帧布局
#     全部实测；61 种记录的语义已逐条读出；
#   · 跳转类记录用 bits 14..31 作一个 18 位有符号相对位移，但**位移来源与
#     偏置逐 kind 不同**：
#       0x22 / 0x2D / 0x2E  取本条记录，偏置 0x1FFFE
#       0x23..0x26         取下一条 u32，偏置 0x1FFFE
#       0x27 / 0x28        取下一条 u32，偏置 0x1FFFD
#     由此「条件跳转落点」必须与「紧跟的 0x22 落点」相同。
#
# 本脚本验证：
#   1) tools/aardio_vm_runtime.py 自检通过（61 种记录、kind 连续 0x00..0x3C）；
#   2) 在官方唯一样本上，条件跳转 / 0x22 配对**全部落点一致**（228 对 / 0 不一致）；
#   3) tools/aardio_vm_tables.py 增加的运行期小节存在且退出码为 0；
#   4) tools/aardio_opcodes.py 按**运行期**位布局解码，输出语义名；
#   5) tools/aardio_bytecode.py --verify 的字节级回归仍然成立。
#
# 本脚本不依赖 nodio.exe，只跑 Python 工具，因此可以在编译被占用时照常执行。
#
# 用法: powershell -File aardio_vm_check.ps1 [-Sample <aifix.aardio>]
param([string]$Sample = 'D:\aardio\lib\ide\aifix.aardio',
      [string]$ToolsDir = 'D:\Nodio\tools')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Output ("  OK   " + $name) }
    else { Write-Output ("  FAIL " + $name + $(if ($detail) { "  -- " + $detail } else { "" })); $script:fail++ }
}

function RunPy([string]$script, [string[]]$argList = @()) {
    $out = & python (Join-Path $ToolsDir $script) @argList 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}

Write-Output "=== AARDIO-VM-CHECK ==="

# 1) 语义表自检
$r = RunPy 'aardio_vm_runtime.py'
Check 'aardio_vm_runtime.py 退出码为 0' ($r.Code -eq 0) $r.Out
Check '识别出 61 种合法记录种类' ($r.Out -match '合法记录种类: 61 \(0x00\.\.0x3C\)') $r.Out

# 2) 官方样本上的跳转模型自检
if (Test-Path $Sample) {
    $r2 = RunPy 'aardio_vm_runtime.py' @($Sample)
    Check '样本跳转模型自检退出码为 0' ($r2.Code -eq 0) $r2.Out
    if ($r2.Out -match '条件跳转/0x22 配对: (\d+) 对，落点不一致: (\d+)') {
        $pairs = [int]$matches[1]; $bad = [int]$matches[2]
        Check ("条件跳转/0x22 配对数 > 0（实际 $pairs）") ($pairs -gt 0) $r2.Out
        Check ("配对落点全部一致（不一致 $bad 处）") ($bad -eq 0) $r2.Out
    } else {
        Check '输出包含跳转模型自检结果' $false $r2.Out
    }

    # 3) 反汇编器按运行期位布局解码
    $r3 = RunPy 'aardio_opcodes.py' @($Sample)
    Check 'aardio_opcodes.py 退出码为 0' ($r3.Code -eq 0) $r3.Out
    Check '反汇编输出语义名而非 opXX' ($r3.Out -match 'GET_MEMBER2|LOAD_SELF_NS') $r3.Out
    Check '反汇编输出 JMP 的落点' ($r3.Out -match 'JMP\s+.*rel18=\d+\(own\)->\d+') $r3.Out

    # 4) 容器层字节级回归仍然成立
    $r4 = RunPy 'aardio_bytecode.py' @($Sample, '--verify')
    Check 'aardio_bytecode.py --verify 退出码为 0' ($r4.Code -eq 0) $r4.Out
    Check '重序列化与原文件逐字节相同' ($r4.Out -match 'identical=True') $r4.Out
} else {
    Write-Output "  SKIP 官方样本不存在: $Sample（跳过样本级验证）"
}

# 5) 内部表工具的运行期小节
$r5 = RunPy 'aardio_vm_tables.py'
Check 'aardio_vm_tables.py 退出码为 0' ($r5.Code -eq 0) $r5.Out
Check '含运行期解释器小节' ($r5.Out -match 'runtime record interpreter \(FUN_004C4BC0\)') $r5.Out
Check '含 61 条记录语义' ($r5.Out -match '0x3C CALL_N') $r5.Out

if ($fail -gt 0) {
    Write-Output "AARDIO-VM-CHECK FAILED ($fail)"
    exit 1
}
Write-Output "AARDIO-VM-CHECK OK"
exit 0
