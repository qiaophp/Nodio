# ide_richedit_probe.ps1 — 直接问官方 richedit：「你到底有没有文本？」
#
# 截图里编辑区只看到行号、没有代码正文，行号栏是我们自绘的，
# 所以要区分两种可能：
#   (a) richedit 里**根本没有文本**（setText 没生效 / 打开文件没接上）
#   (b) 有文本，但前景色和背景色一样（看不见）→ 那是配色问题不是加载问题
#
# 办法：直接对 richedit 子窗口发 WM_GETTEXTLENGTH / WM_GETTEXT，
# 以及 EM_GETCHARFORMAT 读回当前文字色（发/收都用 cdata 缓冲）。
#
#   pwsh -File tests/ide_richedit_probe.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$DelayMs = 9000
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class RE {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
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
}
'@
[RE]::MakeDpiAware()

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
$hwnd = [RE]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
Start-Sleep -Seconds 3

Write-Output "主窗体 = $hwnd"
Write-Output '--- 所有子窗口（类名 / 矩形 / 可视）---'
$rich = @()
foreach ($k in [RE]::Children($hwnd)) {
    $cls = [RE]::Cls($k)
    $vis = [RE]::IsWindowVisible($k)
    Write-Output ("  {0,-18} {1,-24} vis={2}" -f $cls, [RE]::RectStr($k), $vis)
    if ($cls -like '*RichEdit*' -or $cls -like '*EDIT*') { $rich += $k }
}

Write-Output "--- richedit 候选: $($rich.Count) 个 ---"
foreach ($k in $rich) {
    $len = [RE]::SendMessage($k, 0x000E, [IntPtr]0, [IntPtr]0)   # WM_GETTEXTLENGTH
    $sb = New-Object System.Text.StringBuilder 4096
    [void][RE]::SendMessage($k, 0x000D, [IntPtr]4096, $sb)        # WM_GETTEXT
    $t = $sb.ToString()
    Write-Output ("  hwnd={0} 文本长度={1}" -f $k.ToInt64(), $len)
    if ($t.Length -gt 300) { $t = $t.Substring(0,300) + ' ...' }
    Write-Output ("    内容(前300字符)= [{0}]" -f ($t -replace "`r", '\r' -replace "`n", '\n'))

    # EM_GETCHARFORMAT(0x43A) 回读当前文字格式：
    # CHARFORMAT2W 里 textColor 在偏移 20（int，COLORREF 0x00BBGGRR），
    # mask 在偏移 4（0x40000000 = CFM_COLOR 表示颜色有效）。
    if ($len -gt 0) {
        $buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(116)
        try {
            for ($i = 0; $i -lt 116; $i++) { [System.Runtime.InteropServices.Marshal]::WriteByte($buf, $i, 0) }
            [System.Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, 116)   # cbSize
            [void][RE]::SendMessage($k, 0x0B0, [IntPtr]0, [IntPtr]0)           # EM_SETSEL(0,0)
            [void][RE]::SendMessage($k, 0x43A, [IntPtr]1, $buf)               # EM_GETCHARFORMAT(SCF_SELECTION)
            $mask = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, 4)
            $color = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, 20)
            $yH = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, 12)
            $c = $color -band 0xFFFFFF
            $r = $c -band 0xFF; $g = ($c -shr 8) -band 0xFF; $b = ($c -shr 16) -band 0xFF
            Write-Output ("    [文本区] 文字色 raw=0x{0:X8} → RGB({1},{2},{3})  mask=0x{4:X8} CFM_COLOR={5}  yHeight={6}" -f `
                $color, $r, $g, $b, $mask, (($mask -band 0x40000000) -ne 0), $yH)
            $nl = [RE]::SendMessage($k, 0x140, [IntPtr]0, [IntPtr]0)          # EM_GETLINECOUNT
            $tl = [RE]::SendMessage($k, 0x141, [IntPtr]0, [IntPtr]0)          # EM_GETTEXTLEN
            Write-Output ("    [文本区] 行数={0} EM_GETTEXTLEN={1}" -f $nl, $tl)
        } finally { [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
    }
}

Write-Output '--- IDE 日志尾部 ---'
Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -Last 15 | ForEach-Object { Write-Output ("  " + $_) }
try { $p.Kill() } catch {}
Remove-Item $outF -Force -ErrorAction SilentlyContinue
