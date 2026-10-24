# x64 FFI 浮点 ABI 回归 —— 锁住「cdecl 调用绕过 XMM0..3」这个根因缺陷
#
# 缺陷（第二十八轮实测定位）：
#   engine/builtins.cpp 的调用分派写成
#       bool bStub = stub && !sig.bCdecl;      // x64
#   MS x64 ABI 只有**一套**调用约定：编译器忽略 __cdecl / __stdcall 的区别，
#   float/double 实参必须走 XMM0..XMM3。而 InvokeCdl / InvokeStd 的定型函数
#   指针把**每个**实参都声明成整数类（SlotT = unsigned __int64），
#   编译器于是把 double 的位模式塞进 RCX/RDX/R8/R9 —— 被调方在 XMM0 读到的是
#   垃圾，不是那个 double。
#   后果（修复前实测）：
#     · msvcrt pow(2.0, 10.0)  返回 1        （期望 1024）
#     · av.audio.dsp 的 a_fill 写不进缓冲区，a_peak 恒为 0
#   影响面：官方库里所有 `raw.loadDll(...,"cdecl")` + double/float 参数的模块
#   （av.audio.* / gdip / av.video.kernel / string.markdown / sevenZip.lzma …）。
#
# 修法：x64 一律走 ABI 正确的调用桩（bStub = stub != nullptr）。
#
# 用法: powershell -File ffi_double_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$probe = @'
// 1) cdecl + double 实参 / 返回值（必须走 XMM0..3）
var m = raw.loadDll("msvcrt.dll",,"cdecl");
var pw = m.api("pow","double(double a,double b)");
io.print("POW " + pw(2.0,10.0));
var fl = m.api("floor","double(double)");
io.print("FLOOR " + fl(3.7));
var sq = m.api("sqrt","double(double)");
io.print("SQRT " + sq(2.0));
var fm = m.api("fmod","double(double a,double b)");
io.print("FMOD " + fm(7.5,2.0));
var at = m.api("atan2","double(double a,double b)");
io.print("ATAN2 " + at(1.0,1.0));
// 2) 整数 + 浮点混合（位置相关：位置 1 的 double 进 xmm0，位置 2 的整数仍进 rdx）
var ldx = m.api("ldexp","double(double a,int e)");
io.print("LDEXP " + ldx(1.0,10));

// 3) av.audio.dsp overlay：x64 走 Nodio 自带的 x64 DLL，x86 走官方 32 位 DLL，
//    两侧数值必须一致
import av.audio.dsp;
var buf = {float f0; float f1; float f2; float f3;};
var p = raw.structPtr(buf);
av.audio.dsp.fill(p,4,1.5);
io.print("DSPPEAK " + av.audio.dsp.peak(p,4));
av.audio.dsp.scale(p,4,2.0);
io.print("DSPSUM " + av.audio.dsp.sum(p,4));
io.print("DSPSUMQ " + av.audio.dsp.sumsq(p,4));
'@

$tmp = Join-Path $env:TEMP ("ffi_double_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
[IO.File]::WriteAllText($tmp, $probe, (New-Object Text.UTF8Encoding($false)))

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$tmp`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$proc = [Diagnostics.Process]::Start($psi)
$out = $proc.StandardOutput.ReadToEnd()
$err = $proc.StandardError.ReadToEnd()
$proc.WaitForExit()
Remove-Item $tmp -ErrorAction SilentlyContinue
$out.TrimEnd()
if ($err) { "STDERR: $err" }

$fail = 0
function Check([string]$name, [string]$expect, [double]$tol = 0) {
    if ($script:out -match "(?m)^$name\s+(-?[\d.eE+-]+)") {
        $got = [double]$Matches[1]
        $ok = if ($tol -gt 0) { [Math]::Abs($got - $expect) -le $tol } else { $got -eq $expect }
        if ($ok) { "  ok   $name = $got" }
        else { "  FAIL $name = $got (期望 $expect)"; $script:fail++ }
    } else {
        "  FAIL $name 缺失"; $script:fail++
    }
}

Check 'POW'    '1024'
Check 'FLOOR'  '3'
Check 'SQRT'   '1.4142135623730951' 1e-9
Check 'FMOD'   '1.5'
Check 'ATAN2'  '0.7853981633974483' 1e-9
Check 'LDEXP'   '1024'
Check 'DSPPEAK' '1.5'
Check 'DSPSUM'  '12'
Check 'DSPSUMQ' '36'

if ($fail -eq 0) { "FFI-DOUBLE-OK"; exit 0 }
"FFI-DOUBLE-FAIL ($fail)"; exit 1
