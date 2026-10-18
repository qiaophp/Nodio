# Self-hosted IDE smoke test — runs ide/nodio/main.nodio on the Nodio VM in
# headless smoke mode (NODIO_IDE_SMOKE=1): the IDE builds its DSG layout,
# writes a probe script, runs it through the F5 process.popen pipeline and
# reports whether the child's output round-tripped to the output pane.
#
# The IDE replaces the retired native Win32 + Scintilla shell, so this is the
# IDE acceptance test (it no longer pokes Scintilla child windows).
#
#   powershell -File tests/ide_smoke.ps1 [-Exe <nodio.exe>] [-Ide <main.nodio>]
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Ide     = 'D:\Nodio\ide\nodio\main.nodio',
    [string]$LibDir  = 'D:\aardio\lib',
    [int]   $TimeoutSec = 60
)

if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }
if (-not (Test-Path $Ide)) { Write-Output "FAIL: IDE script not found: $Ide"; exit 1 }

$env:NODIO_LIBDIR    = $LibDir
$env:NODIO_EXE       = $Exe
$env:NODIO_RUN       = $Ide
$env:NODIO_IDE_SMOKE = '1'

# the IDE's own project file drives the project-model check
$proj = Join-Path (Split-Path -Parent $Ide) 'ide.nproj'
if (Test-Path $proj) { $env:NODIO_IDE_PROJECT = $proj }

$log = Join-Path $env:TEMP ("nodio_ide_smoke_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: IDE timed out after ${TimeoutSec}s"
    exit 1
}

$out = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

Write-Output $out.TrimEnd()

if ($out -notmatch 'IDE-SMOKE-OK') {
    if ($out -match 'IDE-SMOKE-NO-ECHO') {
        Write-Output 'FAIL: run pipeline produced no echoed output'
    } else {
        Write-Output 'FAIL: IDE did not reach completion'
    }
    exit 1
}

# project model: ide.nproj must parse and resolve its start file
if ($env:NODIO_IDE_PROJECT) {
    if ($out -notmatch 'PROJ-FILES:\d+') { Write-Output 'FAIL: project files not parsed'; exit 1 }
    if ($out -notmatch 'PROJ-START:')     { Write-Output 'FAIL: project start file not resolved'; exit 1 }
}

Write-Output 'SMOKE OK'
exit 0
