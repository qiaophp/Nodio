# probe_shot.ps1 — 给「探针窗体」截图（置顶 + 移到屏幕左上，避免被别的程序挡住）
#
#   pwsh -NoProfile -File tests/probe_shot.ps1 -Script <aardio> -Out <png> [-DelayMs 2500]
param(
    [Parameter(Mandatory=$true)][string]$Script,
    [Parameter(Mandatory=$true)][string]$Out,
    [int]$DelayMs = 2500
)

$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_RUN           = $Script
$env:NODIO_PAUSE         = '0'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ProbeShot {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    // 窗口面积最大的那个（探针 form 一般是唯一的大窗）
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
$hwnd = [ProbeShot]::Biggest([uint32]$p.Id)
if ($hwnd -ne [IntPtr]::Zero) {
    # 置顶(HWND_TOPMOST=-1) 并移到屏幕左上，尺寸 700x400
    [void][ProbeShot]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 700, 400, 0x0040)
    Start-Sleep -Milliseconds 700
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

Write-Output ("SHOT: {0}" -f $outFull)
$o = Get-Content $stdoutFile -Raw -ErrorAction SilentlyContinue
if ($o) { Write-Output "--- stdout ---"; Write-Output $o.Trim() }
$e = Get-Content $stderrFile -Raw -ErrorAction SilentlyContinue
if ($e) { Write-Output "--- stderr ---"; Write-Output $e.Trim() }
exit 0
