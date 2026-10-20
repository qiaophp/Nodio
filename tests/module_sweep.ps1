# Behavioral import coverage for every public official-library module.
# PowerShell port of tests/sweep_all.py with:
#   - a longer per-module timeout
#   - captured first-error text, so the log can be grouped into defect clusters
#   - file-redirected stdout/stderr (a module that spawns a grandchild holding
#     the inherited pipe would otherwise block ReadToEnd forever)
#
# Usage: tests/module_sweep.ps1 [-Exe <nodio.exe>] [-LibDir <lib>] [-List <file>] [-Log <file>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$LibDir = 'D:\aardio\lib',
    [string]$List = 'D:\Nodio\tests\public_lib_modules.txt',
    [string]$Log = 'D:\Nodio\.module-sweep-x64.log',
    [int]$TimeoutMs = 10000,
    [int]$StartAt = 1
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }
$modules = @(Get-Content $List | Where-Object { $_.Trim() -ne '' -and -not $_.StartsWith('#') })
$outF = [System.IO.StreamWriter]::new($Log, $false, [Text.Encoding]::UTF8)
$ok = 0; $fail = 0; $i = 0

# Read a file that another process may still hold open for writing: the
# point of the file redirection is that a long-lived grandchild only keeps a
# file handle, so the read returns what has been written so far instead of
# blocking forever the way an inherited pipe does.
function Read-SharedFile([string]$path) {
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try {
            $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
            try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
        } finally { $fs.Dispose() }
    } catch { return '' }
}

# Run one module in its own process with file-redirected stdout/stderr.
# Returns @{ Status = 'OK'|'FAIL'|'TIMEOUT'; Detail = <text> }
function Invoke-Module([string]$mod, [string]$tmpOut, [string]$tmpErr, [string]$tmpCmd) {
    # The child's stdout/stderr are redirected to FILES by a generated .cmd
    # (not by pipes): a module that starts a long-lived grandchild -
    # process.adb.connecter starts an adb server - inherits the handle and
    # would block ReadToEnd forever.
    $cmdBody = "@echo off`r`n`"$Exe`" -e `"import $mod; print('MOD_OK');`" > `"$tmpOut`" 2> `"$tmpErr`"`r`n"
    [System.IO.File]::WriteAllText($tmpCmd, $cmdBody, [Text.Encoding]::ASCII)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $false
    $psi.RedirectStandardError = $false
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $psi.WorkingDirectory = $env:TEMP
    $psi.ArgumentList.Add('/d')
    $psi.ArgumentList.Add('/c')
    $psi.ArgumentList.Add($tmpCmd)

    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit($TimeoutMs)) {
        $p.Kill($true)
        $p.WaitForExit(5000)
        return @{ Status = 'TIMEOUT'; Detail = "exceeded ${TimeoutMs}ms" }
    }
    $o = Read-SharedFile $tmpOut
    $e = Read-SharedFile $tmpErr
    # "MOD_OK" on stdout is the import verdict. The exit code is NOT
    # reliable: a module that leaves a long-lived grandchild behind
    # (process.adb.connecter starts an adb server) keeps a console
    # attached, and every later run then reports a non-zero exit code
    # even though the module imported fine.
    if ($o -match 'MOD_OK') {
        $detail = if ($p.ExitCode -eq 0) { '' } else { "exit=$($p.ExitCode)" }
        return @{ Status = 'OK'; Detail = $detail }
    }
    $detail = (($e + ' ' + $o) -replace '\s+', ' ').Trim()
    if ($detail.Length -gt 200) { $detail = $detail.Substring(0, 200) }
    return @{ Status = 'FAIL'; Detail = $detail }
}

foreach ($mod in $modules) {
    $i++
    if ($i -lt $StartAt) { continue }

    # EVERY module gets its OWN stdout/stderr file. Sharing one file across
    # modules silently poisons the verdict: a module that leaves a long-lived
    # grandchild behind (process.adb.connecter starts an adb server) keeps the
    # redirected handle open, and the grandchild's own writes then land in
    # whatever module runs next. With one shared file, everything after
    # process.adb.connecter (position 418) reported "OK" no matter what it did
    # — 552 of 969 verdicts were fake. Per-module files remove the cross-talk.
    $tag = [Guid]::NewGuid().ToString('N')
    $tmpOut = Join-Path $env:TEMP ("sw.$tag.out")
    $tmpErr = Join-Path $env:TEMP ("sw.$tag.err")
    $tmpCmd = Join-Path $env:TEMP ("sw.$tag.cmd")

    $res = Invoke-Module $mod $tmpOut $tmpErr $tmpCmd
    # A failure with NO output at all is not a verdict: the module either died
    # before flushing (a lingering grandchild still holds the redirected file
    # object, so the child's buffered "MOD_OK" is not on disk yet) or hit a
    # transient. Retry such a case once before recording it as a defect.
    if ($res.Status -eq 'FAIL' -and $res.Detail -eq '') {
        $res2 = Invoke-Module $mod $tmpOut $tmpErr $tmpCmd
        if ($res2.Status -eq 'OK') {
            $res = @{ Status = 'OK'; Detail = $res2.Detail + ' (retried)' }
        } else {
            $res = $res2
        }
    }
    $status = $res.Status; $detail = $res.Detail

    if ($status -eq 'OK') { $ok++ } else { $fail++ }
    $line = "[$i/$($modules.Count)] $mod : $status $detail"
    $outF.WriteLine($line); $outF.Flush()
    # The grandchild may still hold this file open, so the delete can fail;
    # that is fine — the file is unique to this module.
    Remove-Item $tmpOut, $tmpErr, $tmpCmd -Force -ErrorAction SilentlyContinue
    if ($i % 50 -eq 0) { Write-Output "  ... $i/$($modules.Count) ok=$ok fail=$fail" }
}
$outF.Close()
# Reclaim the adb server that importing process.adb starts.
#
# Importing process.adb has two side effects that outlive the sweep:
#   1. it starts a PERSISTENT adb server (official docs: "进程退出时不会退出
#      常驻服务端"), and
#   2. on the next import it re-writes adb.exe into
#      %LOCALAPPDATA%\aardio\std\adb\<ver>\ — official builtin/io.aardio backs
#      the locked original up as adb.exe.<tick>.old and can only unlink that
#      backup once the running image releases it, so every sweep leaves
#      another 5.8MB .old behind (measured: 93 leftovers / ~540MB).
# Kill the servers we started so the next sweep neither stacks processes nor
# stacks .old files. Only servers whose image lives in the aardio std dir are
# touched, and only ones with no live parent (orphans from a killed sweep).
$adbDir = Join-Path $env:LOCALAPPDATA 'aardio\std\adb'
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -eq 'adb.exe' -and
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($adbDir, [StringComparison]::OrdinalIgnoreCase) -and
        -not (Get-Process -Id $_.ParentProcessId -ErrorAction SilentlyContinue)
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# Only NOW are the .old backups unlinkable: the sweep itself wrote them
# because adb.exe was locked by the very server we just killed. official
# builtin/io.aardio renames the locked original to <name>.<tick>.old and its
# own "remove(old)" call fails while the image is still mapped — so the
# backup survives as garbage (measured: 93 files / ~540MB for adb.exe alone).
# Wait briefly for the killed servers to release their file handles, then
# remove the leftovers. Never touch the live adb.exe / AdbWin*.dll.
Start-Sleep -Milliseconds 800
if (Test-Path $adbDir) {
    Get-ChildItem $adbDir -Recurse -Force -Filter '*.old' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
}
Write-Output "== ok=$ok fail=$fail total=$($modules.Count) =="
