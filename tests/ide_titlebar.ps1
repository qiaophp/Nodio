# ide_titlebar.ps1 — 自绘标题栏的回归检查。
#
#   pwsh -NoProfile -File tests/ide_titlebar.ps1 [-OutDir <dir>]
#
# 做两件事：
#   1) 跑 tests/titlebar_probe.aardio（进程内直接发鼠标消息），
#      断言「拖动窗口」「弹出下拉菜单」「点菜单项触发命令」三条都成立；
#   2) 起 IDE 抓两张截图：普通态 + 最大化态，供人工复核外观。
#
# 为什么不用合成鼠标事件（mouse_event/SendInput）点真窗口：
#   本机桌面会话里合成事件送不进目标进程（点页签、点标题栏都收不到），
#   测出来的结果不可信。进程内发消息走的是同一条消息链，结果确定。
param(
    [string]$OutDir = "$PSScriptRoot\..\build-x64\Release",
    [int]   $DelayMs = 3000
)
$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
if (-not (Test-Path $exe)) { Write-Output "FAIL: exe not found: $exe"; exit 1 }

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'

# ---------- 1) 行为探针 ----------
$probe = Join-Path $PSScriptRoot 'titlebar_probe.aardio'
$out = [IO.Path]::GetTempFileName()
$err = [IO.Path]::GetTempFileName()
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables['NODIO_RUN'] = $probe
$psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$psi.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
$psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = 'D:\Nodio\lib'
$p = [System.Diagnostics.Process]::Start($psi)
$oTask = $p.StandardOutput.ReadToEndAsync()
$eTask = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(30000)) { try { $p.Kill() } catch {}; Write-Output 'FAIL: probe timed out'; exit 1 }
$probeOut = ($oTask.Result + $eTask.Result)
Write-Output '--- titlebar probe ---'
Write-Output $probeOut.Trim()
$fail = 0
foreach ($expect in @('DRAG-OK', 'MENU-OPEN', 'MENU-FIRED', 'PROBE-OK')) {
    if ($probeOut -notmatch [regex]::Escape($expect)) { Write-Output "FAIL: probe missing $expect"; $fail = 1 }
}

# ---------- 2) 外观截图 ----------
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class TB32 {
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

function Save-Shot([IntPtr]$hwnd, [string]$path) {
    $rc = New-Object TB32+RECT
    [void][TB32]::GetWindowRect($hwnd, [ref]$rc)
    $dpi = 0
    try { $dpi = [TB32]::GetDpiForWindow($hwnd) } catch {}
    if ($dpi -le 0) { $dpi = 96 }
    $scale = $dpi / 96.0
    $pw = [int][Math]::Round(($rc.Right - $rc.Left) * $scale)
    $ph = [int][Math]::Round(($rc.Bottom - $rc.Top) * $scale)
    $bmp = New-Object System.Drawing.Bitmap $pw, $ph
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [TB32]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    if (-not $ok) {
        try {
            $g2 = [System.Drawing.Graphics]::FromImage($bmp)
            $g2.CopyFromScreen($rc.Left, $rc.Top, 0, 0, (New-Object System.Drawing.Size($pw, $ph)))
            $g2.Dispose()
        } catch {}
    }
    $full = [IO.Path]::GetFullPath($path)
    $dir = Split-Path -Parent $full
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $full
}

$env:NODIO_RUN = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DEMO = '1'
$env:NODIO_IDE_DEMO_TAB = 'gallery'
$stdoutFile = [IO.Path]::GetTempFileName()
$app = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError ([IO.Path]::GetTempFileName())
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $app.Refresh()
    if ($app.HasExited) { Write-Output 'FAIL: IDE exited early'; exit 1 }
    $hwnd = [TB32]::BiggestWindow([uint32]$app.Id)
    if ($hwnd -ne [IntPtr]::Zero) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { try { $app.Kill() } catch {}; Write-Output 'FAIL: no main window'; exit 1 }

[void][TB32]::ShowWindow($hwnd, 9)     # SW_RESTORE
[void][TB32]::BringWindowToTop($hwnd)
[void][TB32]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040)
[void][TB32]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds $DelayMs
Write-Output ('SHOT-RESTORED: ' + (Save-Shot $hwnd (Join-Path $OutDir 'shot_tb_restored.png')))

[void][TB32]::ShowWindow($hwnd, 3)     # SW_MAXIMIZE
Start-Sleep -Milliseconds 1200
$rcMax = New-Object TB32+RECT
[void][TB32]::GetWindowRect($hwnd, [ref]$rcMax)
Write-Output ("MAXIMIZED-RECT: {0},{1} {2}x{3}" -f $rcMax.Left, $rcMax.Top, ($rcMax.Right-$rcMax.Left), ($rcMax.Bottom-$rcMax.Top))
Write-Output ('SHOT-MAX: ' + (Save-Shot $hwnd (Join-Path $OutDir 'shot_tb_max.png')))
[void][TB32]::ShowWindow($hwnd, 9)     # SW_RESTORE
Start-Sleep -Milliseconds 800

try { $app.Kill() } catch {}
try { Remove-Item $stdoutFile -ErrorAction SilentlyContinue } catch {}
if ($fail -eq 0) { Write-Output 'TITLEBAR OK' } else { Write-Output 'TITLEBAR FAILED' }
exit $fail
