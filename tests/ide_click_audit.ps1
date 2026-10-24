# ide_click_audit.ps1 — IDE 全 UI 点击审计。
#
# 目的：把「按钮基本不可点 / 选项卡没法切换」变成**可复现的清单**。
#
# 做法：
#   1) 启动 IDE（诊断模式，编辑器页签）；
#   2) 逐个在已知坐标上注入点击（直接发窗口消息，锁屏也准）；
#   3) 每点一次，截「点击前 / 点击后」两张图，交给 ide_audit_diff.py
#      做**局部区域**像素差分（整窗差分噪声太大：光标闪烁、状态栏心跳）。
#
# 坐标是**逻辑像素**（客户区），150% 下自动换算成物理像素。
# 每轮写进**独立目录**（audit_<时间戳>），避免旧图残留导致配对错位。
#
#   pwsh -File tests/ide_click_audit.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Out  = 'D:\Nodio\build-x64\Release\ide_audit.png',
    [int]$DelayMs = 9000,
    [int]$WaitAfterClick = 900
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CA {
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
    [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr p, POINT pt, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr GetCapture();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
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
    public static IntPtr ChildAt(IntPtr root, int x, int y) {
        var pt = new POINT(); pt.X = x; pt.Y = y;
        return ChildWindowFromPointEx(root, pt, 0);
    }
    public static IntPtr Post(IntPtr h, uint msg, IntPtr wp, IntPtr lp) {
        return SendMessage(h, msg, wp, lp);
    }
    public static void Click(IntPtr h, int x, int y) {
        IntPtr lp = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(h, 0x200/*WM_MOUSEMOVE*/, new IntPtr(0), lp);
        SendMessage(h, 0x201/*WM_LBUTTONDOWN*/, new IntPtr(1), lp);
        SendMessage(h, 0x202/*WM_LBUTTONUP*/, new IntPtr(0), lp);
    }
    public static string Cls(IntPtr h) {
        var sb = new StringBuilder(256); GetClassName(h, sb, 256);
        return sb.ToString();
    }
    public static List<IntPtr> Tops(uint want) {
        var list = new List<IntPtr>();
        EnumWindows((h,l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want && IsWindowVisible(h)) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }
    public static void CloseWindow(IntPtr h) {
        SendMessage(h, 0x0111/*WM_COMMAND*/, new IntPtr(2/*IDCANCEL*/), IntPtr.Zero);
        SendMessage(h, 0x0010/*WM_CLOSE*/, IntPtr.Zero, IntPtr.Zero);
    }
}
'@

[CA]::MakeDpiAware()

# ===== 审计点（逻辑像素，客户区）=====
# 名字, x, y
$POINTS = @(
    @('工具条-新建',        20,  46),
    @('工具条-打开',        49,  46),
    @('工具条-保存',        79,  46),
    @('工具条-运行',       119,  46),
    @('工具条-调试',       149,  46),
    @('工具条-设计器',     179,  46),
    @('左栏页签-工程',       20,  75),
    @('左栏页签-属性',       80,  75),
    @('左栏页签-搜索',      160,  75),
    @('左栏页签-大纲',      240,  75),
    @('右栏页签-控件',     2220,  75),
    @('右栏页签-标准库',   2330,  75),
    @('右栏页签-组件',     2530,  75),
    @('底栏页签-问题',      280, 1200),
    @('底栏页签-输出',      340, 1200),
    @('文档页签-第一个',    300,  73),
    @('工程树-根节点',       30, 100),
    @('编辑器-正文',        400, 150),
    @('标题栏-文件',         57,  15),
    @('标题栏-编辑',        150,  15),
    @('标题栏-运行',        440,  15)
)

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG      = '1'
$env:NODIO_IDE_DEMO      = '1'
$env:NODIO_IDE_DEMO_FILE = 'D:\Nodio\ide\nodio\app\shell.nodio'
$env:NODIO_IDE_DEMO_TAB  = 'source'

$outF = [IO.Path]::GetTempFileName()
$errF = "$outF.err"
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError $errF
Start-Sleep -Milliseconds $DelayMs
$hwnd = [CA]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
[void][CA]::ShowWindow($hwnd, 5)
[void][CA]::SetWindowPos($hwnd, [IntPtr](-1), 0,0,0,0, 0x0001 -bor 0x0002 -bor 0x0040)
[void][CA]::BringWindowToTop($hwnd)
[void][CA]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 500

$dpi = 0; try { $dpi = [CA]::GetDpiForWindow($hwnd) } catch {}
if ($dpi -le 0) { $dpi = 96 }
$scale = $dpi / 96.0

function Get-LastStatus {
    if (-not (Test-Path $outF)) { return '' }
    $lines = Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue |
             Where-Object { $_ -like 'STATUS:*' }
    if ($lines) { return ($lines[-1] -replace '^STATUS:', '') }
    return ''
}

# 每轮独立目录：旧图不会和新图配错
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$shotDir = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) ("audit_" + $stamp)
New-Item -ItemType Directory -Path $shotDir -Force | Out-Null

$manifest = Join-Path $shotDir 'manifest.txt'
"dir=$shotDir" | Out-File -FilePath $manifest -Encoding UTF8

Write-Output ("INIT-STATUS: " + (Get-LastStatus))
Write-Output ("SHOTDIR: " + $shotDir)
Write-Output '=== 点击审计（局部像素差分）==='

$idx = 0
foreach ($pt in $POINTS) {
    $name = $pt[0]; $lx = [int]$pt[1]; $ly = [int]$pt[2]
    $idx++
    $px = [int][Math]::Round($lx * $scale)
    $py = [int][Math]::Round($ly * $scale)

    $rc = New-Object CA+RECT
    [void][CA]::GetWindowRect($hwnd, [ref]$rc)
    $w = $rc.R - $rc.L; $h = $rc.B - $rc.T
    if ($w -le 0 -or $h -le 0) { $w = 1600; $h = 900 }

    # 点击前
    $before = Get-LastStatus
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][CA]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $shotDir ("audit_{0:d2}_a.png" -f $idx)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    # 注入点击（发给命中子窗口；坐标换算成它的客户区坐标）
    $topsBefore = [CA]::Tops([uint32]$p.Id)
    $target = [CA]::ChildAt($hwnd, $px, $py)
    $cap = [CA]::GetCapture()
    if ($cap -ne [IntPtr]::Zero) { $target = $cap }
    $cpt = New-Object CA+POINT; $cpt.X = $px; $cpt.Y = $py
    [void][CA]::ScreenToClient($target, [ref]$cpt)
    [CA]::Click($target, $cpt.X, $cpt.Y)
    Start-Sleep -Milliseconds $WaitAfterClick

    # 有些按钮会拉起**模态对话框**（如「打开」的文件对话框）：
    # 对话框会盖住整个 IDE，后面的截图就全是对话框了 —— 这里点完自动关掉。
    foreach ($t in [CA]::Tops([uint32]$p.Id)) {
        if ($topsBefore -notcontains $t -and $t -ne $hwnd) {
            [CA]::CloseWindow($t)
            Start-Sleep -Milliseconds 400
        }
    }

    # 点击后
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][CA]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $shotDir ("audit_{0:d2}_b.png" -f $idx)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    $now = Get-LastStatus
    "$idx`t$name`t$lx`t$ly`t$px`t$py`t$target`t$before`t$now" | Out-File -FilePath $manifest -Encoding UTF8 -Append
    Write-Output ("CLICK {0,-18} ({1},{2}) target={3,-16} status={4}" -f $name, $lx, $ly, [CA]::Cls($target), $now)
}

# 收尾截图
$rc = New-Object CA+RECT
[void][CA]::GetWindowRect($hwnd, [ref]$rc)
$w = $rc.R - $rc.L; $h = $rc.B - $rc.T
if ($w -gt 0 -and $h -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][CA]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save([IO.Path]::GetFullPath($Out), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output ("SHOT: " + [IO.Path]::GetFullPath($Out))
}
try { $p.Kill() } catch {}
Remove-Item $outF, $errF -Force -ErrorAction SilentlyContinue
Write-Output ("MANIFEST: " + $manifest)
