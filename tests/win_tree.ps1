# win_tree.ps1 — 枚举目标进程的所有子窗口（层次 + 类名 + 矩形 + 可见性）。
#
# 用途：自绘控件（plus 板）如果**没有自己的 HWND**（直绘到窗体背景），
# 就既画不出来也**收不到鼠标** —— 表现是「整片点不动」。
# 枚举一遍就能看出哪些交互元素根本没有窗口。
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Run  = 'D:\Nodio\ide\nodio\main.nodio',
    [int]$WaitMs  = 9000
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WT {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
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
    public static List<string> Children(IntPtr root) {
        var outp = new List<string>();
        EnumChildWindows(root, (h,l) => {
            RECT r; GetWindowRect(h, out r);
            var sb = new StringBuilder(256); GetClassName(h, sb, 256);
            outp.Add(string.Format("{0}\t{1}\t{2}\tvis={3}\tparent={4}",
                h.ToInt64(), sb.ToString(),
                string.Format("{0},{1},{2},{3}", r.L, r.T, r.R, r.B),
                IsWindowVisible(h) ? 1 : 0,
                GetParent(h).ToInt64()));
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
'@

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe
$env:NODIO_RUN           = $Run

$outF = [IO.Path]::GetTempFileName()
$errF = "$outF.err"
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError $errF
Start-Sleep -Milliseconds $WaitMs
$hwnd = [WT]::Biggest([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'NO-WINDOW'; try { $p.Kill() } catch {}; exit 1 }
Write-Output ("ROOT {0}" -f $hwnd.ToInt64())
$rows = [WT]::Children($hwnd)
Write-Output ("CHILD-COUNT {0}" -f $rows.Count)
$rows | ForEach-Object { Write-Output $_ }
try { $p.Kill() } catch {}
Remove-Item $outF, $errF -Force -ErrorAction SilentlyContinue
