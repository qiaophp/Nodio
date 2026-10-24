# tests/ide_resize_check.ps1 — 改尺寸压力测试：量化「卡顿 + 闪」。
#
# 场景：拖动窗口大小时 Windows 连发几十个 WM_SIZE。
#   修之前：每个 WM_SIZE 都全量重排（布局 15ms + redrawAll 94ms = 110ms 同步阻塞）
#           ⇒ 一次拖拽阻塞 1~2 秒，界面一块块跳出来。
#   修之后：onSize 只记尺寸，真正的重排合并到 50ms 一拍 ⇒ 阻塞总时长大降。
#
# 这里用 SetWindowPos 连续改 12 次尺寸，量：
#   · 触发的重排次数（从 IDE 自己的 DIAG 日志里数）
#   · 全部改完到布局稳定用时
#   · 最后的窗口/编辑区矩形是否正确
param(
    [int]$WaitMs = 9000,
    [int]$Steps = 12
)
$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE = $exe
$env:NODIO_RUN = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG = '1'
$env:NODIO_IDE_PROJECT = 'D:\Nodio\ide\nodio\ide.nproj'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassName(h,sb,256); return sb.ToString(); }
}
'@

$stdoutFile = [IO.Path]::GetTempFileName()
$errFile = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $errFile
try {
    Start-Sleep -Milliseconds $WaitMs
    $script:top = [IntPtr]::Zero; $script:big = 0
    $cb = [RS+EnumProc]{
        param($h,$l)
        [uint32]$procId = 0
        [RS]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
        if ($procId -eq $p.Id) {
            $r = New-Object RS+RECT
            [RS]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:big) { $script:big = $a; $script:top = $h }
        }
        return $true
    }
    [RS]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:top -eq [IntPtr]::Zero) { Write-Output 'FAIL: no IDE window'; exit 1 }
    $r0 = New-Object RS+RECT
    [RS]::GetWindowRect($script:top, [ref]$r0) | Out-Null
    if (($r0.Right-$r0.Left) -lt 400) { [void][RS]::ShowWindow($script:top, 3); Start-Sleep -Milliseconds 2000 }

    # 基线：重排前的日志行数
    $before = 0
    if (Test-Path $stdoutFile) { $before = (Get-Content $stdoutFile -Encoding UTF8 | Select-String -Pattern 'DIAG layout cost').Count }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Steps; $i++) {
        $w = 2400 + (($i % 4) * 260)
        $h = 1400 + (($i % 3) * 180)
        [void][RS]::SetWindowPos($script:top, [IntPtr]::Zero, 40, 40, $w, $h, 0x0004)  # SWP_NOZORDER
        Start-Sleep -Milliseconds 30
    }
    $sw.Stop()
    Write-Output ("RESIZE: {0} steps in {1}ms" -f $Steps, $sw.ElapsedMilliseconds)
    Start-Sleep -Milliseconds 2500

    $after = 0
    $costs = @()
    if (Test-Path $stdoutFile) {
        $all = Get-Content $stdoutFile -Encoding UTF8
        $m = $all | Select-String -Pattern 'DIAG layout cost'
        $after = $m.Count
        $costs = $m | Select-Object -Last 4 | ForEach-Object { $_.ToString() }
    }
    Write-Output ("LAYOUTS during resize: {0}" -f ($after - $before))
    $costs | ForEach-Object { Write-Output ("  " + $_) }

    $cr = New-Object RS+RECT
    [RS]::GetClientRect($script:top, [ref]$cr) | Out-Null
    $script:ed = [IntPtr]::Zero; $script:edArea = 0
    $cb2 = [RS+EnumProc]{
        param($h,$l)
        if ([RS]::Cls($h) -eq 'RICHEDIT50W') {
            $r = New-Object RS+RECT
            [RS]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:edArea) { $script:edArea = $a; $script:ed = $h }
        }
        return $true
    }
    [RS]::EnumChildWindows($script:top, $cb2, [IntPtr]::Zero) | Out-Null
    $er = New-Object RS+RECT
    [RS]::GetWindowRect($script:ed, [ref]$er) | Out-Null
    Write-Output ("FINAL client={0}x{1} editor={2}x{3}" -f ($cr.Right-$cr.Left), ($cr.Bottom-$cr.Top), ($er.Right-$er.Left), ($er.Bottom-$er.Top))
    $ok = ($cr.Right-$cr.Left) -gt 800 -and ($er.Right-$er.Left) -gt 500
    Write-Output ("RESIZE-CHECK: " + $(if ($ok) { 'OK' } else { 'BROKEN' }))
    if (-not $ok) { exit 1 }
}
catch { Write-Output ("RESIZE-CHECK: ERROR " + $_.Exception.Message); exit 1 }
finally { try { $p.Kill() } catch {} }
exit 0
