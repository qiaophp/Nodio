# ide_size_sync.ps1 — 同一时刻对照「OS 真实尺寸」与「IDE 自己读到的客户区尺寸」。
#
# 用途：定位「最大化/还原黑屏」——到底是 IDE 的 clientWH() 读错了，
# 还是 OS 报的尺寸和 IDE 读的不是同一个窗口。
#
#   pwsh -File tests/ide_size_sync.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$DelayMs = 9000,
    [string]$ShotDir = 'D:\Nodio\build-x64\Release\size_sync'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class SS {
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
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
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
    public static List<IntPtr> Children(IntPtr root) {
        var list = new List<IntPtr>();
        EnumChildWindows(root, (h,l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string RectStr(IntPtr h) { RECT r; GetWindowRect(h, out r); return string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B); }
    public static string ClientStr(IntPtr h) { RECT r; GetClientRect(h, out r); return string.Format("{0}x{1}", r.R-r.L, r.B-r.T); }
    public static void Click(IntPtr h, int x, int y) {
        IntPtr lp = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(h, 0x200, IntPtr.Zero, lp);
        SendMessage(h, 0x201, new IntPtr(1), lp);
        SendMessage(h, 0x202, IntPtr.Zero, lp);
    }
}
'@
[SS]::MakeDpiAware()

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
$hwnd = [SS]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
$dpi = 96; try { $d = [SS]::GetDpiForWindow($hwnd); if ($d -gt 0) { $dpi = $d } } catch {}
$scale = $dpi / 96.0
Start-Sleep -Seconds 2

function Last-IdeRead {
    if (-not (Test-Path $outF)) { return '(no stdout)' }
    $l = Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue |
         Where-Object { $_ -like '*clientWH*' -or $_ -like '*relayout*' } | Select-Object -Last 2
    if (-not $l) { return '(IDE 还没打印过)' }
    return ($l -join ' | ')
}

function Report([string]$tag) {
    $rc = New-Object SS+RECT
    [void][SS]::GetWindowRect($hwnd, [ref]$rc)
    $c  = New-Object SS+RECT
    [void][SS]::GetClientRect($hwnd, [ref]$c)
    Write-Output ("--- {0} ---" -f $tag)
    Write-Output ("  OS : zoomed={0} iconic={1} winRect={2} client={3}x{4}" -f `
        [SS]::IsZoomed($hwnd), [SS]::IsIconic($hwnd), [SS]::RectStr($hwnd), ($c.R-$c.L), ($c.B-$c.T))
    Write-Output ("  IDE: {0}" -f (Last-IdeRead))
}

Report 'after-startup'

# 找标题栏画板 → 推「最大化/还原」按钮位置
function Get-Titlebar {
    foreach ($k in [SS]::Children($hwnd)) {
        $r = [SS]::RectStr($k) -split ','
        $w = [int]$r[2] - [int]$r[0]; $hh = [int]$r[3] - [int]$r[1]
        if ([SS]::Cls($k) -eq 'AAPicturePlus2' -and [SS]::IsWindowVisible($k) -and $hh -le 60 -and $hh -ge 30 -and $w -gt 1000) {
            return [pscustomobject]@{ H=$k; Left=[int]$r[0]; Top=[int]$r[1]; Width=$w; Height=$hh }
        }
    }
    return $null
}
function Click-MaxBtn {
    $tb = Get-Titlebar
    if ($tb -eq $null) { Write-Output '  (找不到标题栏)'; return }
    $btnW = [int][Math]::Round(46 * $scale)
    $cx = $tb.Left + $tb.Width - $btnW * 1.5
    $cy = $tb.Top + [int]($tb.Height / 2)
    $cpt = New-Object SS+POINT; $cpt.X = $cx; $cpt.Y = $cy
    [void][SS]::ScreenToClient($tb.H, [ref]$cpt)
    [SS]::Click($tb.H, $cpt.X, $cpt.Y)
}

Write-Output '=== 点「最大化/还原」按钮 ==='
Click-MaxBtn
Start-Sleep -Seconds 2
Report 'after-click-1'

Click-MaxBtn
Start-Sleep -Seconds 2
Report 'after-click-2'

Write-Output '=== 直接用 ShowWindow 切换（绕过按钮） ==='
[void][SS]::SendMessage($hwnd, 0x0, [IntPtr]0, [IntPtr]0)
Start-Sleep -Seconds 1
Report 'after-wmnull'

Write-Output '--- IDE 全部 clientWH 读数 ---'
Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like '*clientWH*' } | ForEach-Object { Write-Output ("  " + $_) }
Write-Output '--- IDE 全部 relayout 读数 ---'
Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like '*relayout*' } | ForEach-Object { Write-Output ("  " + $_) }
try { $p.Kill() } catch {}
