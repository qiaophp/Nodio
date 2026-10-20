# ide_shot_screen.ps1 — 全屏截图（模态对话框不在 IDE 窗口矩形内时用）
#
#   pwsh -NoProfile -File tests/ide_shot_screen.ps1 -Out shot.png [-DelayMs 4000] [-Script <file>]
param(
    [string]$Out      = "$PSScriptRoot\..\build-x64\Release\ide_screen.png",
    [string]$Script   = 'D:\Nodio\ide\nodio\main.nodio',
    [int]   $DelayMs  = 4000
)

$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $exe
$env:NODIO_RUN           = $Script

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WinShot {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public static IntPtr Biggest(uint want) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want || !IsWindowVisible(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (a > area) { area = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@

$stdoutFile = [IO.Path]::GetTempFileName()
$stderrFile = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
Start-Sleep -Milliseconds $DelayMs
$hwnd = [WinShot]::Biggest([uint32]$p.Id)
if ($hwnd -ne [IntPtr]::Zero) {
    # 置顶并挪到屏幕左上（桌面别的置顶程序会挡住 IDE，截图要能看见全貌）
    [void][WinShot]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0040)
    Start-Sleep -Milliseconds 600
}
$bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bmp.Size)
$g.Dispose()
$outFull = [IO.Path]::GetFullPath($Out)
$dir = Split-Path -Parent $outFull
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$bmp.Save($outFull, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
try { $p.Kill() } catch {}
Write-Output ("SCREEN: {0} ({1}x{2})" -f $outFull, $bounds.Width, $bounds.Height)
exit 0
