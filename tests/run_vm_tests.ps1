# One-shot VM regression: runs every scripts\vmtests\tNN.aardio through the
# NODIO_RUN harness and compares merged stdout+stderr with tNN.expect.
# Usage: powershell -File run_vm_tests.ps1 [path\to\aardio_re.exe] [-Only tNN[,tMM...]]
param($Exe = 'D:\aardio.re\skeleton\build-win32\Release\aardio_re.exe',
      [string]$Only = '')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$tests = Join-Path $dir 'vmtests'
$pass = 0; $fail = 0; $failed = @()
# -Only tNN[,tMM]: run just the named tests (edit-scoped reruns); a tNN
# listed here skips the tNN.aardio loop below and the t49 publish phase
# is likewise gated on 't49' appearing in -Only (empty -Only = all).
$onlySet = @{}
if ($Only -ne '') {
    $Only -split ',' | ForEach-Object { $onlySet[$_.Trim()] = $true }
}

# local HTTP server for web-library tests (webroot\_srv.py), if present
$srvProc = $null
$srvPy = Join-Path $tests 'webroot\_srv.py'
if (Test-Path $srvPy) {
    $py = if (Get-Command 'python' -ErrorAction SilentlyContinue) { 'python' } elseif (Test-Path 'C:\Python311\python.exe') { 'C:\Python311\python.exe' } else { '' }
    if ($py -ne '') {
        $srvProc = Start-Process $py -ArgumentList "`"$srvPy`"" -PassThru `
            -WindowStyle Hidden
        Start-Sleep -Milliseconds 900
    }
}

Get-ChildItem $tests -Filter 't??.aardio' | Sort-Object Name | ForEach-Object {
    $name = $_.BaseName
    if ($onlySet.Count -gt 0 -and -not $onlySet[$name]) { return }
    $expectFile = Join-Path $tests ($name + '.expect')
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_RUN'] = $_.FullName
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    # $psi.EnvironmentVariables['NODIO_LIBDIR'] = 'D:\aardio\lib'
    # optional tNN.stdin -> piped to the process (debugger command channel);
    # optional tNN.brk -> NODIO_BREAK breakpoint lines (implies NODIO_DEBUG=1)
    $stdinFile = Join-Path $tests ($name + '.stdin')
    $brkFile = Join-Path $tests ($name + '.brk')
    $hasStdin = Test-Path $stdinFile
    if ($hasStdin) { $psi.RedirectStandardInput = $true }
    if (Test-Path $brkFile) {
        $psi.EnvironmentVariables['NODIO_DEBUG'] = '1'
        $psi.EnvironmentVariables['NODIO_BREAK'] =
            (Get-Content $brkFile -Raw -Encoding UTF8).Trim()
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if ($hasStdin) {
        $p.StandardInput.Write((Get-Content $stdinFile -Raw -Encoding UTF8))
        $p.StandardInput.Close()
    }
    # start draining both pipes before waiting: a child blocked on a full
    # pipe would never exit, and a timed-out kill can drop buffered output
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(20000)) { $p.Kill(); $p.WaitForExit(5000) }
    $out = ($outTask.Result + $errTask.Result)
    $out = $out -replace "`r", '' -replace "`n", ''
    $expect = (Get-Content $expectFile -Raw -Encoding UTF8)
    $expect = $expect -replace "`r", '' -replace "`n", ''
    if ($out -eq $expect) { $pass++; Write-Host "PASS $name" }
    else { $fail++; $failed += $name; Write-Host "FAIL $name got=[$out] want=[$expect]" }
}

# t49: two-phase publish acceptance — NODIO_PUBLISH the proj\ fixture into
# dist\t49app.exe, then run THAT exe and compare its merged output. The
# published exe resolves imports from the embedded libEmbed closure and
# res/ from the payload (~\ prefix), so this exercises the M6 pipeline.
$projDir = Join-Path $tests 'proj'
$projOk = Test-Path (Join-Path $projDir 'default.aproj')
$t49Selected = ($onlySet.Count -eq 0 -or $onlySet['t49'])
if ($projOk -and $t49Selected) {
    $name = 't49'
    $expectFile = Join-Path $tests ($name + '.expect')
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_PUBLISH'] =
        (Join-Path $projDir 'main.aardio')
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(5000) }
    $pubOut = ($outTask.Result + $errTask.Result)
    $exePath = Join-Path $projDir 'dist\t49app.exe'
    if (Test-Path $exePath) {
        $psi2 = New-Object System.Diagnostics.ProcessStartInfo
        $psi2.FileName = $exePath
        $psi2.UseShellExecute = $false
        $psi2.RedirectStandardOutput = $true
        $psi2.RedirectStandardError = $true
        $psi2.StandardOutputEncoding = [Text.Encoding]::UTF8
        $psi2.StandardErrorEncoding = [Text.Encoding]::UTF8
        $psi2.EnvironmentVariables['NODIO_PAUSE'] = '0'
        $p2 = [System.Diagnostics.Process]::Start($psi2)
        $outTask2 = $p2.StandardOutput.ReadToEndAsync()
        $errTask2 = $p2.StandardError.ReadToEndAsync()
        if (-not $p2.WaitForExit(20000)) { $p2.Kill(); $p2.WaitForExit(5000) }
        $out = ($outTask2.Result + $errTask2.Result)
        $out = $out -replace "`r", '' -replace "`n", ''
        $expect = (Get-Content $expectFile -Raw -Encoding UTF8)
        $expect = $expect -replace "`r", '' -replace "`n", ''
        if ($out -eq $expect) { $pass++; Write-Host "PASS $name" }
        else { $fail++; $failed += $name; Write-Host "FAIL $name got=[$out] want=[$expect]" }
    }
    else {
        $fail++; $failed += $name
        Write-Host "FAIL $name (publish produced no exe) pub=[$pubOut]"
    }
}
Write-Host "== $pass passed, $fail failed =="
# _srv.py spawns a child, so killing the wrapper PID alone can leave a
# listener behind; kill the whole tree by PID.
if ($srvProc -and -not $srvProc.HasExited) {
    & taskkill /PID $srvProc.Id /T /F 2>$null | Out-Null
}
if ($fail -gt 0) { exit 1 }
exit 0
