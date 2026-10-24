# ide_winbtn.ps1 — 验自绘标题栏右侧的窗口按钮（最小化/最大化/还原/关闭）。
#
# 同时盯两件事：
#   · 画面：每次动作后截一张图（看有没有黑屏 / 残缺）
#   · 心跳：IDE 的心跳每 300ms 打一行 STATUS: —— 心跳停了 = 界面从此不再重绘
#
#   pwsh -File tests/ide_winbtn.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$DelayMs = 9000,
    [string]$ShotDir = 'D:\Nodio\build-x64\Release\winbtn'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WB {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    public static List<IntPtr> Children(IntPtr root) {
        var list = new List<IntPtr>();
        EnumChildWindows(root, (h,l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string RectStr(IntPtr h) { RECT r; GetWindowRect(h, out r); return string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B); }
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
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
    public static void Click(IntPtr h, int x, int y) {
        IntPtr lp = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(h, 0x200, IntPtr.Zero, lp);
        SendMessage(h, 0x201, new IntPtr(1), lp);
        SendMessage(h, 0x202, IntPtr.Zero, lp);
    }
}
'@
[WB]::MakeDpiAware()

if (-not (Test-Path $ShotDir)) { New-Item -ItemType Directory -Path $ShotDir -Force | Out-Null }
Get-ChildItem $ShotDir -Filter '*.png' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG      = '1'
$env:NODIO_IDE_DEMO      = '1'
$env:NODIO_IDE_DEMO_FILE = 'D:\Nodio\ide\nodio\app\shell.nodio'
$env:NODIO_IDE_DEMO_TAB  = 'source'

$outF = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
Start-Sleep -Milliseconds $DelayMs
$hwnd = [WB]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }

$dpi = 96; try { $d = [WB]::GetDpiForWindow($hwnd); if ($d -gt 0) { $dpi = $d } } catch {}
$scale = $dpi / 96.0

function Get-StatusCount {
    if (-not (Test-Path $outF)) { return 0 }
    return @(Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like 'STATUS:*' }).Count
}
function Save-Shot([string]$name) {
    $rc = New-Object WB+RECT
    [void][WB]::GetWindowRect($hwnd, [ref]$rc)
    $w = $rc.R - $rc.L; $h = $rc.B - $rc.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][WB]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $ShotDir ($name + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function Click-Logical([int]$lx, [int]$ly) {
    $px = [int][Math]::Round($lx * $scale)
    $py = [int][Math]::Round($ly * $scale)
    $target = [WB]::ChildWindowFromPointEx($hwnd, (New-Object WB+POINT -Property @{X=$px;Y=$py}), 0)
    $cpt = New-Object WB+POINT; $cpt.X = $px; $cpt.Y = $py
    [void][WB]::ScreenToClient($target, [ref]$cpt)
    [WB]::Click($target, $cpt.X, $cpt.Y)
}

function Get-ClientPhysW {
    $rc = New-Object WB+RECT
    [void][WB]::GetWindowRect($hwnd, [ref]$rc)
    return ($rc.R - $rc.L)
}
function Show-Tops([uint32]$pid_) {
    $list = @()
    $cb = [WB+EnumProc]{
        param($h, $l)
        $pid2 = 0
        [void][WB]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $pid_) {
            $rc = New-Object WB+RECT
            [void][WB]::GetWindowRect($h, [ref]$rc)
            $list += ("hwnd={0} size={1}x{2} vis={3}" -f $h.ToInt64(), ($rc.R-$rc.L), ($rc.B-$rc.T), [WB]::IsWindowVisible($h))
        }
        return $true
    }
    [void][WB]::EnumWindows($cb, [IntPtr]::Zero)
    return $list
}

# 按钮位置**从真实画板矩形推**（不要自己算窗口宽度：
# 最大化时客户区左边有 10 物理像素的可缩放边框，算出来的位置会偏 100 多像素，
# 点下去就落到「最小化」上）。
function Get-TitlebarRect {
    $best = $null
    foreach ($k in [WB]::Children($hwnd)) {
        $r = [WB]::RectStr($k)
        $p = $r -split ','
        $w = [int]$p[2] - [int]$p[0]
        $hh = [int]$p[3] - [int]$p[1]
        if ([WB]::Cls($k) -eq 'AAPicturePlus2' -and [WB]::IsWindowVisible($k) -and $hh -le 60 -and $hh -ge 30 -and $w -gt 1000) {
            if ($best -eq $null -or $w -gt $best.Width) {
                $best = [pscustomobject]@{ H=$k; Left=[int]$p[0]; Top=[int]$p[1]; Width=$w; Height=$hh }
            }
        }
    }
    return $best
}

# 等窗口稳定 + 最大化完成
Start-Sleep -Seconds 2
Save-Shot '00_before'
$st0 = Get-StatusCount
Write-Output ("before: zoomed={0} clientPhysW={1} statusLines={2}" -f [WB]::IsZoomed($hwnd), (Get-ClientPhysW), $st0)

Write-Output '--- 点「最大化/还原」按钮两次 ---'
for ($i = 1; $i -le 2; $i++) {
    $tb = Get-TitlebarRect
    if ($tb -eq $null) { Write-Output 'no titlebar found'; break }
    $btnW = [int][Math]::Round(46 * $scale)      # 按钮宽（逻辑 46 → 物理）
    $cx = $tb.Left + $tb.Width - $btnW * 1.5     # 中间那个 = 最大化/还原
    $cy = $tb.Top + [int]($tb.Height / 2)
    $target = $tb.H
    $cpt = New-Object WB+POINT; $cpt.X = $cx; $cpt.Y = $cy
    [void][WB]::ScreenToClient($target, [ref]$cpt)
    [WB]::Click($target, $cpt.X, $cpt.Y)
    Start-Sleep -Seconds 2
    $st = Get-StatusCount
    Save-Shot ("0{0}_after" -f $i)
    Write-Output ("after click {0}: zoomed={1} clientPhysW={2} statusLines={3} (心跳增量={4})" -f `
        $i, [WB]::IsZoomed($hwnd), (Get-ClientPhysW), $st, ($st - $st0))
    $now = [WB]::Biggest([uint32]$p.Id)
    Write-Output ("   原hwnd={0} isWindow={1}  当前最大窗={2} 同窗={3}" -f `
        $hwnd.ToInt64(), [WB]::IsWindow($hwnd), $now.ToInt64(), ($now -eq $hwnd))
    # ⚠ **不要**在这里调 RedrawWindow(RDW_ALLCHILDREN|RDW_UPDATENOW)：
    #   官方 plus 的 WM_PAINT 只画「本次失效区域」，整树强制重画会把
    #   所有自绘板清成空白（实测截图非黑像素 123474 → 57068）。
    #   IDE 自己的 redrawAll() 才是对的修法。
    Start-Sleep -Milliseconds 800
    Save-Shot ("0{0}_after_redraw" -f $i)
    $st0 = $st
}

Write-Output '--- 直发 WM_CLOSE（官方 form 的关闭流程）---'
[void][WB]::SendMessage($hwnd, 0x10, [IntPtr]0, [IntPtr]0)
Start-Sleep -Seconds 2
Write-Output ("after WM_CLOSE: alive={0}" -f (-not $p.HasExited))

Write-Output '--- DIAG 尾部 ---'
if (Test-Path $outF) {
    Copy-Item $outF (Join-Path $ShotDir 'stdout.txt') -Force
    Get-Content $outF -Encoding UTF8 | Select-Object -Last 12 | ForEach-Object { Write-Output $_ }
}
Write-Output '--- 心跳计数 ---'
if (Test-Path $outF) { Get-Content $outF -Encoding UTF8 | Where-Object { $_ -like 'DIAG hb=*' } | Select-Object -Last 5 | ForEach-Object { Write-Output $_ } }
Write-Output '--- relayout 次数 ---'
if (Test-Path $outF) { Write-Output ("relayout lines = " + @(Get-Content $outF -Encoding UTF8 | Where-Object { $_ -like '*relayout*' }).Count) }
try { $p.Kill() } catch {}
Remove-Item $outF -Force -ErrorAction SilentlyContinue
