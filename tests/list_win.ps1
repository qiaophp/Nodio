# list_win.ps1 — 列出某个进程的所有顶层窗口（类名/尺寸/可见性）
param(
    [string]$ProcName = 'nodio',
    [int]$WaitMs = 6000
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WList {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public static string[] List(uint want) {
        var res = new System.Collections.Generic.List<string>();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want) return true;
            RECT r; GetWindowRect(h, out r);
            var cn = new StringBuilder(256); GetClassName(h, cn, 256);
            var tt = new StringBuilder(256); GetWindowText(h, tt, 256);
            res.Add(string.Format("{0} vis={1} rect={2},{3},{4},{5} size={6}x{7} class={8} text={9}",
                h, IsWindowVisible(h), r.Left, r.Top, r.Right, r.Bottom,
                r.Right-r.Left, r.Bottom-r.Top, cn, tt));
            return true;
        }, IntPtr.Zero);
        return res.ToArray();
    }
}
'@
$procs = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
if (-not $procs) { Write-Output "no process $ProcName"; exit 1 }
$p = $procs | Sort-Object StartTime | Select-Object -Last 1
Start-Sleep -Milliseconds $WaitMs
Write-Output ("--- pid {0} ---" -f $p.Id)
[WList]::List([uint32]$p.Id) | ForEach-Object { Write-Output $_ }
