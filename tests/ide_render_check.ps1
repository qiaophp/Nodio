# tests/ide_render_check.ps1 — IDE 渲染的**屏幕像素级**验收。
#
# 为什么不用截图：PrintWindow 抓父窗时抓不到标准子控件（richedit）的
# 内容 —— 屏幕上有字、截图里是空的（已实测：GetPixel 读到语法色，
# 而父窗 PrintWindow 的那块是纯背景）。所以验收改成两路：
#   A. 直接读**屏幕像素**（GetDC + GetPixel）统计编辑器里有没有带颜色的字
#   B. 最小化 → 还原，确认客户区尺寸和编辑区矩形不塌（237x39 / 反向矩形）
param(
    [int]$WaitMs = 9000,
    [string]$Project = 'D:\Nodio\ide\nodio\ide.nproj'
)
$ErrorActionPreference = 'Stop'
$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE = $exe
$env:NODIO_RUN = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG = '1'
$env:NODIO_IDE_PROJECT = $Project

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr hdc);
  [DllImport("gdi32.dll")] public static extern int GetPixel(IntPtr hdc, int x, int y);
  public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassName(h,sb,256); return sb.ToString(); }
}
'@

$stdoutFile = [IO.Path]::GetTempFileName()
$errFile = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $errFile
$rc = 1
try {
    Start-Sleep -Milliseconds $WaitMs

    $script:top = [IntPtr]::Zero
    $script:seen = 0
    $script:big = 0
    $cb = [RC+EnumProc]{
        param($h,$l)
        [uint32]$procId = 0
        [RC]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
        if ($procId -eq $p.Id) {
            $script:seen = $script:seen + 1
            $r = New-Object RC+RECT
            [RC]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left) * ($r.Bottom-$r.Top)
            if ($a -gt $script:big) { $script:big = $a; $script:top = $h }
        }
        return $true
    }
    [RC]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    Write-Output ("windows-of-process={0} top={1}" -f $script:seen, $script:top)
    if ($script:top -eq [IntPtr]::Zero) { Write-Output 'FAIL: no IDE window'; exit 1 }
    # 光标停在「最小化」按钮上时环境会随机把窗口最小化 → 拉回来
    $r0 = New-Object RC+RECT
    [RC]::GetWindowRect($script:top, [ref]$r0) | Out-Null
    if (($r0.Right-$r0.Left) -lt 400) {
        Write-Output 'window was minimized by env; restoring'
        [void][RC]::ShowWindow($script:top, 3)
        Start-Sleep -Milliseconds 2500
    }

    # 找最大的 RICHEDIT50W 子窗口 = 编辑器
    $script:ed = [IntPtr]::Zero
    $script:edArea = 0
    $cb2 = [RC+EnumProc]{
        param($h,$l)
        if ([RC]::Cls($h) -eq 'RICHEDIT50W') {
            $r = New-Object RC+RECT
            [RC]::GetWindowRect($h, [ref]$r) | Out-Null
            $a = ($r.Right-$r.Left) * ($r.Bottom-$r.Top)
            if ($a -gt $script:edArea) { $script:edArea = $a; $script:ed = $h }
        }
        return $true
    }
    [RC]::EnumChildWindows($script:top, $cb2, [IntPtr]::Zero) | Out-Null
    if ($script:ed -eq [IntPtr]::Zero) { Write-Output 'FAIL: no richedit'; exit 1 }

    $er = New-Object RC+RECT
    [RC]::GetWindowRect($script:ed, [ref]$er) | Out-Null
    Write-Output ("EDITOR rect={0},{1} {2}x{3}" -f $er.Left,$er.Top,($er.Right-$er.Left),($er.Bottom-$er.Top))

    # ---- A. 屏幕像素：编辑器里到底有没有带颜色的字 ----
    $hdc = [RC]::GetDC($script:ed)
    $counts = @{}
    $black = 0; $texty = 0
    for ($y = 0; $y -lt 120; $y += 3) {
        for ($x = 0; $x -lt 900; $x += 1) {
            $c = [RC]::GetPixel($hdc, $x, $y)
            if ($c -lt 0) { continue }
            $key = '{0:X6}' -f $c
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
            if ($c -eq 0 -or $c -eq 2037005) { $black++ }
            if ($c -ne 1973790 -and $c -ne 2631720 -and $c -ne 0) { $texty++ }
        }
    }
    [void][RC]::ReleaseDC($script:ed, $hdc)
    $top5 = $counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6 | ForEach-Object { "$($_.Key)x$($_.Value)" }
    Write-Output ("PIXELS colors: " + ($top5 -join '  '))
    Write-Output ("PIXELS nonbg=$texty nearblack=$black")

    # ---- B. 最小化 → 还原，布局不许塌 ----
    # ⚠ 这台机器上光标停在标题栏「最小化」按钮上，环境会**随机点它**，
    #   窗口会被反复最小化 → 还原路径测不稳。
    #   所以这里只做「最小化 → 最大化拉回」并观察：
    #   关键是**最小化期间 IDE 不能拿退化尺寸去重排**（否则布局永久损坏）。
    $wr0 = New-Object RC+RECT
    [RC]::GetWindowRect($script:top, [ref]$wr0) | Out-Null
    Write-Output ("BEFORE win={0},{1} {2}x{3}" -f $wr0.Left,$wr0.Top,($wr0.Right-$wr0.Left),($wr0.Bottom-$wr0.Top))
    [void][RC]::ShowWindow($script:top, 6)   # SW_MINIMIZE
    Start-Sleep -Milliseconds 1500
    $crMini = New-Object RC+RECT
    [RC]::GetClientRect($script:top, [ref]$crMini) | Out-Null
    $wrMini = New-Object RC+RECT
    [RC]::GetWindowRect($script:top, [ref]$wrMini) | Out-Null
    Write-Output ("MINIMIZED client={0}x{1} win={2},{3} {4}x{5}" -f ($crMini.Right-$crMini.Left), ($crMini.Bottom-$crMini.Top), $wrMini.Left,$wrMini.Top,($wrMini.Right-$wrMini.Left),($wrMini.Bottom-$wrMini.Top))
    [void][RC]::ShowWindow($script:top, 3)   # SW_MAXIMIZE 拉回
    Start-Sleep -Milliseconds 2500
    $cr = New-Object RC+RECT
    [RC]::GetClientRect($script:top, [ref]$cr) | Out-Null
    $wr = New-Object RC+RECT
    [RC]::GetWindowRect($script:top, [ref]$wr) | Out-Null
    $er2 = New-Object RC+RECT
    [RC]::GetWindowRect($script:ed, [ref]$er2) | Out-Null
    Write-Output ("RESTORED client={0}x{1} win={2},{3} {4}x{5} editor={6}x{7}" -f ($cr.Right-$cr.Left), ($cr.Bottom-$cr.Top), $wr.Left,$wr.Top,($wr.Right-$wr.Left),($wr.Bottom-$wr.Top),($er2.Right-$er2.Left),($er2.Bottom-$er2.Top))
    $ok = ($cr.Right-$cr.Left) -gt 800 -and ($er2.Right-$er2.Left) -gt 500 -and ($er2.Bottom-$er2.Top) -gt 300
    Write-Output ("LAYOUT-AFTER-RESTORE: " + $(if ($ok) { 'OK' } else { 'BROKEN' }))

    # IDE 自己看到的客户区尺寸（最权威）
    if (Test-Path $stdoutFile) {
        $lines = Get-Content $stdoutFile -Encoding UTF8 -ErrorAction SilentlyContinue |
                 Select-String -Pattern 'clientWH|layout skip|relayout' |
                 Select-Object -Last 6 | ForEach-Object { $_.ToString() }
        if ($lines) { Write-Output '--- IDE 自己的窗口尺寸日志 ---'; $lines | ForEach-Object { Write-Output $_ } }
    }

    if ($texty -gt 200 -and $black -lt 50 -and $ok) { $rc = 0 }
    if ($rc -eq 0) { Write-Output 'RENDER-CHECK: OK' } else { Write-Output 'RENDER-CHECK: FAIL' }
}
catch {
    Write-Output ("RENDER-CHECK: ERROR " + $_.Exception.Message)
}
finally {
    try { $p.Kill() } catch {}
}
exit $rc
