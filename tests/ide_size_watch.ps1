# ide_size_watch.ps1 — 每 100ms 采样一次窗口矩形/客户区，观察「还原」过程中
# 窗口尺寸到底怎么变的、什么时候变的。
#
# 目的：定位「最大化/还原黑屏」——
#   IDE 的 heartbeat 每 300ms 读一次 ::GetClientRect 决定要不要重排，
#   如果窗口尺寸在两次心跳之间变了又变回来，就会漏掉（黑屏）。
#
#   pwsh -File tests/ide_size_watch.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$DelayMs = 9000,
    [string]$ShotDir = 'D:\Nodio\build-x64\Release\size_watch'
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class SW2 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
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
    public static List<IntPtr> Children(IntPtr root) {
        var list = new List<IntPtr>();
        EnumChildWindows(root, (h,l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string RectStr(IntPtr h) { RECT r; GetWindowRect(h, out r); return string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B); }
    public static void Click(IntPtr h, int x, int y) {
        IntPtr lp = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(h, 0x200, IntPtr.Zero, lp);
        SendMessage(h, 0x201, new IntPtr(1), lp);
        SendMessage(h, 0x202, IntPtr.Zero, lp);
    }
}
'@
[SW2]::MakeDpiAware()

if (-not (Test-Path $ShotDir)) { New-Item -ItemType Directory -Path $ShotDir -Force | Out-Null }
$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG      = '1'
$env:NODIO_IDE_DEMO      = '1'
$env:NODIO_IDE_DEMO_FILE = 'D:\Nodio\ide\nodio\app\shell.nodio'
$env:NODIO_IDE_DEMO_TAB  = 'source'

$outF = Join-Path $ShotDir 'stdout.txt'
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
Start-Sleep -Milliseconds $DelayMs
$hwnd = [SW2]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
$dpi = 96; try { $d = [SW2]::GetDpiForWindow($hwnd); if ($d -gt 0) { $dpi = $d } } catch {}
$scale = $dpi / 96.0
Start-Sleep -Seconds 2

function Get-Titlebar {
    $best = $null
    foreach ($k in [SW2]::Children($hwnd)) {
        $r = [SW2]::RectStr($k) -split ','
        $w = [int]$r[2] - [int]$r[0]; $hh = [int]$r[3] - [int]$r[1]
        if ([SW2]::Cls($k) -eq 'AAPicturePlus2' -and [SW2]::IsWindowVisible($k) -and $hh -le 60 -and $hh -ge 30 -and $w -gt 1000) {
            if ($best -eq $null -or $w -gt $best.Width) {
                $best = [pscustomobject]@{ H=$k; Left=[int]$r[0]; Top=[int]$r[1]; Width=$w; Height=$hh }
            }
        }
    }
    return $best
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$last = ''
Write-Output 't(ms)   zoomed  winRect                    client'
Write-Output '--- 采样开始 ---'
$clicked = $false
while ($sw.ElapsedMilliseconds -lt 9000) {
    $rc = New-Object SW2+RECT
    [void][SW2]::GetWindowRect($hwnd, [ref]$rc)
    $c  = New-Object SW2+RECT
    [void][SW2]::GetClientRect($hwnd, [ref]$c)
    $key = "{0}|{1}" -f ([SW2]::IsZoomed($hwnd)), ($rc.R-$rc.L)
    if ($key -ne $last) {
        $last = $key
        Write-Output ("{0,6}  {1,-6}  {2,-24}  {3}x{4}" -f $sw.ElapsedMilliseconds, [SW2]::IsZoomed($hwnd), [SW2]::RectStr($hwnd), ($c.R-$c.L), ($c.B-$c.T))
    }
    if (-not $clicked -and $sw.ElapsedMilliseconds -gt 1200) {
        $clicked = $true
        Write-Output ("{0,6}  >>> 点「最大化/还原」按钮" -f $sw.ElapsedMilliseconds)
        $tb = Get-Titlebar
        if ($tb -ne $null) {
            $btnW = [int][Math]::Round(46 * $scale)
            $cx = $tb.Left + $tb.Width - $btnW * 1.5
            $cy = $tb.Top + [int]($tb.Height / 2)
            $cpt = New-Object SW2+POINT; $cpt.X = $cx; $cpt.Y = $cy
            [void][SW2]::ScreenToClient($tb.H, [ref]$cpt)
            [SW2]::Click($tb.H, $cpt.X, $cpt.Y)
        }
    }
    Start-Sleep -Milliseconds 100
}
Write-Output '--- IDE 的 clientWH 读数 ---'
Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like '*clientWH*' -or $_ -like '*relayout*' -or $_ -like '*tb.cmd*' } | ForEach-Object { Write-Output ("  " + $_) }
try { $p.Kill() } catch {}
