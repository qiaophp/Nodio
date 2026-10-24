# tests/ide_visual_lsp.ps1 — 实机验证编辑器 × LSP 三个视觉链路（跨进程 PostMessage 驱动）：
#   ① 引用标记（把光标放到标识符上 → 300ms → 同符号垫亮）
#   ② 悬停提示（鼠标停 350ms → hover 浮层）
#   ③ 代码补全（敲 '.' → 自动补全浮层）
#   ④ 滚动后高亮带跟随（CFE_BACKCOLOR 是字符属性，应随文本滚动）
# 全程 PostMessage 直接投递到编辑器 richedit —— 不抢焦点、不动用户鼠标
# （实测 SetCursorPos/mouse_event 会点到前台浏览器上；SetForegroundWindow 被
#   焦点窃取保护拦下）。截图用 PrintWindow 合成（本仓库既有验证管线）。
param(
    [string]$OutPrefix = 'D:\Nodio\build-x64\Release\ide_vhl',
    [int]$BootMs = 10000
)
$ErrorActionPreference = 'Stop'

$exe = 'D:\Nodio\build-x64\Release\nodio.exe'
$env:NODIO_LIBDIR          = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR  = 'D:\Nodio\lib'
$env:NODIO_EXE             = $exe
$env:NODIO_RUN             = 'D:\Nodio\ide\nodio\main.nodio'
$env:NODIO_IDE_DIAG        = '1'
$env:NODIO_IDE_PROJECT     = 'D:\Nodio\ide\nodio\ide.nproj'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct CHARRANGE { public int cpMin, cpMax; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct FINDTEXTEXW { public CHARRANGE chrg; [MarshalAs(UnmanagedType.LPWStr)] public string text; public CHARRANGE chrgText; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, ref FINDTEXTEXW l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, ref CHARRANGE l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr h, uint m, ref CHARRANGE w, IntPtr l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassName(h,sb,256); return sb.ToString(); }
    public static IntPtr MakeLparam(int x, int y){ return (IntPtr)((y << 16) | (x & 0xFFFF)); }
}
'@
[void][V]::SetProcessDpiAwarenessContext([IntPtr](-4))
Add-Type -AssemblyName System.Drawing

function Shot([string]$path, [IntPtr]$top) {
    # PrintWindow 合成：父窗 + 全部可见子窗按 z 序(自底向上)贴回
    $tr = New-Object V+RECT
    [void][V]::GetWindowRect($top, [ref]$tr)
    $w = $tr.Right - $tr.Left; $h = $tr.Bottom - $tr.Top
    if ($w -le 0 -or $h -le 0) { return }

    $kids = New-Object System.Collections.Generic.List[object]
    $cb = [V+EnumProc]{ param($h2,$l)
        $r2 = New-Object V+RECT
        [void][V]::GetWindowRect($h2, [ref]$r2)
        $kids.Add(@{ H = $h2; R = $r2; Vis = [V]::IsWindowVisible($h2) })
        return $true
    }
    [void][V]::EnumChildWindows($top, $cb, [IntPtr]::Zero)

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][V]::PrintWindow($top, $hdc, 2)
    $g.ReleaseHdc($hdc)

    for ($i = $kids.Count - 1; $i -ge 0; $i--) {
        $k = $kids[$i]
        if (-not $k.Vis) { continue }
        $kw = $k.R.Right - $k.R.Left; $kh = $k.R.Bottom - $k.R.Top
        if ($kw -le 0 -or $kh -le 0) { continue }
        $kb = New-Object System.Drawing.Bitmap $kw, $kh
        $kg = [System.Drawing.Graphics]::FromImage($kb)
        $khdc = $kg.GetHdc()
        [void][V]::PrintWindow($k.H, $khdc, 2)
        $kg.ReleaseHdc($khdc); $kg.Dispose()
        $ox = $k.R.Left - $tr.Left; $oy = $k.R.Top - $tr.Top
        $g.DrawImage($kb, $ox, $oy)
        $kb.Dispose()
    }
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# ---- 启动 IDE ----
$so = [IO.Path]::GetTempFileName(); $se = [IO.Path]::GetTempFileName()
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardOutput $so -RedirectStandardError $se
Start-Sleep -Milliseconds $BootMs

try {
    # ---- 找主窗口 / 编辑器 ----
    $script:top = [IntPtr]::Zero; $script:big = 0
    $cb = [V+EnumProc]{ param($h,$l)
        [uint32]$procId = 0
        [void][V]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -eq $p.Id) {
            $r = New-Object V+RECT
            [void][V]::GetWindowRect($h, [ref]$r)
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:big) { $script:big = $a; $script:top = $h }
        }
        return $true
    }
    [void][V]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:top -eq [IntPtr]::Zero) { Write-Output 'FAIL: no IDE window'; exit 1 }
    [void][V]::ShowWindow($script:top, 3)   # 最大化
    Start-Sleep -Milliseconds 2500

    $script:ed = [IntPtr]::Zero; $script:edArea = 0
    $cb2 = [V+EnumProc]{ param($h,$l)
        if ([V]::Cls($h) -eq 'RICHEDIT50W') {
            $r = New-Object V+RECT
            [void][V]::GetWindowRect($h, [ref]$r)
            $a = ($r.Right-$r.Left)*($r.Bottom-$r.Top)
            if ($a -gt $script:edArea) { $script:edArea = $a; $script:ed = $h }
        }
        return $true
    }
    [void][V]::EnumChildWindows($script:top, $cb2, [IntPtr]::Zero)
    if ($script:ed -eq [IntPtr]::Zero) { Write-Output 'FAIL: no richedit'; exit 1 }

    $ecr = New-Object V+RECT
    [void][V]::GetClientRect($script:ed, [ref]$ecr)
    $origin = New-Object V+PT
    [void][V]::ClientToScreen($script:ed, [ref]$origin)
    Write-Host ("ED-CLIENT: 0,0," + $ecr.Right + "," + $ecr.Bottom + " origin=" + $origin.X + "," + $origin.Y)

    # ---- 找一个出现 ≥4 次的标识符（读回文本统计）----
    $len = [int][V]::SendMessage($script:ed, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)
    $sb = New-Object System.Text.StringBuilder ($len + 4)
    [void][V]::SendMessage($script:ed, 0x000D, [IntPtr]($len + 1), $sb)
    $text = $sb.ToString()
    Write-Output ("TEXT-LEN: " + $text.Length)
    if ($text.Length -lt 100) { Write-Output 'FAIL: editor text too short'; exit 1 }

    $words = @{}
    foreach ($m in [regex]::Matches($text, '[A-Za-z_][A-Za-z0-9_]{3,}')) {
        $k = $m.Value
        if (-not $words.ContainsKey($k)) { $words[$k] = 0 }
        $words[$k]++
    }
    $word = $null; $best = 0
    foreach ($kv in $words.GetEnumerator()) { if ($kv.Value -gt $best) { $best = $kv.Value; $word = $kv.Key } }
    if ($null -eq $word) { Write-Output 'FAIL: no identifier'; exit 1 }
    Write-Output ("WORD: " + $word + "  OCCURRENCES(text): " + $best)

    # ---- 找标识符的出现位置：直接用读回的文本算 cp ----
    # （EM_FINDTEXTEXW 跨进程结构封送不稳定，实测返回 cp=0；
    #   RichEdit 的字符 offset 在纯 BMP 文本下 == .NET UTF-16 index，
    #   所以先确认文档里没有增补平面字符，再把 .NET index 当 cp 用）
    $firstAstral = [long]::MaxValue
    for ($i = 0; $i -lt $text.Length; $i++) {
        $c = [int][char]$text[$i]
        if ($c -ge 0xD800 -and $c -le 0xDFFF) { $firstAstral = $i; break }
    }
    Write-Host ("FIRST-ASTRAL: " + $firstAstral)

    $occ = New-Object System.Collections.Generic.List[int]
    if ($firstAstral -gt 0) {
        foreach ($m in [regex]::Matches($text, ('(?<![A-Za-z0-9_])' + [regex]::Escape($word) + '(?![A-Za-z0-9_])'))) {
            if ($m.Index -lt $firstAstral) { $occ.Add([int]$m.Index) }
        }
    }
    if ($occ.Count -eq 0) { Write-Output 'FAIL: no BMP-safe occurrence'; exit 1 }
    Write-Output ("OCC-COUNT: " + $occ.Count + "  first=" + $occ[0])

    # ⚠ RichEdit 内部把 \r\n 归一成 \r —— 每个换行少 1 个字符。
    #   .NET 文本 index → RichEdit cp 必须减去前面 '\n' 的个数。
    function NetIndexToCp([int]$idx) {
        $nl = 0
        for ($i = 0; $i -lt $idx -and $i -lt $text.Length; $i++) { if ($text[$i] -eq "`n") { $nl++ } }
        return ($idx - $nl)
    }
    for ($i = 0; $i -lt $occ.Count; $i++) { $occ[$i] = NetIndexToCp $occ[$i] }
    Write-Host ("OCC-CPS: " + (($occ | Select-Object -First 6) -join ","))
    # cp → 客户端坐标（RICHEDIT50W 接受 EDIT 形式的 EM_POSFROMCHAR: wParam=index → DWORD 坐标）
    function CpToPoint([int]$cp) {
        $ret = [long][V]::SendMessage($script:ed, 0x00D6, [IntPtr]$cp, [IntPtr]::Zero)
        $x = [int]($ret -band 0xFFFF); $y = [int](($ret -shr 16) -band 0xFFFF)
        if ($x -ge 32768) { $x -= 65536 }
        if ($y -ge 32768) { $y -= 65536 }
        return @{ X = $x; Y = $y }
    }
    function EnsureInView([int]$cp) {
        $pt = CpToPoint $cp
        $inView = ($pt.X -ge 0 -and $pt.X -lt $ecr.Right -and $pt.Y -ge 0 -and $pt.Y -lt $ecr.Bottom)
        if (-not $inView) {
            [void][V]::SendMessage($script:ed, 0x00B1, [IntPtr]$cp, [IntPtr]$cp)   # EM_SETSEL
            [void][V]::SendMessage($script:ed, 0x0431, [IntPtr]::Zero, [IntPtr]::Zero)  # EM_SCROLLCARET (richedit WM_USER+49)
            Start-Sleep -Milliseconds 500
            $pt = CpToPoint $cp
        }
        return $pt
    }
    function GetCaret() {
        # EM_EXGETSEL: wParam = CHARRANGE*（之前参数顺序放错了，读回来一直是 0）
        $cr = New-Object V+CHARRANGE
        [void][V]::SendMessage($script:ed, 0x0434, [ref]$cr, [IntPtr]::Zero)
        return $cr
    }
    function PlaceCaret([int]$cp, [int]$cpEnd, [string]$tag) {
        # 确定性落位：先 EM_SETSEL + EM_SCROLLCARET，再补一发 WM_LBUTTONUP
        # （编辑器在 WM_LBUTTONUP 里 updateCursor → 300ms 后问 LSP 画高亮带）
        [void][V]::SendMessage($script:ed, 0x00B1, [IntPtr]$cp, [IntPtr]$cpEnd)   # EM_SETSEL
        [void][V]::SendMessage($script:ed, 0x0431, [IntPtr]::Zero, [IntPtr]::Zero) # EM_SCROLLCARET
        Start-Sleep -Milliseconds 350
        $pt = CpToPoint $cp
        [void][V]::PostMessage($script:ed, 0x0020, [IntPtr]0, [V]::MakeLparam($pt.X + 6, $pt.Y + 12))  # WM_MOUSEMOVE
        Start-Sleep -Milliseconds 80
        [void][V]::PostMessage($script:ed, 0x0202, [IntPtr]0, [V]::MakeLparam($pt.X + 6, $pt.Y + 12))  # WM_LBUTTONUP
        Start-Sleep -Milliseconds 250
        $cr = GetCaret
        $fvl = [int][V]::SendMessage($script:ed, 0x00CE, [IntPtr]::Zero, [IntPtr]::Zero)  # EM_GETFIRSTVISIBLELINE
        Write-Host ("PLACE " + $tag + " cp=" + $cp + "  caret=" + $cr.cpMin + ".." + $cr.cpMax + "  firstVisibleLine=" + $fvl + "  pt=(" + $pt.X + "," + $pt.Y + ")")
        return $pt
    }

    $occ1cp = $occ[0]
    $wr = $script:top

    # ① 光标落位到标识符 → 引用标记
    $pt1 = PlaceCaret $occ1cp ($occ1cp + $word.Length) 'occ1'
    Start-Sleep -Milliseconds 1200
    Shot ($OutPrefix + "_1_highlight.png") $wr
    Write-Output 'SHOT1: highlight'

    # ② 第二处出现 → 光标/鼠标停住 → 悬停提示
    if ($occ.Count -ge 2) {
        $occ2cp = $occ[[int][Math]::Min(1, $occ.Count - 1)]
        $pt2 = PlaceCaret $occ2cp ($occ2cp + $word.Length) 'occ2'
        Start-Sleep -Milliseconds 1400
        Shot ($OutPrefix + "_2_hover.png") $wr
        Write-Output 'SHOT2: hover'
    }

    # ③ 光标放到 import 行词尾，敲 '.' → 自动补全浮层（编辑器在 WM_CHAR '.' 触发）
    $impNet = -1
    foreach ($m in [regex]::Matches($text, 'import [A-Za-z]')) {
        if ($m.Index -lt $firstAstral) { $impNet = [int]($m.Index + 7); break }
    }
    if ($impNet -ge 0) {
        $impCp = NetIndexToCp $impNet
        $pt3 = PlaceCaret $impCp $impCp 'import-word-end'
        [void][V]::PostMessage($script:ed, 0x0102, [IntPtr]0x2E, [IntPtr]0)   # WM_CHAR '.'
        Start-Sleep -Milliseconds 1500
        Shot ($OutPrefix + "_3_completion.png") $wr
        Write-Output 'SHOT3: completion'
    }

    # ④ 滚动 3 行 → 高亮带应随文本移动
    for ($i = 0; $i -lt 3; $i++) {
        [void][V]::PostMessage($script:ed, 0x0115, [IntPtr]1, [IntPtr]::Zero)  # WM_VSCROLL SB_LINEDOWN
        Start-Sleep -Milliseconds 150
    }
    Start-Sleep -Milliseconds 500
    Shot ($OutPrefix + "_4_scrolled.png") $wr
    Write-Output 'SHOT4: scrolled'
}
finally {
    try { $p.Kill() } catch {}
    Start-Sleep -Milliseconds 600
    if (Test-Path $so) {
        $out = [IO.File]::ReadAllText($so, [Text.Encoding]::UTF8)
        $diag = $out -split "`r?`n" | Where-Object { $_ -match 'onHighlight|marks|ERROR|error' }
        if ($diag) { Write-Output '--- IDE LOG (highlight/error lines) ---'; $diag | Select-Object -First 20 | ForEach-Object { Write-Output $_ } }
        Remove-Item $so, $se -Force -ErrorAction SilentlyContinue
    }
}
Write-Output 'VISUAL-DONE'
exit 0
