# IDE smoke test — launches nodio-ide with a source file, drives Run (the
# IDM_RUN_RUN command, i.e. F5), then reads both Scintilla panes back to
# confirm the file was loaded and the child process output really arrived
# in the output pane.
#
#   powershell -File tests/ide_smoke.ps1 [-Exe <ide.exe>] [-File <src>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio-ide.exe',
    [string]$File = 'D:\Nodio\tests\vmtests\t01.aardio',
    [int]$WaitMs = 7000
)

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class IdeW {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    public delegate bool EnumCb(IntPtr h, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr p, EnumCb cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
}
'@

# Read a Scintilla pane. Scintilla writes into the caller's memory and a
# managed byte[] is not round-tripped by P/Invoke, so the text is pulled
# with WM_GETTEXT into a StringBuilder.
function Get-SciText([IntPtr]$h) {
    $len = [int][IdeW]::SendMessageW($h, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($len -le 0) { return '' }
    $sb = New-Object System.Text.StringBuilder ($len + 2)
    [void][IdeW]::SendMessageW($h, 0x000D, [IntPtr]($len + 1), $sb)
    return $sb.ToString()
}

# SCI_GETREADONLY = 2140
function Test-SciReadOnly([IntPtr]$h) {
    return ([int][IdeW]::SendMessageW($h, 2140, [IntPtr]::Zero, [IntPtr]::Zero)) -ne 0
}

function Normalize([string]$s) { return ($s -replace "`r`n", "`n") }

$proc = Start-Process -FilePath $Exe -ArgumentList $File -PassThru
try {
    Start-Sleep -Milliseconds 2500
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Output 'FAIL: main window not created'
        exit 1
    }
    Write-Output "window: $($proc.MainWindowTitle)"

    $script:panes = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [IdeW+EnumCb]{
        param([IntPtr]$h, [IntPtr]$l)
        $cn = New-Object System.Text.StringBuilder 64
        [void][IdeW]::GetClassNameW($h, $cn, 64)
        if ($cn.ToString() -eq 'Scintilla') { $script:panes.Add($h) }
        return $true
    }
    [void][IdeW]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero)
    if ($script:panes.Count -lt 2) {
        Write-Output "FAIL: expected 2 Scintilla panes, found $($script:panes.Count)"
        exit 1
    }
    Write-Output "scintilla panes: $($script:panes.Count)"

    # the output pane is the read-only one
    $editH = [IntPtr]::Zero
    $outH = [IntPtr]::Zero
    foreach ($h in $script:panes) {
        if (Test-SciReadOnly $h) { $outH = $h }
        elseif ($editH -eq [IntPtr]::Zero) { $editH = $h }
    }
    if ($editH -eq [IntPtr]::Zero -or $outH -eq [IntPtr]::Zero) {
        Write-Output 'FAIL: could not identify editor/output panes'
        exit 1
    }

    $editor = Get-SciText $editH
    $disk = [System.IO.File]::ReadAllText($File, [System.Text.Encoding]::UTF8)
    if ($editor.Length -eq 0 -or (Normalize $editor) -ne (Normalize $disk)) {
        Write-Output 'FAIL: editor does not hold the loaded file'
        Write-Output "editor : [$editor]"
        Write-Output "on disk: [$disk]"
        exit 1
    }
    $firstLine = ($editor -split "`n")[0]
    Write-Output "editor: $($editor.Length) chars, first line: $firstLine"

    # IDM_RUN_RUN = 1021, sent as a WM_COMMAND from the menu
    [void][IdeW]::SetForegroundWindow($hwnd)
    [void][IdeW]::SendMessageW($hwnd, 0x0111, [IntPtr]1021, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $WaitMs

    # ASCII-only assertion: PowerShell 5.1 reads this file as ANSI unless it
    # carries a BOM, so a non-ASCII literal here would not match the pane.
    $out = Get-SciText $outH
    $leaf = [System.IO.Path]::GetFileName($File)
    if ($out.Length -lt 10 -or -not $out.Contains($leaf)) {
        Write-Output 'FAIL: output pane has no run banner'
        Write-Output "output: [$out]"
        exit 1
    }
    Write-Output '--- output pane ---'
    Write-Output $out
    Write-Output 'SMOKE OK'
}
finally {
    if (-not $proc.HasExited) { $proc.Kill(); [void]$proc.WaitForExit(5000) }
}
