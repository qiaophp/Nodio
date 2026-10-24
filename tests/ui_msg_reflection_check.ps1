# Nodio win.ui & native win.form message reflection & wndproc regression lock
$ErrorActionPreference = 'Stop'

$exe = "D:\Nodio\build-x64\Release\nodio.exe"
if (-not (Test-Path $exe)) {
    Write-Error "nodio.exe not found at $exe"
}

Write-Host "Running win.ui reflection check..."
$out1 = & $exe tests/fixtures_winui_reflection.aardio
Write-Host "Output: $out1"
if ($out1 -notmatch "WINUI_REFLECTION_PASS") {
    throw "win.ui reflection test failed"
}

Write-Host "Running native win.form reflection check..."
$out2 = & $exe tests/fixtures_native_reflection.aardio
Write-Host "Output: $out2"
if ($out2 -notmatch "NATIVE_REFLECTION_PASS") {
    throw "native win.form reflection test failed"
}

Write-Host "[OK] win.ui and native control message reflection chain verified" -ForegroundColor Green
