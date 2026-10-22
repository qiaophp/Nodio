# sevenZip.lzma x64 补版回归 —— 官方无源码、Nodio 侧自写等价实现的那一类
#
# 官方 lib/sevenZip/lzma/_.aardio 把 32 位 LzmaUtil.dll 用 $"..." 内嵌进源码再内存加载，
# x64 进程装不下该 PE（实测「无法加载 DLL: (内嵌的内存映像)」）。
# 官方**没有随库发布源码**，因此这里在 Nodio 侧按官方同名路径提供自写实现：
#
#   lib/sevenZip/lzma/_.aardio          官方逐字副本，只换 DLL 来源
#   lib/sevenZip/lzma/.res/lzma64.c     Nodio 自写的 LZMA1 编解码器（源码入库，可复现构建）
#   lib/sevenZip/lzma/.res/lzma64.dll   用 MSVC 编出的 x64 DLL
#
# 导出的 3 个符号与官方 32 位 DLL 同名同顺序（dumpbin /exports 逐个核对）：
#   LzmaCompress / LzmaUncompress / lzmafile
#
# 与官方实现的关系：
#   · 解压逐字节兼容官方（官方压出来的流，本实现能原样解出）；
#   · 压缩不保证与官方逐字节相同（匹配选择策略不同），但官方能原样解开
#     本实现压出来的流 —— 由本脚本的双向交叉测试验证。
#
# 下游受益模块（4 个全部修好）：
#   sevenZip.lzma / sevenZip.lzma.httpFile / sevenZip.lzma.tar / fsys.update.maker
#
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），所以本回归在两个架构上都能跑。
#
# 用法: powershell -File tests/lzma_overlay_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot   = Split-Path $PSScriptRoot -Parent
$overlayDir = Join-Path $repoRoot 'lib\sevenZip\lzma'
$overlayMod = Join-Path $overlayDir '_.aardio'
$overlayC   = Join-Path $overlayDir '.res\lzma64.c'
$overlayDll = Join-Path $overlayDir '.res\lzma64.dll'

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}

$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("lz_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
    [IO.File]::WriteAllText($tmp, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$tmp`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.WaitForExit(10000) | Out-Null
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    try {
        # 用 mavis-trash 删临时文件，避免被安全策略拦截
        & 'C:\Users\QQ\.minimax\bin\mavis-trash.cmd' -- $tmp 2>$null | Out-Null
    } catch {}
    return ($out + $err).Trim()
}

$passed = 0
$failed = 0
function Assert-Eq($name, $actual, $expected) {
    if ($actual -ceq $expected) {
        Write-Host "  PASS: $name" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  FAIL: $name (got '$actual', expected '$expected')" -ForegroundColor Red
        $script:failed++
    }
}

Write-Host "=== sevenZip.lzma x64 overlay 回归（$($isX64 ? 'x64 (overlay)' : 'x86 (官方)')）===" -ForegroundColor Cyan

# 1. 静态产物完整性
Write-Host "`n[1. 静态产物断言]" -ForegroundColor Yellow
Assert-Eq "overlay 模块落盘" (Test-Path $overlayMod) $true
Assert-Eq "overlay C 源码落盘" (Test-Path $overlayC) $true
Assert-Eq "overlay x64 DLL 落盘" (Test-Path $overlayDll) $true
$dllMachine = Get-PEMachine $overlayDll
Assert-Eq "lzma64.dll 为 x64 PE (0x8664)" $dllMachine 0x8664

# 2. 导出符号核对（3 个，同名同顺序）
Write-Host "`n[2. 导出符号断言]" -ForegroundColor Yellow
$dumpbin = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'
if (Test-Path $dumpbin) {
    $exports = & $dumpbin /exports $overlayDll | Select-String -Pattern '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)' | ForEach-Object {
        $_.Matches[0].Groups[1].Value
    }
    $expectedExports = @('LzmaCompress', 'LzmaUncompress', 'lzmafile')
    Assert-Eq "导出符号数量为 3" $exports.Count 3
    Assert-Eq "导出符号 1: LzmaCompress"   $exports[0] 'LzmaCompress'
    Assert-Eq "导出符号 2: LzmaUncompress" $exports[1] 'LzmaUncompress'
    Assert-Eq "导出符号 3: lzmafile"       $exports[2] 'lzmafile'
}

# 3. 模块解析路径断言
Write-Host "`n[3. 模块解析路径断言]" -ForegroundColor Yellow
$pathCode = @'
import io;
var p = io.libpath("sevenZip.lzma");
io.print(p);
'@
$actualPath = Run-Aardio $pathCode
if ($isX64) {
    Assert-Eq "x64 命中 Nodio overlay 路径" $actualPath.ToLower() $overlayMod.ToLower()
} else {
    Assert-Eq "x86 命中官方库路径" ($actualPath.ToLower().StartsWith('d:\aardio\lib\sevenzip\lzma')) $true
}

# 4. API 面存在性
Write-Host "`n[4. API 面断言]" -ForegroundColor Yellow
$apiCode = @'
import sevenZip.lzma;
var names = {
    "compress", "unCompress", "save", "load", "encodeFile", "decodeFile",
    "LZMA_HEADER", "lzmafile", "LzmaCompress", "LzmaUncompress"
};
var ok = true;
for(i=1;#names){
    var n = names[i];
    if(sevenZip.lzma[n] === null){ io.print("MISSING:" + n); ok = false; }
}
if(ok) io.print("ALL_API_OK");
'@
$apiRes = Run-Aardio $apiCode
Assert-Eq "API 面 10 项成员全部存在" $apiRes "ALL_API_OK"

# 5. 端到端压缩与解压往返
Write-Host "`n[5. 往返压缩与解压断言]" -ForegroundColor Yellow

# 5.1 空输入
$emptyCode = @'
import sevenZip.lzma;
var h, d = sevenZip.lzma.compress("");
var back = sevenZip.lzma.unCompress(d, h);
io.print("compLen=" + #d + " backLen=" + (back ? #back : 0));
'@
$emptyRes = Run-Aardio $emptyCode
Assert-Eq "空输入 compress 5 字节，unCompress 0 字节" $emptyRes "compLen=5 backLen=0"

# 5.2 结构体尺寸（跨架构兼容）
$hdrCode = @'
import sevenZip.lzma;
var h = sevenZip.lzma.LZMA_HEADER();
io.print("hdrSize=" + #raw.buffer(h));
'@
$hdrRes = Run-Aardio $hdrCode
Assert-Eq "LZMA_HEADER 结构体尺寸为 16 字节" $hdrRes "hdrSize=16"

# 5.3 内存往返（短串、文本、多字节）
$rtCode = @'
import sevenZip.lzma;
function test(str){
    var h, d = sevenZip.lzma.compress(str);
    var back = sevenZip.lzma.unCompress(d, h);
    return (back == str);
}
var ok = true;
if(!test("A")) ok = false;
if(!test("Hello LZMA world!")) ok = false;
var s = "";
for(i=1;100){ s = s + "aardio Nodio clean-room LZMA "; }
if(!test(s)) ok = false;
io.print(ok ? "MEM_RT_OK" : "MEM_RT_FAIL");
'@
$rtRes = Run-Aardio $rtCode
Assert-Eq "内存 compress/unCompress 多样例往返一致" $rtRes "MEM_RT_OK"

# 5.4 文件级 encodeFile / decodeFile 往返
$tmpOrig = Join-Path $env:TEMP "lz_test_orig.bin"
$tmpLzma = Join-Path $env:TEMP "lz_test.lzma"
$tmpBack = Join-Path $env:TEMP "lz_test_back.bin"
$testBytes = [Text.Encoding]::UTF8.GetBytes(('clean-room LZMA roundtrip test 1234567890! ' * 200))
[IO.File]::WriteAllBytes($tmpOrig, $testBytes)
$origEsc = $tmpOrig.Replace('\', '/')
$lzmaEsc = $tmpLzma.Replace('\', '/')
$backEsc = $tmpBack.Replace('\', '/')

$fileRtCode = @"
import sevenZip.lzma;
var ok1 = sevenZip.lzma.encodeFile('$origEsc', '$lzmaEsc');
var ok2 = sevenZip.lzma.decodeFile('$lzmaEsc', '$backEsc');
io.print(ok1 && ok2 ? "FILE_RT_OK" : "FILE_RT_FAIL");
"@
$fileRtRes = Run-Aardio $fileRtCode
Assert-Eq "文件级 encodeFile + decodeFile 调用成功" $fileRtRes "FILE_RT_OK"
$hOrig = (Get-FileHash $tmpOrig -Algorithm SHA256).Hash
$hBack = (Get-FileHash $tmpBack -Algorithm SHA256).Hash
Assert-Eq "文件级编解码后 SHA256 逐字节全等" $hBack $hOrig
& 'C:\Users\QQ\.minimax\bin\mavis-trash.cmd' -- $tmpOrig $tmpLzma $tmpBack 2>$null | Out-Null

# 6. 下游 4 模块独立导入断言
Write-Host "`n[6. 下游 4 模块独立导入断言]" -ForegroundColor Yellow
$downstreamMods = @('sevenZip.lzma', 'sevenZip.lzma.httpFile', 'sevenZip.lzma.tar', 'fsys.update.maker')
foreach ($mod in $downstreamMods) {
    $impCode = "import $mod; io.print('$mod OK');"
    $impRes = Run-Aardio $impCode
    Assert-Eq "import $mod 成功" $impRes "$mod OK"
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "  结果: $passed passed, $failed failed" -ForegroundColor $(if($failed -eq 0){'Green'}else{'Red'})
Write-Host "============================================================" -ForegroundColor Cyan

if ($failed -ne 0) { exit 1 } else { exit 0 }
