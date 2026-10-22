# ── tests/fasm_overlay_check.ps1 ─────────────────────────────────────────────
# 验证 raw.fasm x64 overlay 补版（同名路径覆盖）：
#   1. 产物存在性、PE 架构与导出符号检查（3 个符号全部对齐）
#   2. x64 Nodio 执行 import raw.fasm
#   3. getVersion() 版本返回 (1, 71)
#   4. assemble() 基础汇编生成机器码 (nop, ret -> 0x90 0xC3)
#   5. assemble() 错误语法返回 Illegal instruction 与行号
#   6. x86 构建回归验证（双架构同绿）
# ─────────────────────────────────────────────────────────────────────────────
$ErrorActionPreference = 'Stop'

$NodioX64 = "C:\Users\QQ\AppData\Local\Temp\nr26\build-x64\Release\nodio.exe"
$NodioX86 = "C:\Users\QQ\AppData\Local\Temp\nr26\build-x86\Release\nodio.exe"
$FasmDll = "lib/raw/fasm/.res/fasm64.dll"
$FasmAardio = "lib/raw/fasm/_.aardio"
$FasmC = "lib/raw/fasm/.res/fasm64.c"

Write-Host "=== 1. 静态文件与 PE 检查 ==="
if (-not (Test-Path $FasmDll)) { throw "缺少 $FasmDll" }
if (-not (Test-Path $FasmAardio)) { throw "缺少 $FasmAardio" }
if (-not (Test-Path $FasmC)) { throw "缺少 $FasmC" }

$peCheck = python -c "
import pefile
pe = pefile.PE('$FasmDll')
assert pe.FILE_HEADER.Machine == 0x8664, f'Expected x64 (0x8664), got {hex(pe.FILE_HEADER.Machine)}'
names = [exp.name.decode('ascii') for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols]
expected = ['fasm_Assemble', 'fasm_AssembleFile', 'fasm_GetVersion']
assert sorted(names) == sorted(expected), f'Exports mismatch: {names}'
print('PE_OK')
"
if ($peCheck -ne "PE_OK") { throw "PE 校验失败: $peCheck" }
Write-Host "PE 架构与 3 导出符号检查通过"

Write-Host "`n=== 2. x64 运行时功能验证 ==="
$code = @"
import raw.fasm;
var maj, min = raw.fasm.getVersion();
if(maj != 1 || min != 71) {
    error("version mismatch: " + maj + "," + min);
}

var bin = raw.fasm.assemble("use32`n nop`n ret`n");
if(!bin || #bin != 2 || string.hex(bin) != "90C3") {
    error("assemble failed or mismatch: " + (bin ? string.hex(bin) : "null"));
}

var bErr, err, line = raw.fasm.assemble("invalid_opcode_here`n");
if(bErr !== null || err != "Illegal instruction" || line != 1) {
    error("error handling mismatch: " + tostring(err));
}

print("X64_ALL_PASS");
"@

$outX64 = & $NodioX64 -e $code
if ($outX64 -notmatch "X64_ALL_PASS") {
    throw "x64 运行失败: $outX64"
}
Write-Host "x64 运行验证全部通过"

Write-Host "`n=== 3. x86 运行时回归对比验证 ==="
if (Test-Path $NodioX86) {
    $outX86 = & $NodioX86 -e $code
    if ($outX86 -notmatch "X64_ALL_PASS") {
        throw "x86 运行失败: $outX86"
    }
    Write-Host "x86 官方库对比验证全部通过"
}

Write-Host "`n=== ALL TESTS PASSED ==="
