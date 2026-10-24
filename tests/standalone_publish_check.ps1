# Nodio standalone single EXE publisher regression lock
$ErrorActionPreference = 'Stop'

$nodioExe = "D:\Nodio\build-x64\Release\nodio.exe"
if (-not (Test-Path $nodioExe)) {
    Write-Error "nodio.exe not found at $nodioExe"
}

# 1. 准备测试工程脚本
$appSrc = @'
import console;
import string;

var s = "NodioStandaloneExeSuccess";
var sub = string.left(s, 15);
console.log("APP_PAYLOAD_EXEC_START");
console.log("SUBSTR:" ++ sub);
console.log("STANDALONE_RUN_OK");
'@

$fixtureDir = Join-Path $PSScriptRoot "fixtures_dist"
if (-not (Test-Path $fixtureDir)) {
    New-Item -ItemType Directory -Path $fixtureDir | Out-Null
}
$entryPath = Join-Path $fixtureDir "main.aardio"
[System.IO.File]::WriteAllText($entryPath, $appSrc, [System.Text.Encoding]::UTF8)

$outExe = Join-Path $fixtureDir "test_standalone.exe"
if (Test-Path $outExe) {
    rm -- $outExe
}

# 2. 执行 CLI 单文件打包编译器
Write-Host "Publishing standalone single EXE via CLI..."
$pubOut = & $nodioExe --publish $entryPath -o $outExe --lib "D:\Nodio\lib" 2>&1
Write-Host "Publish output: $pubOut"

if (-not (Test-Path $outExe)) {
    throw "Published exe was not generated: $outExe"
}

$fileSize = (Get-Item $outExe).Length
Write-Host "Generated standalone EXE size: $fileSize bytes"
if ($fileSize -lt 100000) {
    throw "Generated exe size is suspiciously small: $fileSize"
}

# 3. 独立脱机运行生成的单个 EXE，验证自包含 runtime 与 payload 正确执行
Write-Host "Running generated standalone EXE in isolated environment..."
$env:NODIO_LIBDIR = $null
$env:NODIO_RUN = $null

$runOut = (& $outExe | Out-String)
Write-Host "Standalone execution output:"
Write-Host $runOut

if ($runOut -notmatch "APP_PAYLOAD_EXEC_START" -or $runOut -notmatch "STANDALONE_RUN_OK") {
    throw "Standalone exe failed to execute payload correctly!"
}

Write-Host "[OK] Standalone single EXE packaging and execution verified successfully!" -ForegroundColor Green
