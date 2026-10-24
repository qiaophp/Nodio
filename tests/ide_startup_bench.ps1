# ide_startup_bench.ps1 — IDE 启动耗时 + 交互卡顿量化。
#
# 目的：回答「IDE 打开太卡」到底是
#   (a) 引擎/VM 本身慢（脚本场景基线），
#   (b) 启动时**编译源码模块**的代价（aardio 源码每次 import 都要编译），
#   还是
#   (c) 窗口出来之后 UI 线程被占住（LSP 握手 / 工程扫描 / 语法着色 / 心跳重排）。
#
# 关键指标：**UI 线程响应延迟** —— 每 200ms 给主窗口发一条 WM_NULL（同步），
# 往返时间就是 UI 线程的阻塞时长。>200ms 就是用户能感觉到的「卡」。
#
#   pwsh -File tests/ide_startup_bench.ps1
param(
    [string]$Exe  = 'D:\Nodio\build-x64\Release\nodio.exe',
    [int]$Rounds = 3,
    [int]$WatchMs = 20000
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class SB {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("kernel32.dll")] public static extern uint GetTickCount();
    public static uint Tick() { return GetTickCount(); }
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
    public static long Ping(IntPtr h) {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        SendMessage(h, 0x0000/*WM_NULL*/, IntPtr.Zero, IntPtr.Zero);
        sw.Stop();
        return sw.ElapsedMilliseconds;
    }
}
'@
[SB]::MakeDpiAware()

$env:NODIO_LIBDIR        = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE           = $Exe

$tmp = Join-Path $env:TEMP ('nodio_bench_' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Measure-Script([string]$name, [string]$code) {
    $f = Join-Path $tmp ($name + '.aardio')
    [IO.File]::WriteAllText($f, $code, (New-Object Text.UTF8Encoding($false)))
    $times = @()
    for ($i = 0; $i -lt $Rounds; $i++) {
        $outF = Join-Path $tmp ($name + '_' + $i + '.out')
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
        $p.WaitForExit(60000) | Out-Null
        $sw.Stop()
        $times += $sw.ElapsedMilliseconds
        if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    }
    $min = ($times | Measure-Object -Minimum).Minimum
    $avg = [math]::Round((($times | Measure-Object -Average).Average))
    Write-Host ("{0,-12} runs={1}  min={2}ms avg={3}ms   [{4}]" -f $name, $Rounds, $min, $avg, ($times -join ','))
}

$hello = @'
io.print("hello");
'@

$libimport = @'
import win.ui;
import win.ui.menu;
import gdip;
import nodio.ui.board;
import nodio.ui.dock;
import nodio.ui.toolbar;
import nodio.ui.statusbar;
import nodio.ui.projectTree;
import nodio.ui.searchPanel;
import nodio.ui.tabstrip;
import nodio.ui.list;
import nodio.ui.tree;
import nodio.ui.theme;
import nodio.ui.dpi;
import nodio.ui.titlebar;
import nodio.ui.editor;
import nodio.ui.filedlg;
import nodio.ui.prompt;
import nodio.gui.gallery;
io.print("imports ok");
'@

Write-Host '=== 脚本场景（进程跑完即止）==='
Measure-Script 'hello' $hello
Measure-Script 'libimport' $libimport

Write-Host ''
Write-Host '=== 完整 IDE：窗口出现耗时 + UI 线程响应延迟 ==='
$env:NODIO_RUN = 'D:\Nodio\ide\nodio\main.nodio'
for ($i = 0; $i -lt $Rounds; $i++) {
    $outF = Join-Path $tmp ('ide_' + $i + '.out')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $launchTick = [SB]::Tick()
    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $winMs = -1
    $winTick = 0
    $lat = @()
    $stamps = @()
    while ($sw.ElapsedMilliseconds -lt $WatchMs) {
        Start-Sleep -Milliseconds 150
        $h = [SB]::Biggest([uint32]$p.Id)
        if ($h -eq [IntPtr]::Zero) { continue }
        if ($winMs -lt 0) { $winMs = $sw.ElapsedMilliseconds; $winTick = [SB]::Tick() }
        $t = [SB]::Ping($h)
        if ($t -ge 0) { $lat += [int]$t; $stamps += [int]$sw.ElapsedMilliseconds }
    }
    $cpu = 0
    try { $cpu = [math]::Round($p.TotalProcessorTime.TotalMilliseconds) } catch {}
    $mx = 0; $p95 = 0; $over200 = 0
    if ($lat.Count) {
        $s = $lat | Sort-Object
        $mx = $s[-1]
        $p95 = $s[[int]($s.Count * 0.95)]
        $over200 = ($lat | Where-Object { $_ -gt 200 }).Count
    }
    Write-Host ("ide#{0}  窗口出现={1}ms  UI延迟 max={2}ms p95={3}ms  >200ms次数={4}/{5}  进程CPU={6}ms" -f `
        $i, $winMs, $mx, $p95, $over200, $lat.Count, $cpu)
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    Start-Sleep -Milliseconds 300
    Write-Host ("        launchTick={0} windowTick={1} (进程启动到窗口可见 {2}ms)" -f $launchTick, $winTick, ($winTick - $launchTick))
    # 卡顿发生在什么时刻 —— 和 IDE 内部时间线对齐才有用
    for ($k = 0; $k -lt $lat.Count; $k++) {
        if ($lat[$k] -gt 200) {
            Write-Host ("        卡顿 {0}ms 出现在启动后 {1}ms" -f $lat[$k], $stamps[$k])
        }
    }
}
Write-Host ''
Write-Host '=== IDE 内部启动时间线（DIAG 模式）==='
$env:NODIO_IDE_DIAG = '1'
$outF = Join-Path $tmp 'ide_timeline.out'
$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
Start-Sleep -Seconds 8
if (-not $p.HasExited) { try { $p.Kill() } catch {} }
Start-Sleep -Milliseconds 400
if (Test-Path $outF) {
    Get-Content $outF -Encoding UTF8 -ErrorAction SilentlyContinue |
        Where-Object { $_ -like 'DIAG boot*' } |
        ForEach-Object { Write-Output ("  " + $_) }
}
