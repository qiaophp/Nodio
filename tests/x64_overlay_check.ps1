# x64 补版库回归 —— 锁住「按官方同名路径提供 Nodio 自己的 x64 版本」这一批
#
# 官方这几份模块把 32 位 DLL 用 $ 内嵌进源码再内存加载（官方随库发布的
# .res 目录里同时给出了 C 源码，官方自己就是用它 + tcc/MSVC 重建的）：
#
#   lib/av/audio/dsp.aardio          .res/dsp.c            -> a_*   (38 个导出)
#   lib/win/region/png/_.aardio      .res/build.aardio 内嵌 C -> PngDataScan0ToRegion
#   lib/win/region/bitmap/_.aardio   .res/build.aardio 内嵌 C -> BitmapToRegion
#   lib/gdip/feather.aardio          .res/build-feather.aardio 内嵌 C -> copyAlpha
#   lib/raw/argsPointer/_.aardio     .res/build.aardio 内嵌 C -> getArgsPointer
#   lib/sys/audioVolume/_.aardio     .res/volumeCtrl2.cpp  -> CreateVolumeCtrl / GetVolume ...
#
# x64 进程装不下 32 位 PE → 这一批模块在 x64 上原本全部导入失败。
# Nodio 侧按**官方同名路径**提供自己的 x64 版本（overlay 目录优先于官方库）：
# .res\*.dll 由上面的 C 源码用 MSVC 重新编译（x64，/MT 静态 CRT），
# 模块源码逐字保留、只换 DLL 来源。
#
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），所以本回归在两个架构上都能跑。
#
# 用法: powershell -File x64_overlay_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$probe = @'
import av.audio.dsp;
import win.region.png;
import win.region.bitmap;
import gdip.feather;
import raw.argsPointer;
import sys.audioVolume;

io.print("DSP   " + type(av.audio.dsp.fill));
io.print("PNGR  " + type(win.region.png.PngDataScan0ToRegion));
io.print("BMPR  " + type(win.region.bitmap.BitmapToRegion));
io.print("FEATH " + type(gdip.feather._dll.copyAlpha));
io.print("ARGP  " + type(raw.argsPointer._dll.getArgsPointer));
io.print("VOL   " + type(sys.audioVolume._dll.GetVolume));

// av.audio.dsp 数值：x64 overlay 版与 x86 官方版必须给出同样的结果
var buf = {float f0; float f1; float f2; float f3;};
var p = raw.structPtr(buf);
av.audio.dsp.fill(p,4,1.5);
io.print("PEAK  " + av.audio.dsp.peak(p,4));
av.audio.dsp.scale(p,4,2.0);
io.print("SUM   " + av.audio.dsp.sum(p,4));

// win.region.png：PngDataScan0ToRegion 走的是「扫描 alpha 通道切出矩形块」
// 的算法 —— 用一张全透明的假扫描数据验证它返回的是有效 HRGN
var scan0 = raw.malloc(16 * 4);
for(i=0;16;1){
    // 每行 4 像素：第 2 像素不透明 -> 应切出一个 1 像素宽的区域
    var px = scan0 + i * 4;
    raw.convert(px, {int v = (i == 1) ? 0xFF000000 : 0;});
}
var hRgn = win.region.png.PngDataScan0ToRegion(scan0, 4, 4);
io.print("RGN   " + type(hRgn));
'@

$tmp = Join-Path $env:TEMP ("x64_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
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
function Check([string]$name, [string]$expect) {
    if ($script:out -match "(?m)^$name\s+(\S+)") {
        $got = $Matches[1]
        if ($got -eq $expect) { "  ok   $name = $got" }
        else { "  FAIL $name = $got (期望 $expect)"; $script:fail++ }
    } else {
        "  FAIL $name 缺失"; $script:fail++
    }
}

Check 'DSP'   'native'
Check 'PNGR'  'native'
Check 'BMPR'  'native'
Check 'FEATH' 'native'
Check 'ARGP'  'native'
Check 'VOL'   'native'
Check 'PEAK'  '1.5'
Check 'SUM'   '12'
Check 'RGN'   'pointer'

if ($fail -eq 0) { "X64-OVERLAY-OK"; exit 0 }
"X64-OVERLAY-FAIL ($fail)"; exit 1
