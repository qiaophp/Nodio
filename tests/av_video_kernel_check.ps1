# av.video.kernel x64 补版回归 —— 官方随库发布 C 源码的那一类
#
# 官方 lib/av/video/kernel.aardio 把 32 位 kernel.dll 用 $"..." 内嵌进源码再内存加载，
# x64 进程装不下该 PE（实测「无法加载 DLL: (内嵌的内存映像)」）。
# 与 av.audio.dsp 不同的是：官方**随库发布了 C 源码**
# （D:\aardio\lib\av\video\.res\kernel.c，注释里写明用 tcc 编译成 32 位 DLL），
# 所以这里不需要自写实现 —— 直接用官方源码重新编译成 x64 DLL 即可。
#
# Nodio 侧按**官方同名路径**提供自己的版本（overlay 目录优先于官方库）：
#   lib/av/video/kernel.aardio          官方逐字副本，只换 DLL 来源
#   lib/av/video/.res/kernel64.dll      官方 .res/kernel.c 用 MSVC 编出的 x64 DLL
#
# 导出的 13 个符号与官方 32 位 DLL 同名同顺序（dumpbin /exports 逐个核对）：
#   v_bg / v_blend / v_blit / v_bounds / v_boxMean / v_clear / v_flickerScore
#   v_glitch2 / v_grain / v_lumaStats / v_motionCheck / v_over / v_resample
#
# 下面的期望值全部来自**官方 32 位 kernel.dll 的实测**（x86 构建走官方库，
# x64 构建走 Nodio overlay），两个架构必须给出完全一样的结果。
# 像素缓冲用「加权和 + 位置加权和」双校验和回传（避开 io.print 的 CRLF 转换）。
#
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），所以本回归在两个架构上都能跑。
#
# 用法: powershell -File av_video_kernel_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot   = Split-Path $PSScriptRoot -Parent
$overlayDir = Join-Path $repoRoot 'lib\av\video'
$overlayMod = Join-Path $overlayDir 'kernel.aardio'
$overlayDll = Join-Path $overlayDir '.res\kernel64.dll'

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}

$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("vk_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
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

# ---- 1. overlay 文件必须在库里 ----
foreach ($f in @($overlayMod, $overlayDll)) {
    if (Test-Path $f) { "  ok   overlay 文件存在 $(Split-Path $f -Leaf)" }
    else { "  FAIL overlay 文件缺失 $f"; $script:fail++ }
}
if (Test-Path $overlayDll) {
    Check 'overlay DLL 架构(0x8664)' '34404' ([string](Get-PEMachine $overlayDll))
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($overlayDll))
    $exports = @('v_bg','v_blend','v_blit','v_bounds','v_boxMean','v_clear','v_flickerScore',
                 'v_glitch2','v_grain','v_lumaStats','v_motionCheck','v_over','v_resample')
    $missing = @()
    foreach ($e in $exports) { if ($dllText -notmatch [regex]::Escape($e)) { $missing += $e } }
    if ($missing.Count -eq 0) { "  ok   DLL 导出 13 个符号齐全" }
    else { "  FAIL DLL 缺少导出: $($missing -join ',')"; $script:fail++ }
}

# ---- 2. 像素内核黄金样本（期望值来自官方 32 位 kernel.dll 实测）----
$probe = @'
import av.video.kernel;
function cks(s){
    var a = 0, b = 0;
    for(i=1; #s; i++){
        a = (a + s[i]) % 1000003;
        b = (b + s[i]*i) % 1000003;
    }
    return a + ":" + b;
}
var w = 24, h = 16;
var buf = raw.buffer(w*h*4);

av.video.kernel.background(buf, w, h, {type="linear"; angle=45; stops={{0.0,0x102030},{0.5,0xA0B0C0},{1.0,0xFFFFFF}}; vignette=0.35; grain=0.25; cell=3; seed=7});
io.print("BG1 " ++ cks(raw.tostring(buf,1,w*h*4)));

var buf2 = raw.buffer(w*h*4);
av.video.kernel.background(buf2, w, h, {type="radial"; cx=0.4; cy=0.6; radius=0.8; stops={0x112233,0x445566}; vignette=0; grain=0; cell=3; seed=3});
io.print("BG2 " ++ cks(raw.tostring(buf2,1,w*h*4)));

av.video.kernel.grain(buf2, w, h, 0.3, 4, 11);
io.print("GRAIN " ++ cks(raw.tostring(buf2,1,w*h*4)));

var bb = av.video.kernel.bounds(buf2, w, h);
io.print("BOUNDS " ++ (bb ? ("" + bb.x0 + "," + bb.y0 + "," + bb.x1 + "," + bb.y1) : "null"));

av.video.kernel.clear(buf2, w, h);
io.print("CLEAR " ++ cks(raw.tostring(buf2,1,w*h*4)));

av.video.kernel.over(buf, buf2, w, h, 0.5);
io.print("OVER " ++ cks(raw.tostring(buf,1,w*h*4)));

av.video.kernel.blend(buf, buf2, w*h, 128);
io.print("BLEND " ++ cks(raw.tostring(buf,1,w*h*4)));

var buf3 = raw.buffer(w*h*4);
av.video.kernel.blit(buf3, w, h, buf, w, h, 2, 1);
io.print("BLIT " ++ cks(raw.tostring(buf3,1,w*h*4)));

var buf4 = raw.buffer(w*h*4);
av.video.kernel.glitch(buf4, buf, w, h, 0.6, 5, 0.004);
io.print("GLITCH " ++ cks(raw.tostring(buf4,1,w*h*4)));

var dw = w*2, dh = h*2;
var dst = raw.buffer(dw*dh*4);
av.video.kernel.resample(dst, dw, dh, buf, w, h, {x=0;y=0;width=dw;height=dh}, null, {opacity=0.8; filter=1; radius=3});
io.print("RESAMPLE " ++ cks(raw.tostring(dst,1,dw*dh*4)));

var cols = 4, rows = 3;
var mean = raw.buffer(cols*rows*4);
var sd = raw.buffer(cols*rows*4);
av.video.kernel.lumaStats(buf, w, h, w*4, cols, rows, mean, sd, 1);
io.print("LUMA " ++ cks(raw.tostring(mean,1,cols*rows*4)) ++ "|" ++ cks(raw.tostring(sd,1,cols*rows*4)));

var win = 2;
var ow = cols-win+1, oh = rows-win+1;
var out = raw.buffer(ow*oh*4);
av.video.kernel.boxMean(mean, cols, rows, win, out);
io.print("BOX " ++ cks(raw.tostring(out,1,ow*oh*4)));

var m1 = raw.buffer({double v[] = {1,2,3,4,5,6}});
var m2 = raw.buffer({double v[] = {2,4,6,8,10,12}});
var m3 = raw.buffer({double v[] = {1,3,5,7,9,11}});
var m4 = raw.buffer({double v[] = {6,5,4,3,2,1}});
var m5 = raw.buffer({double v[] = {3,1,4,1,5,9}});
var fs, arg, sgn = av.video.kernel.flickerScore(m1,m2,m3,m4,m5,ow,oh,1,0.5,null);
io.print("FLICK " ++ string.format('%.6f', fs) ++ "|" ++ arg ++ "|" ++ sgn);

var P = raw.buffer({double v[] = {1,2,3,4,5,6,7,8,9,10,11,12}});
var C = raw.buffer({double v[] = {1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5,10.5,11.5,12.5}});
var N = raw.buffer({double v[] = {2,3,4,5,6,7,8,9,10,11,12,13}});
var mc = av.video.kernel.motionCheck(P, C, N, cols, rows, 0, 0, cols-1, rows-1, 1, 1);
io.print("MOVE " ++ string.format('%.6f', mc.e0) ++ "|" ++ string.format('%.6f', mc.emc) ++ "|" ++ mc.d1x ++ "|" ++ mc.d1y ++ "|" ++ mc.n);
'@
$r = Run-Aardio $probe
if ($r.err) { "  NOTE stderr: $($r.err.Trim())" }
$map = @{}
foreach ($ln in ($r.out -split "`r?`n")) {
    if ($ln -match '^([A-Z0-9]+)\s+(.*?)\s*$') { $map[$Matches[1]] = $Matches[2] }
}
$expected = [ordered]@{
    'BG1'      = '264526:5119'                 # v_bg 线性渐变 + 暗角 + 颗粒
    'BG2'      = '176604:242686'               # v_bg 径向渐变
    'GRAIN'    = '176604:242686'               # v_grain
    'BOUNDS'   = '0,0,24,16'                   # v_bounds
    'CLEAR'    = '0:0'                          # v_clear
    'OVER'     = '264526:5119'                 # v_over
    'BLEND'    = '131788:131097'               # v_blend
    'BLIT'     = '111948:113169'               # v_blit
    'GLITCH'   = '169097:510447'               # v_glitch2
    'RESAMPLE' = '420526:783841'               # v_resample（含圆角与滤波）
    'LUMA'     = '4818:117971|4877:127301'     # v_lumaStats
    'BOX'      = '2941:36663'                  # v_boxMean
    'FLICK'    = '0.250000|null|null'          # v_flickerScore
    'MOVE'     = '0.000000|0.109375|0|-0.25|12' # v_motionCheck
}
foreach ($k in $expected.Keys) {
    $got = if ($map.ContainsKey($k)) { $map[$k] } else { '<缺失>' }
    Check $k $expected[$k] $got
}

# ---- 3. 模块解析路径：x64 必须命中 Nodio overlay，x86 必须走官方库 ----
$r2 = Run-Aardio 'import av.video.kernel; io.print("LIBPATH " + io.libpath("av.video.kernel"));'
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

if ($fail -eq 0) { "AV-VIDEO-KERNEL-OK"; exit 0 }
"AV-VIDEO-KERNEL-FAIL ($fail)"; exit 1
