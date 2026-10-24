# ide_click.ps1 — 在 IDE 窗口的**客户区逻辑坐标**上模拟一次真实点击，截前后两张图。
#
# 用途：自绘控件（plus 板）的点击链路一旦断了，肉眼看截图完全看不出来
# （界面照常画出来，就是点不动）。这个脚本把「点」这件事变成可回归的东西：
#   pwsh -File tests/ide_click.ps1 -At "227,72" -Out shot.png
#   -At 用**逻辑像素**（DPI 150% 下会自动换算成物理像素）。
param(
    [string]$Exe      = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Run      = 'D:\Nodio\ide\nodio\main.nodio',
    # 多点用分号分隔：-At "108,15;107,116"
    [string]$At       = '227,72',
    [string]$Out      = "$PSScriptRoot\..\build-x64\Release\ide_click.png",
    [int]$DelayMs     = 8000,
    [int]$AfterMs     = 1500
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CK {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    public static List<string> ZChain(IntPtr h) {
        var outp = new List<string>();
        IntPtr c = h; int i = 0;
        while (c != IntPtr.Zero && i < 40) {
            var sb = new StringBuilder(256); GetClassName(c, sb, 256);
            RECT r; GetWindowRect(c, out r);
            outp.Add(string.Format("ABOVE{0} {1} {2} [{3},{4},{5},{6}]", i, c.ToInt64(), sb.ToString(), r.L, r.T, r.R, r.B));
            c = GetWindow(c, 3/*GW_HWNDPREV*/);
            i++;
        }
        return outp;
    }
    public static List<string> ZOrder(IntPtr root) {
        var outp = new List<string>();
        IntPtr h = GetWindow(root, 5/*GW_CHILD*/);
        int i = 0;
        while (h != IntPtr.Zero && i < 60) {
            var sb = new StringBuilder(256); GetClassName(h, sb, 256);
            RECT r; GetWindowRect(h, out r);
            outp.Add(string.Format("Z{0} {1} {2} [{3},{4},{5},{6}]", i, h.ToInt64(), sb.ToString(), r.L, r.T, r.R, r.B));
            h = GetWindow(h, 2/*GW_HWNDNEXT*/);
            i++;
        }
        return outp;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetCapture();
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    public static IntPtr ChildAt(IntPtr root, int x, int y) {
        var pt = new POINT(); pt.X = x; pt.Y = y;
        // flags=0：不跳过透明窗口（WS_EX_TRANSPARENT 的浮层也要能命中）
        return ChildWindowFromPointEx(root, pt, 0);
    }
    public static void SendClick(IntPtr h, int x, int y) {
        IntPtr lp = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(h, 0x200/*WM_MOUSEMOVE*/, new IntPtr(0), lp);   // 先触发悬停
        SendMessage(h, 0x201/*WM_LBUTTONDOWN*/, new IntPtr(1), lp);
        SendMessage(h, 0x202/*WM_LBUTTONUP*/, new IntPtr(0), lp);
    }
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public static void MakeDpiAware() {
        try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch { }
        try { SetProcessDPIAware(); } catch { }
    }
    public static string Desc(IntPtr h) {
        if (h == IntPtr.Zero) return "none";
        var sb = new StringBuilder(256); GetClassName(h, sb, 256);
        RECT r; GetWindowRect(h, out r);
        return string.Format("{0} {1} [{2},{3},{4},{5}]", h.ToInt64(), sb.ToString(), r.L, r.T, r.R, r.B);
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
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(120);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);   // LEFTDOWN
        System.Threading.Thread.Sleep(60);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);   // LEFTUP
    }
    public static List<string> Tops(uint want) {
        var outp = new List<string>();
        EnumWindows((h,l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want) return true;
            RECT r; GetWindowRect(h, out r);
            var sb = new StringBuilder(256); GetClassName(h, sb, 256);
            outp.Add(string.Format("{0}\t{1}\t{2}\tvis={3}",
                h.ToInt64(), sb.ToString(),
                string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B),
                IsWindowVisible(h) ? 1 : 0));
            return true;
        }, IntPtr.Zero);
        return outp;
    }
    public static List<string> Kids(IntPtr root) {
        var outp = new List<string>();
        EnumChildWindows(root, (h,l) => {
            RECT r; GetWindowRect(h, out r);
            var sb = new StringBuilder(256); GetClassName(h, sb, 256);
            outp.Add(string.Format("{0}\t{1}\t{2}\tvis={3}",
                h.ToInt64(), sb.ToString(),
                string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B),
                IsWindowVisible(h) ? 1 : 0));
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
'@

# 本进程必须 DPI 感知：否则 GetWindowRect / ClientToScreen 返回的是
# 虚拟化过的逻辑坐标，点击位置会整体偏掉（实测偏到屏幕外，点了等于没点）。
[CK]::MakeDpiAware()

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = $Run

$outF = [IO.Path]::GetTempFileName()
$errF = "$outF.err"
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError $errF
Start-Sleep -Milliseconds $DelayMs
$hwnd = [CK]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }

# ⚠ 不要用 SW_RESTORE(9)：它会把最大化窗口还原回正常位置，
# 而且还原是**异步**的（带窗口动画）—— 紧接着取 ClientToScreen 拿到的
# 还是最大化时的坐标，点击位置和实际窗口完全对不上。
[void][CK]::ShowWindow($hwnd, 5)
[void][CK]::SetWindowPos($hwnd, [IntPtr](-1), 0,0,0,0, 0x0001 -bor 0x0002 -bor 0x0040)
[void][CK]::BringWindowToTop($hwnd)
[void][CK]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400

$outFull = [IO.Path]::GetFullPath($Out)
$dpi0 = 0; try { $dpi0 = [CK]::GetDpiForWindow($hwnd) } catch {}
if ($dpi0 -le 0) { $dpi0 = 96 }
$scale = $dpi0 / 96.0

foreach ($one in ($At -split ';')) {
    $parts = $one -split ','
    $lx = [int]$parts[0]; $ly = [int]$parts[1]
    $px = [int][Math]::Round($lx * $scale)
    $py = [int][Math]::Round($ly * $scale)
    $pt = New-Object CK+POINT
    $pt.X = $px; $pt.Y = $py
    [void][CK]::ClientToScreen($hwnd, [ref]$pt)
    Write-Output ("CLICK logical=({0},{1}) physical=({2},{3}) screen=({4},{5}) dpi={6}" -f $lx, $ly, $px, $py, $pt.X, $pt.Y, $dpi0)

    # 桌面是锁屏状态时，合成鼠标事件送不到 IDE（锁屏窗口在 z 序最顶）。
    # 直接向命中点所在的子窗口发窗口消息：确定、不依赖前台焦点、不依赖桌面。
    $target = [CK]::ChildAt($hwnd, $px, $py)
    # ⚠ 有鼠标捕获时，真实鼠标事件是发给**捕获窗口**的，
    # 不管命中测试算出来是谁（浮层类控件用 SetCapture 绕开命中测试）。
    $cap = [CK]::GetCapture()
    if ($cap -ne [IntPtr]::Zero -and [CK]::GetAncestor($cap, 2) -eq $hwnd) {
        Write-Output ("CAPTURE-TAKES-OVER: " + [CK]::Desc($cap))
        $target = $cap
    }
    Write-Output ("TARGET: " + [CK]::Desc($target))
    if ($target -ne [IntPtr]::Zero) {
        # 换算成子窗口自己的客户区坐标
        $cpt = New-Object CK+POINT; $cpt.X = $px; $cpt.Y = $py
        [void][CK]::ScreenToClient($target, [ref]$cpt)
        Write-Output ("TARGET-CLIENT: {0},{1}" -f $cpt.X, $cpt.Y)
        [CK]::SendClick($target, $cpt.X, $cpt.Y)
    }
    Start-Sleep -Milliseconds 600
}
Start-Sleep -Milliseconds $AfterMs

# 点击后的顶层窗口（菜单/对话框是独立顶层窗口）
$tops = [CK]::Tops([uint32]$p.Id)
Write-Output ("TOP-COUNT {0}" -f $tops.Count)
$tops | ForEach-Object { Write-Output ("TOP " + $_) }
# 点击后新出现的可见子窗口（下拉菜单是自绘板 → 子窗口）
$kids = [CK]::Kids($hwnd)
Write-Output ("KID-ALL {0}" -f $kids.Count)
$kids | ForEach-Object { Write-Output ("KID " + $_) }
$vis = @($kids | Where-Object { $_ -match 'vis=1' })
Write-Output ("KID-VISIBLE {0}" -f $vis.Count)
[CK]::ZOrder($hwnd) | ForEach-Object { Write-Output ("Z " + $_) }
# 找出下拉菜单画板（点开菜单后新出现的那个 91x129 / 204x330 板）
$pop = $kids | Where-Object { $_ -match 'AAPicturePlus2' -and $_ -notmatch 'vis=0' } |
       Where-Object { $p = ($_ -split "`t")[2] -split ','; ([int]$p[2] - [int]$p[0]) -gt 100 -and ([int]$p[3] - [int]$p[1]) -gt 200 }
if ($pop) {
    $ph = [IntPtr][int64](($pop[0] -split "`t")[0])
    Write-Output ("POPUP-HWND: " + [CK]::Desc($ph))
    [CK]::ZChain($ph) | ForEach-Object { Write-Output ("Z " + $_) }
}
$vis | ForEach-Object { Write-Output ("KIDV " + $_) }

# 单独抓第一个可见子窗口（下拉菜单板）—— 看它**自己**的窗口里有没有内容：
#   有内容 → 它正常绘制，被别的窗口盖住了（z 序）
#   没内容 → 它是「直绘到窗体背景」的（绘制机制问题）
if ($vis.Count -gt 0) {
    $first = ($vis[0] -split "`t")[0]
    $ph = [IntPtr][int64]$first
    $prc = New-Object CK+RECT
    [void][CK]::GetWindowRect($ph, [ref]$prc)
    $pw2 = $prc.R - $prc.L; $ph2 = $prc.B - $prc.T
    if ($pw2 -gt 0 -and $ph2 -gt 0) {
        $bmp2 = New-Object System.Drawing.Bitmap $pw2, $ph2
        $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
        $hdc2 = $g2.GetHdc()
        [void][CK]::PrintWindow($ph, $hdc2, 2)
        $g2.ReleaseHdc($hdc2); $g2.Dispose()
        $pOut = [IO.Path]::ChangeExtension($outFull, '.popup.png')
        $bmp2.Save($pOut, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp2.Dispose()
        Write-Output ("POPUP-SHOT: {0} ({1}x{2})" -f $pOut, $pw2, $ph2)
    }
}

# 截图（本进程已 DPI 感知：GetWindowRect 直接给物理像素）
$rc = New-Object CK+RECT
[void][CK]::GetWindowRect($hwnd, [ref]$rc)
$w = $rc.R - $rc.L; $h = $rc.B - $rc.T
$pw = $w; $ph = $h
$bmp = New-Object System.Drawing.Bitmap $pw, $ph
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][CK]::PrintWindow($hwnd, $hdc, 2)
$g.ReleaseHdc($hdc); $g.Dispose()
$outFull = [IO.Path]::GetFullPath($Out)
$bmp.Save($outFull, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("SHOT: {0}" -f $outFull)
try { $p.Kill() } catch {}
if (Test-Path $outF) {
    $diag = Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue
    if ($diag) { Write-Output '--- IDE stdout ---'; $diag | Select-Object -Last 40 | ForEach-Object { Write-Output $_ } }
}
if (Test-Path $errF) {
    $e = Get-Content $errF -Encoding UTF8 -ErrorAction SilentlyContinue
    if ($e) { Write-Output '--- IDE stderr ---'; $e | Select-Object -Last 40 | ForEach-Object { Write-Output $_ } }
}
Remove-Item $outF, $errF -Force -ErrorAction SilentlyContinue
