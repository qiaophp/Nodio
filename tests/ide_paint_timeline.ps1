# ide_paint_timeline.ps1 — IDE 启动「内容出现」时间轴。
#
# 窗口句柄 1 秒就有了，但**组件内容**可能迟迟不画出来（自绘板的
# 首次 WM_PAINT 更新区域为空 ⇒ 画一遍等于没画）。
# 这里按固定间隔连续截图，交给 ide_paint_timeline.py 分析
# 「画面什么时候稳定成完整 IDE」。
#
#   pwsh -File tests/ide_paint_timeline.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Run  = 'D:\Nodio\ide\nodio\main.nodio',
    [string]$ShotDir = 'D:\Nodio\build-x64\Release\paint_timeline',
    [int]$TotalMs = 40000,
    [int]$IntervalMs = 500,
    [int]$WaitMs = 9000
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class PT {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public static void MakeDpiAware() {
        try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch { }
        try { SetProcessDPIAware(); } catch { }
    }
    public static IntPtr Biggest(uint want) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h,l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want || !IsWindowVisible(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long a = (long)(r.R-r.L)*(r.B-r.T);
            if (a > area) { area = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@
[PT]::MakeDpiAware()

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = $Run

if (Test-Path $ShotDir) {
    Get-ChildItem $ShotDir -Filter '*.png' -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
    }
} else {
    New-Item -ItemType Directory -Path $ShotDir -Force | Out-Null
}

$outF = Join-Path $ShotDir 'stdout.txt'
$errF = Join-Path $ShotDir 'stderr.txt'
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError $errF
$sw = [Diagnostics.Stopwatch]::StartNew()
$hwnd = [IntPtr]::Zero
$n = 0
while ($sw.ElapsedMilliseconds -lt $TotalMs) {
    if ($hwnd -eq [IntPtr]::Zero) {
        $hwnd = [PT]::Biggest([uint32]$p.Id)
        if ($hwnd -ne [IntPtr]::Zero) { Write-Output ("WINDOW at {0}ms" -f $sw.ElapsedMilliseconds) }
    }
    if ($hwnd -ne [IntPtr]::Zero) {
        $rc = New-Object PT+RECT
        [void][PT]::GetWindowRect($hwnd, [ref]$rc)
        $w = $rc.R - $rc.L; $h = $rc.B - $rc.T
        if ($w -gt 0 -and $h -gt 0) {
            $n++
            $bmp = New-Object System.Drawing.Bitmap $w, $h
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $hdc = $g.GetHdc()
            [void][PT]::PrintWindow($hwnd, $hdc, 2)
            $g.ReleaseHdc($hdc); $g.Dispose()
            $bmp.Save((Join-Path $ShotDir ("t_{0:d4}.png" -f $n)), [System.Drawing.Imaging.ImageFormat]::Png)
            $bmp.Dispose()
        }
    }
    Start-Sleep -Milliseconds $IntervalMs
}
Write-Output ("SHOTS: {0}  interval={1}ms" -f $n, $IntervalMs)
try { $p.Kill() } catch {}
