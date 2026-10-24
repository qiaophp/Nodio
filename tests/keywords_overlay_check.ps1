# string.keywords x64 补版回归 —— 锁住「按官方同名路径补 Nodio 自己实现」这一批
#
# 官方 lib/string/keywords/_.aardio 把 32 位 keywords.dll 用 $ 内嵌进源码再内存加载，
# x64 进程装不下该 PE（官方报错「无法加载 DLL: (内嵌的内存映像)」），
# 官方也**没有随库发布 keywords.dll 的源码**（无法字节级复刻）。
#
# Nodio 侧按官方同名同路径提供自己的版本（overlay 目录优先于官方库）：
#   lib/string/keywords/_.aardio          官方逐字副本，只换 DLL 来源
#   lib/string/keywords/.res/keywords64.c Nodio 自写实现（源码入库，可复现）
#   lib/string/keywords/.res/keywords64.dll 上面源码用 MSVC 编出的 x64 DLL
#
# 导出的 5 个符号与官方 DLL 同名同签名同调用约定（cdecl）：
#   findOne / findAll / anyMatchLines / allMatchLines / noneMatchLines
#
# 下面的期望值全部来自**官方 32 位 DLL 的实测**（x86 构建走官方库，x64 构建走 overlay），
# 两个架构必须给出完全一样的结果。
#
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），所以本回归在两个架构上都能跑。
#
# 用法: powershell -File keywords_overlay_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot   = Split-Path $PSScriptRoot -Parent
$overlayDir = Join-Path $repoRoot 'lib\string\keywords'
# overlay 模块已迁移为 .nodio 扩展名（.aardio 保留兜底，旧检出兼容）
$overlayMod = if (Test-Path (Join-Path $overlayDir '_.nodio')) { Join-Path $overlayDir '_.nodio' } else { Join-Path $overlayDir '_.aardio' }
$overlaySrc = Join-Path $overlayDir '.res\keywords64.c'
$overlayDll = Join-Path $overlayDir '.res\keywords64.dll'

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}

$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("kw_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
    [IO.File]::WriteAllText($tmp, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$tmp`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $proc = [Diagnostics.Process]::Start($psi)
    $o = $proc.StandardOutput.ReadToEnd()
    $e = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    Remove-Item $tmp -ErrorAction SilentlyContinue
    return @{ out = $o; err = $e; code = $proc.ExitCode }
}

$fail = 0
function Check([string]$name, [string]$expect, [string]$actual) {
    if ($actual -eq $expect) { "  ok   $name = $actual" }
    else { "  FAIL $name = '$actual' (期望 '$expect')"; $script:fail++ }
}

# ---- 1. overlay 三个文件必须在库里（源码入库，保证可复现）----
foreach ($f in @($overlayMod, $overlaySrc, $overlayDll)) {
    if (Test-Path $f) { "  ok   overlay 文件存在 $(Split-Path $f -Leaf)" }
    else { "  FAIL overlay 文件缺失 $f"; $script:fail++ }
}
if (Test-Path $overlayDll) {
    Check 'overlay DLL 架构(0x8664)' '34404' ([string](Get-PEMachine $overlayDll))
}

# ---- 2. 语义黄金样本（期望值来自官方 32 位 DLL 实测）----
$probe = @'
import string.keywords;

function dump(v){
    if(type(v) == "table"){
        if(#v == 0) return "[]";
        var s = "";
        for(k,val in v) s = s ++ "[" ++ k ++ ":" ++ #val ++ "]";
        return s;
    }
    return tostring(v);
}

var kw = string.keywords({"alpha","beta","gamma"});
io.print("ONE1 ", kw.findOne("gamma and alpha"));
io.print("ONE2 ", kw.findOne("alpha and gamma"));
io.print("ONE3 ", kw.findOne("zzz"));
io.print("ONE4 ", kw.findOne(""));
io.print("ALL1 ", kw.findAll("alpha beta gamma"));
io.print("ALL2 ", kw.findAll("alpha beta"));
io.print("ALL3 ", kw.findAll(""));

var kw2 = string.keywords({"alpha","beta"});
var data = 'line one alpha\r\nline two beta\r\nline three\r\nline four alpha and beta\r\nlast';
io.print("ANY  ", dump(kw2.anyMatchLines(data)));
io.print("ALLL ", dump(kw2.allMatchLines(data)));
io.print("NONE ", dump(kw2.noneMatchLines(data)));

var kwE = string.keywords({});
io.print("E1 ", kwE.findOne("abc"));
io.print("E2 ", kwE.findAll("abc"));
io.print("E3 ", dump(kwE.anyMatchLines("a\r\nb")));
io.print("E4 ", dump(kwE.allMatchLines("a\r\nb")));
io.print("E5 ", dump(kwE.noneMatchLines("a\r\nb")));

var kwX = string.keywords({"x"});
var kwZ = string.keywords({"zzz"});
io.print("L1 ", dump(kwX.anyMatchLines('ax\ry\rcz')));
io.print("L2 ", dump(kwZ.noneMatchLines('abc\r')));
io.print("L3 ", dump(kwZ.noneMatchLines('a\r\r')));
io.print("L4 ", dump(kwZ.noneMatchLines('a\r\r\r')));
io.print("L5 ", dump(kwZ.noneMatchLines('\r')));
io.print("L6 ", dump(kwZ.noneMatchLines('a\r\n\r\nb')));
io.print("L7 ", dump(kwZ.noneMatchLines('a\n\rb\n')));
io.print("L8 ", dump(kwZ.noneMatchLines('')));
io.print("L9 ", dump(kwZ.noneMatchLines('a\r\nb\r\nc')));

var crKw = raw.tostring(raw.buffer({byte items[]={120,13}}),1,2);
var kwCR = string.keywords({crKw});
io.print("C1 ", kwCR.findOne('x\r\n'));
io.print("C2 ", dump(kwCR.anyMatchLines('x\r\n')));
io.print("C3 ", dump(kwCR.anyMatchLines('x\r')));

var bin = raw.buffer({byte items[]={1,2,0,3}});
var kwB = string.keywords({raw.tostring(bin,1,2)});
io.print("B1 ", kwB.findOne(bin));
io.print("B2 ", kwB.findAll(bin));
io.print("B3 ", kwB.findAll(raw.buffer({byte items[]={9,9}})));

io.print("S1 ", string.keywords({"Alpha","beta"}).findOne("alpha beta"));
io.print("P1 ", string.keywords("aa|bb|cc").findOne("zz cc"));
io.print("D1 ", string.keywords({"aa","aa","bb"}).findOne("bb aa"));
io.print("G1 ", string.keywords({"abcdefghij"}).findOne("abc"));
io.print("G2 ", string.keywords({"abcdefghij"}).findAll("abc"));
'@

$r = Run-Aardio $probe
if ($r.err) { "  NOTE stderr: $($r.err.Trim())" }

$map = @{}
foreach ($ln in ($r.out -split "`r?`n")) {
    if ($ln -match '^(\S+)\s+(.*?)\s*$') { $map[$Matches[1]] = $Matches[2] }
}

$expected = [ordered]@{
    # findOne：返回「在数据里出现位置最靠前」的关键字（1 基），位置相同取下标小的
    'ONE1' = '3';   # "gamma and alpha" -> gamma 在数据里更靠前
    'ONE2' = '1';   # "alpha and gamma" -> alpha
    'ONE3' = '0';   # 无命中
    'ONE4' = '0';   # 空数据
    # findAll：全部命中 -> true；空数据 -> true
    'ALL1' = 'true'
    'ALL2' = 'false'
    'ALL3' = 'true'
    # 行匹配：dump 打的是 行号:长度
    'ANY'  = '[1:14][2:13][3:24]'
    'ALLL' = '[1:24]'
    'NONE' = '[1:10][2:4]'
    # 空关键字表：findAll -> true；三个 *MatchLines 一行都不回调
    'E1' = '0'
    'E2' = 'true'
    'E3' = '[]'
    'E4' = '[]'
    'E5' = '[]'
    # 切行规则：只按 \n 切；报告长度去掉行末所有 \r；结尾 \n 不产生空行
    'L1' = '[1:7]'      # 'ax\ry\rcz' -> 1 行，孤立 \r 不分行
    'L2' = '[1:3]'      # 'abc\r'   -> 1 行，行末 \r 去掉
    'L3' = '[1:1]'      # 'a\r\r'   -> 行末两个 \r 都去掉
    'L4' = '[1:1]'      # 'a\r\r\r'
    'L5' = '[1:0]'      # '\r'      -> 1 个空行
    'L6' = '[1:1][2:0][3:1]'   # 'a\r\n\r\nb' -> 3 行，中间空行保留
    'L7' = '[1:1][2:2]'         # 'a\n\rb\n'  -> 行内 \r 保留
    'L8' = '[]'                 # 空数据 -> 0 行
    'L9' = '[1:1][2:1][3:1]'   # 'a\r\nb\r\nc'
    # 关键字含 \r：匹配用原始行，报告长度去掉 \r
    'C1' = '1'
    'C2' = '[1:1]'
    'C3' = '[1:1]'
    # 二进制安全（关键字含 0x00 之前的字节）
    'B1' = '1'
    'B2' = 'true'
    'B3' = 'false'
    # 大小写敏感 / 管道分隔 / 重复关键字 / 关键字比数据长
    'S1' = '2'
    'P1' = '3'
    'D1' = '3'
    'G1' = '0'
    'G2' = 'false'
}
foreach ($k in $expected.Keys) {
    $got = if ($map.ContainsKey($k)) { $map[$k] } else { '<缺失>' }
    Check $k $expected[$k] $got
}

# ---- 3. 模块解析路径：x64 必须命中 Nodio overlay，x86 必须走官方库 ----
$r2 = Run-Aardio 'import string.keywords; io.print("LIBPATH " + io.libpath("string.keywords"));'
$libPath = ''
foreach ($ln in ($r2.out -split "`r?`n")) {
    if ($ln -match '^LIBPATH\s+(.*?)\s*$') { $libPath = $Matches[1] }
}
$expectOverlay = $overlayMod
if ($isX64) {
    Check 'x64 命中 Nodio overlay' $expectOverlay $libPath
}
else {
    if ($libPath -and ($libPath -ne $expectOverlay)) { "  ok   x86 走官方库 ($libPath)" }
    else { "  FAIL x86 未走官方库: '$libPath'"; $script:fail++ }
}

if ($fail -eq 0) { "KEYWORDS-OVERLAY-OK"; exit 0 }
"KEYWORDS-OVERLAY-FAIL ($fail)"; exit 1
