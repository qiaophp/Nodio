# Namespace import coverage: import every top-level official-lib
# namespace headlessly and record pass/fail. PowerShell port of
# tests/ns_sweep.sh so the sweep runs on a Windows host without bash.
#
# Usage: tests/ns_sweep.ps1 [-Exe <nodio.exe>] [-LibDir <lib>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$LibDir = 'D:\aardio\lib'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }
if (-not (Test-Path $LibDir)) { "LIB DIR NOT FOUND: $LibDir"; exit 2 }

$probe = Join-Path $env:TEMP ("nsprobe." + [Guid]::NewGuid().ToString('N') + ".aardio")
$ok = 0; $fail = 0; $lines = @()

foreach ($dir in (Get-ChildItem $LibDir -Directory | Sort-Object Name)) {
    $name = $dir.Name
    Set-Content -Path $probe -Value "import $name;`nio.print(`"ns-ok`");" -Encoding UTF8

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_RUN'] = $probe
    $psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.WorkingDirectory = $env:TEMP

    $p = [System.Diagnostics.Process]::Start($psi)
    $oTask = $p.StandardOutput.ReadToEndAsync()
    $eTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(5000) }
    $out = $oTask.Result + $eTask.Result
    $rc = $p.ExitCode

    if (($rc -eq 0) -and ($out -notmatch '运行错误|语法错误|无法打开|无法加载|找不到|错误')) {
        $ok++; $lines += "ok    $name"
    }
    else {
        $fail++
        $m = [regex]::Match($out, '(?m)^.*(错误|找不到|无法).*$')
        $msg = if ($m.Success) { $m.Value.Trim() } else { "exit $rc" }
        if ($msg.Length -gt 140) { $msg = $msg.Substring(0, 140) }
        $lines += "FAIL  $name :: $msg"
    }
}
Remove-Item $probe -Force -ErrorAction SilentlyContinue
$lines | ForEach-Object { Write-Output $_ }
"== ok=$ok fail=$fail =="
