# ide_shot.ps1 — 抓 IDE 窗口截图，供「按截图自绘」做视觉回归。
#
#   powershell -NoProfile -File tests/ide_shot.ps1 [-Out shot.png] [-DelayMs 2500] [-Script <file>]
#
# 原理：启动 nodio.exe 跑一个脚本（默认 IDE 主程序），等主窗口出现后
# 按窗口矩形抓屏存 PNG。窗口是自绘的，光看 stdout 断言看不出画面对不对，
# 这一步是自绘外观迭代的反馈闭环。
param(
    [string]$Out      = "$PSScriptRoot\..\build-x64\Release\ide_shot.png",
    [string]$Script   = 'D:\Nodio\ide\nodio\main.nodio',
    [int]   $DelayMs  = 3500,
    [string]$Title    = 'Nodio IDE'
)

$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
if (-not (Test-Path $exe)) { Write-Output "FAIL: exe not found: $exe"; exit 1 }

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $exe
$env:NODIO_RUN           = $Script

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    // 进程里最大的可见顶层窗口 —— MainWindowHandle 偶尔会抓到
    // tooltip / IME 这类小窗口，抓出来的图就是错的
    public static IntPtr BiggestWindow(uint want) {
        IntPtr best = IntPtr.Zero; long bestArea = 0;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want || !IsWindowVisible(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (a > bestArea) { bestArea = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@

$stdoutFile = [IO.Path]::GetTempFileName()
$stderrFile = [IO.Path]::GetTempFileName()
$env:NODIO_IDE_DIAG = '1'
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $p.Refresh()
    if ($p.HasExited) { Write-Output "FAIL: process exited early (code $($p.ExitCode))"; exit 1 }
    # 取进程里最大的可见顶层窗口（MainWindowHandle 偶尔会抓到 tooltip 小窗口）
    $hwnd = [Win32Shot]::BiggestWindow([uint32]$p.Id)
    if ($hwnd -ne [IntPtr]::Zero) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { try { $p.Kill() } catch {}; Write-Output 'FAIL: no main window'; exit 1 }

[void][Win32Shot]::ShowWindow($hwnd, 9)   # SW_RESTORE
[void][Win32Shot]::BringWindowToTop($hwnd)
[void][Win32Shot]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040) # HWND_TOPMOST|NOMOVE|NOSIZE|SHOWWINDOW
[void][Win32Shot]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds $DelayMs

$rc = New-Object Win32Shot+RECT
if (-not [Win32Shot]::GetWindowRect($hwnd, [ref]$rc)) {
    try { $p.Kill() } catch {}; Write-Output 'FAIL: GetWindowRect'; exit 1
}
$w = $rc.Right - $rc.Left
$h = $rc.Bottom - $rc.Top
if ($w -le 0 -or $h -le 0) { try { $p.Kill() } catch {}; Write-Output 'FAIL: empty window rect'; exit 1 }

# DPI：本脚本进程可能不是 DPI 感知的，GetWindowRect 拿到的是**逻辑**坐标，
# 而 IDE 窗口是按物理像素建的（系统 DPI 150%），直接用逻辑尺寸做位图
# 会把窗口右下角裁掉（表现为「底栏不见了」）。这里按窗口 DPI 还原物理尺寸。
$dpi = 0
try { $dpi = [Win32Shot]::GetDpiForWindow($hwnd) } catch { $dpi = 0 }
if ($dpi -le 0) { $dpi = 96 }
$scale = $dpi / 96.0
$pw = [int][Math]::Round($w * $scale)
$ph = [int][Math]::Round($h * $scale)

$bmp = New-Object System.Drawing.Bitmap $pw, $ph
$g = [System.Drawing.Graphics]::FromImage($bmp)
# PrintWindow 直接问窗口要自己的位图，不受遮挡/置顶影响
# flag 2 = PW_RENDERFULLCONTENT（DWM 合成窗口也能拿到）
$hdc = $g.GetHdc()
$ok = [Win32Shot]::PrintWindow($hwnd, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) {
    # 退回到屏幕抓取
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.CopyFromScreen($rc.Left, $rc.Top, 0, 0, (New-Object System.Drawing.Size($pw, $ph)))
    $g2.Dispose()
}
$outFull = [IO.Path]::GetFullPath($Out)
$dir = Split-Path -Parent $outFull
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$bmp.Save($outFull, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

try { $p.Kill() } catch {}
Write-Output ("SHOT: {0} ({1}x{2} logical, {3}x{4} physical @ {5}dpi) printwindow={6}" -f $outFull, $w, $h, $pw, $ph, $dpi, $ok)
# 把 IDE 的布局诊断一起带出来（几何与客户区尺寸是否自洽）
$diag = @()
if (Test-Path $stdoutFile) {
    $diag = Get-Content $stdoutFile -Encoding UTF8 -ErrorAction SilentlyContinue |
            Where-Object { $_ -like 'DIAG*' }
    if ($diag.Count -gt 0) { Write-Output '--- IDE layout diagnostics ---'; $diag | Select-Object -First 12 | ForEach-Object { Write-Output $_ } }
}
try { Remove-Item $stdoutFile, $stderrFile -ErrorAction SilentlyContinue } catch {}
exit 0
