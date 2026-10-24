# ide_restore_dump.ps1 — 最大化 → 还原之后，导出一遍子窗口 + 截图。
#
# 用来定位「还原后黑屏」：是画板没重排、还是画板被官方控件盖住、
# 还是官方控件被重建（句柄变了）。
#
#   pwsh -File tests/ide_restore_dump.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$DelayMs = 9000,
    [string]$ShotDir = 'D:\Nodio\build-x64\Release\restore_dump'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class RD {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
    [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr p);
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
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
    public static List<IntPtr> TopDown(IntPtr root) {
        var list = new List<IntPtr>();
        IntPtr h = GetTopWindow(root);
        while (h != IntPtr.Zero) { list.Add(h); h = GetWindow(h, 2); }
        return list;
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
[RD]::MakeDpiAware()

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
$hwnd = [RD]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
$dpi = 96; try { $d = [RD]::GetDpiForWindow($hwnd); if ($d -gt 0) { $dpi = $d } } catch {}
$scale = $dpi / 96.0
Start-Sleep -Seconds 2

function Dump([string]$tag) {
    Write-Output "=== $tag ==="
    $rc = New-Object RD+RECT
    [void][RD]::GetWindowRect($hwnd, [ref]$rc)
    Write-Output ("form rect={0} zoomed={1}" -f [RD]::RectStr($hwnd), [RD]::IsZoomed($hwnd))
    $zOf = @{}
    $td = [RD]::TopDown($hwnd)
    for ($i = 0; $i -lt $td.Count; $i++) { $zOf[$td[$i].ToInt64()] = $i }
    $rows = @()
    foreach ($k in [RD]::Children($hwnd)) {
        $z = if ($zOf.ContainsKey($k.ToInt64())) { $zOf[$k.ToInt64()] } else { 99 }
        $rows += [pscustomobject]@{ Z=$z; H=$k.ToInt64(); C=[RD]::Cls($k); R=[RD]::RectStr($k); V=[RD]::IsWindowVisible($k) }
    }
    $rows | Sort-Object Z | ForEach-Object {
        Write-Output ("  z={0,-3} hwnd={1,-9} {2,-16} rect={3,-26} vis={4}" -f $_.Z, $_.H, $_.C, $_.R, $_.V)
    }
    $bmpW = $rc.R - $rc.L; $bmpH = $rc.B - $rc.T
    if ($bmpW -gt 0 -and $bmpH -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap $bmpW, $bmpH
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][RD]::PrintWindow($hwnd, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        $bmp.Save((Join-Path $ShotDir ($tag + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
    }
}

Dump 'before_restore'

# 点「最大化/还原」按钮：按钮位置**从真实画板矩形推**
# （自己按窗口宽算会偏 100 多像素，点下去落到「最小化」上）。
$tb = $null
foreach ($k in [RD]::Children($hwnd)) {
    $r = [RD]::RectStr($k) -split ','
    $w = [int]$r[2] - [int]$r[0]
    $hh = [int]$r[3] - [int]$r[1]
    if ([RD]::Cls($k) -eq 'AAPicturePlus2' -and [RD]::IsWindowVisible($k) -and $hh -le 60 -and $hh -ge 30 -and $w -gt 1000) {
        if ($tb -eq $null -or $w -gt $tb.Width) {
            $tb = [pscustomobject]@{ H=$k; Left=[int]$r[0]; Top=[int]$r[1]; Width=$w; Height=$hh }
        }
    }
}
if ($tb -eq $null) { Write-Output 'no titlebar'; try { $p.Kill() } catch {}; exit 1 }
$btnW = [int][Math]::Round(46 * $scale)
$cx = $tb.Left + $tb.Width - $btnW * 1.5
$cy = $tb.Top + [int]($tb.Height / 2)
$target = $tb.H
$cpt = New-Object RD+POINT; $cpt.X = $cx; $cpt.Y = $cy
[void][RD]::ScreenToClient($target, [ref]$cpt)
[RD]::Click($target, $cpt.X, $cpt.Y)
Start-Sleep -Seconds 3

Dump 'after_restore'

# 判定：IDE 是**冻结**了还是**死循环**了？
$sw = [Diagnostics.Stopwatch]::StartNew()
[void][RD]::SendMessage($hwnd, 0x0, [IntPtr]0, [IntPtr]0)
$ping = $sw.ElapsedMilliseconds
$cpu1 = 0
try { $cpu1 = [math]::Round($p.TotalProcessorTime.TotalMilliseconds) } catch {}
Start-Sleep -Seconds 2
$cpu2 = 0
try { $cpu2 = [math]::Round($p.TotalProcessorTime.TotalMilliseconds) } catch {}
Write-Output ("WM_NULL 往返={0}ms   CPU {1}ms -> {2}ms (2秒内增长 {3}ms)" -f $ping, $cpu1, $cpu2, ($cpu2 - $cpu1))
Write-Output ("stdout 行数=" + @(Get-Content (Join-Path $ShotDir 'stdout.txt') -ErrorAction SilentlyContinue).Count)
try { $p.Kill() } catch {}
