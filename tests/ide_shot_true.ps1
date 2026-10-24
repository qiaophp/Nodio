# tests/ide_shot_true.ps1 — 抓一张「用户真实看到」的 IDE 截图。
#
# 为什么不用 tests/ide_shot.ps1：
#   PrintWindow 抓**父窗**时抓不到标准子控件（richedit）的内容 ——
#   屏幕上有字（GetPixel 读得到语法色），但父窗那张图里编辑区是空的。
#   （实测：单独 PrintWindow 编辑器控件 → 文字带语法色正常渲染。）
# 所以这里抓两张再合成：父窗（标题栏/工具条/停靠面板/页签/状态栏）
# + 编辑器控件（代码区），按屏幕坐标贴回去。
param(
    [string]$Out = 'D:\Nodio\build-x64\Release\ide_shot_true.png',
    [int]$DelayMs = 9000
)
$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE = $exe
$env:NODIO_RUN = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG = '1'
$env:NODIO_IDE_PROJECT = 'D:\Nodio\ide\nodio\ide.nproj'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassName(h,sb,256); return sb.ToString(); }
}
'@

function Grab([IntPtr]$hwnd, [double]$scale) {
    $rc = New-Object TS+RECT
    [void][TS]::GetWindowRect($hwnd, [ref]$rc)
    $pw = [int][Math]::Round(($rc.Right-$rc.Left) * $scale)
    $ph = [int][Math]::Round(($rc.Bottom-$rc.Top) * $scale)
    $bmp = New-Object System.Drawing.Bitmap $pw, $ph
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [TS]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    return @{ Bmp = $bmp; Rect = $rc; Ok = $ok }
}

$stdoutFile = [IO.Path]::GetTempFileName()
$errFile = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $errFile
try {
    Start-Sleep -Milliseconds $DelayMs
    $script:top = [IntPtr]::Zero; $script:big = 0
    $cb = [TS+EnumProc]{
        param($h,$l)
        [uint32]$procId = 0
        [TS]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
        if ($procId -eq $p.Id) {
            $r = New-Object TS+RECT
            [TS]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:big) { $script:big = $a; $script:top = $h }
        }
        return $true
    }
    [TS]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:top -eq [IntPtr]::Zero) { Write-Output 'FAIL: no IDE window'; exit 1 }
    # 环境可能把窗口最小化了 → 拉回最大化
    $r0 = New-Object TS+RECT
    [TS]::GetWindowRect($script:top, [ref]$r0) | Out-Null
    if (($r0.Right-$r0.Left) -lt 400) { [void][TS]::ShowWindow($script:top, 3); Start-Sleep -Milliseconds 2500 }

    # 找编辑器（最大的 RICHEDIT50W）
    $script:ed = [IntPtr]::Zero; $script:edArea = 0
    $cb2 = [TS+EnumProc]{
        param($h,$l)
        if ([TS]::Cls($h) -eq 'RICHEDIT50W') {
            $r = New-Object TS+RECT
            [TS]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:edArea) { $script:edArea = $a; $script:ed = $h }
        }
        return $true
    }
    [TS]::EnumChildWindows($script:top, $cb2, [IntPtr]::Zero) | Out-Null
    if ($script:ed -eq [IntPtr]::Zero) { Write-Output 'FAIL: no richedit'; exit 1 }

    $dpi = 0
    try { $dpi = [TS]::GetDpiForWindow($script:top) } catch {}
    if ($dpi -le 0) { $dpi = 96 }
    $scale = $dpi / 96.0

    $parent = Grab $script:top $scale
    $child  = Grab $script:ed  $scale

    $dst = $parent.Bmp
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $ox = [int][Math]::Round(($child.Rect.Left  - $parent.Rect.Left) * $scale)
    $oy = [int][Math]::Round(($child.Rect.Top   - $parent.Rect.Top)  * $scale)
    $g.DrawImage($child.Bmp, $ox, $oy)
    $g.Dispose()
    $child.Bmp.Dispose()
    $dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $dst.Dispose()
    $pw = $parent.Rect.Right - $parent.Rect.Left
    $ph = $parent.Rect.Bottom - $parent.Rect.Top
    Write-Output ("SHOT-TRUE: {0} parent={1}x{2} editorAt={3},{4}" -f $Out, $pw, $ph, $ox, $oy)
}
finally {
    try { $p.Kill() } catch {}
}
exit 0
