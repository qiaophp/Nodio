# ide_hit_probe.ps1 — 诊断「点击落到哪个窗口」。
#
# 目的：把「按钮点不动 / 页签切不了」定位到**窗口层**而不是猜：
#   · 每个坐标上命中的是哪个子窗口（类名 + 句柄）
#   · 各子窗口的 z 序（谁盖住谁）
#   · 子窗口的客户区矩形（自绘板的矩形是不是摆对位置了）
#
# 坐标是**逻辑像素**（客户区），按窗口 DPI 换算成物理像素。
#
#   pwsh -File tests/ide_hit_probe.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Run  = 'D:\Nodio\ide\nodio\main.nodio',
    [string]$DemoFile = 'D:\Nodio\ide\nodio\app\shell.nodio',
    [string]$DemoTab  = 'source',
    [int]$WaitMs  = 9000
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class HP {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint flags);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    public static int ExStyle(IntPtr h) { return GetWindowLong(h, -20); } // GWL_EXSTYLE
    public static string StyleFlags(IntPtr h) {
        int s = ExStyle(h);
        var sb = new StringBuilder(64);
        if ((s & 0x20) != 0) sb.Append("TRANSPARENT ");
        if ((s & 0x80000) != 0) sb.Append("LAYERED ");
        if ((s & 0x40000) != 0) sb.Append("TOOLWINDOW ");
        if ((s & 0x100000) != 0) sb.Append("COMPOSITED ");
        return sb.ToString().Trim();
    }

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
    public static string Cls(IntPtr h) {
        var sb = new StringBuilder(256); GetClassName(h, sb, 256);
        return sb.ToString();
    }
    // z 序：GW_HWNDPREV(3) 往上是更靠前。返回 0 = 最上面
    public static int ZRank(IntPtr h) {
        int n = 0;
        IntPtr p = h;
        while (true) {
            IntPtr q = GetWindow(p, 3); // GW_HWNDPREV
            if (q == IntPtr.Zero) break;
            n++;
            p = q;
            if (n > 500) break;
        }
        return n;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr p);
    // 权威 z 序：从最上面开始用 GW_HWNDNEXT(2) 往下走，index 0 = 最上面
    public static List<IntPtr> TopDown(IntPtr root) {
        var list = new List<IntPtr>();
        IntPtr h = GetTopWindow(root);
        while (h != IntPtr.Zero) {
            list.Add(h);
            h = GetWindow(h, 2); // GW_HWNDNEXT = 更下面
        }
        return list;
    }
    public static List<IntPtr> Children(IntPtr root) {
        var list = new List<IntPtr>();
        EnumChildWindows(root, (h,l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static IntPtr Hit(IntPtr root, int x, int y, uint flags) {
        var pt = new POINT(); pt.X = x; pt.Y = y;
        return ChildWindowFromPointEx(root, pt, flags);
    }
    public static string RectStr(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B);
    }
}
'@
[HP]::MakeDpiAware()

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = $Run
$env:NODIO_IDE_DIAG      = '1'
$env:NODIO_IDE_DEMO      = '1'
$env:NODIO_IDE_DEMO_FILE = $DemoFile
$env:NODIO_IDE_DEMO_TAB  = $DemoTab

$outF = [IO.Path]::GetTempFileName()
$errF = "$outF.err"
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError $errF
Start-Sleep -Milliseconds $WaitMs
$hwnd = [HP]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
[void][HP]::ShowWindow($hwnd, 5)

$dpi = 96; try { $d = [HP]::GetDpiForWindow($hwnd); if ($d -gt 0) { $dpi = $d } } catch {}
$scale = $dpi / 96.0
Write-Output ("ROOT {0}  dpi={1} scale={2}" -f $hwnd.ToInt64(), $dpi, $scale)

Write-Output '=== CHILD WINDOWS (z=0 是最上面) ==='
$kids = [HP]::Children($hwnd)
$rows = @()
$topDown = [HP]::TopDown($hwnd)
$zOf = @{}
for ($i = 0; $i -lt $topDown.Count; $i++) { $zOf[$topDown[$i].ToInt64()] = $i }
foreach ($k in $kids) {
    $z = if ($zOf.ContainsKey($k.ToInt64())) { $zOf[$k.ToInt64()] } else { 99 }
    $rows += [pscustomobject]@{
        Z = $z; H = $k.ToInt64(); Cls = [HP]::Cls($k)
        Rect = [HP]::RectStr($k); Vis = [HP]::IsWindowVisible($k)
        Ex = [HP]::StyleFlags($k)
    }
}
$rows | Sort-Object Z | ForEach-Object {
    Write-Output ("z={0,-3} hwnd={1,-8} {2,-16} rect={3,-28} vis={4} ex={5}" -f $_.Z, $_.H, $_.Cls, $_.Rect, $_.Vis, $_.Ex)
}

Write-Output '=== HIT TEST (逻辑像素客户区) ==='
$POINTS = @(
    @('工具条x30',   30,  46),
    @('工具条x62',   62,  46),
    @('工具条x118', 118,  46),
    @('工具条x150', 150,  46),
    @('左页签x20',   20,  75),
    @('左页签x80',   80,  75),
    @('左页签x160', 160,  75),
    @('右页签x2220', 2220, 75),
    @('底页签x280',  280, 1200),
    @('文档页签x300',300,  98),
    @('工程树x30',   30, 100),
    @('编辑区x400',  400, 150),
    @('标题x57',     57,  15),
    @('标题x150',   150,  15)
)
foreach ($pt in $POINTS) {
    $nm = $pt[0]
    $px = [int][Math]::Round([int]$pt[1] * $scale)
    $py = [int][Math]::Round([int]$pt[2] * $scale)
    $h0 = [HP]::Hit($hwnd, $px, $py, 0)                    # flags=0：含不可见窗口
    $h1 = [HP]::Hit($hwnd, $px, $py, 1)                    # CWP_SKIPINVISIBLE
    $h2 = [HP]::Hit($hwnd, $px, $py, 2)                    # CWP_SKIPTRANSPARENT
    $d0 = if ($h0 -eq [IntPtr]::Zero) { '-' } else { [HP]::Cls($h0) + '#' + $h0.ToInt64() }
    $d1 = if ($h1 -eq [IntPtr]::Zero) { '-' } else { [HP]::Cls($h1) + '#' + $h1.ToInt64() }
    $d2 = if ($h2 -eq [IntPtr]::Zero) { '-' } else { [HP]::Cls($h2) + '#' + $h2.ToInt64() }
    # WindowFromPoint：系统真正用来派发鼠标消息的拾取（屏幕坐标）
    $wpt = New-Object HP+POINT; $wpt.X = $px; $wpt.Y = $py
    $wh = [HP]::WindowFromPoint($wpt)
    $dw = if ($wh -eq [IntPtr]::Zero) { '-' } else { [HP]::Cls($wh) + '#' + $wh.ToInt64() }
    Write-Output ("{0,-14} phys=({1},{2})  hitAll={3,-24} hitVis={4,-24} winFromPoint={5}" -f $nm, $px, $py, $d0, $d1, $dw)
}

Write-Output '=== STDOUT (diag) ==='
if (Test-Path $outF) { Get-Content $outF -Encoding UTF8 | Select-Object -Last 30 | ForEach-Object { Write-Output $_ } }

# 截图（PrintWindow：锁屏也能拍到）
Add-Type -AssemblyName System.Drawing
$shot = 'D:\Nodio\build-x64\Release\ide_hit_probe.png'
$rc = New-Object HP+RECT
[void][HP]::GetWindowRect($hwnd, [ref]$rc)
$w = $rc.R - $rc.L; $h = $rc.B - $rc.T
if ($w -gt 0 -and $h -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][HP]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output ("SHOT: " + $shot)
}
try { $p.Kill() } catch {}
Remove-Item $outF, $errF -Force -ErrorAction SilentlyContinue
