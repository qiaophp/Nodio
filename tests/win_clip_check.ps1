# win.clip 官方 API 面回归（E-17）。
#
# 契约来源：D:\aardio\lib\win\clip\_.aardio（`namespace win.clip;`）
# 官方定义 12 个成员：readUtf16/read/write/writeBitmap/readBitmap/clear/
# test/formats/formatNames/getFormatName/eachFormat/readUnicode。
#
# 覆盖两条路径：
#   B-* = 引擎内置实现（未 `import win.clip;` 时的兜底，IDE 的
#          ide/nodio/app/editor.nodio 走的就是这条）
#   O-* = 官方包实现（`import win.clip;` 之后，官方文档要求的用法）
# 两条路径的 API 面与静态语义必须一致。
#
# 剪贴板是**全局**系统资源：被同机其它进程占用时 OpenClipboard 返回 0
# （ERROR_ACCESS_DENIED），官方语义是「静默失败」（相关调用返回 null）。
# 两种环境都给出确定断言：CLIP-OK=1 断言往返值，CLIP-OK=0 断言静默失败。
#
#   pwsh -File tests/win_clip_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe       = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Script    = 'D:\Nodio\tests\win_clip\clip.nopio',
    [string]$LibDir    = 'D:\aardio\lib',
    [string]$Overlay   = 'D:\Nodio\lib',
    [int]   $TimeoutSec = 60
)

if (-not (Test-Path $Exe))    { Write-Output "FAIL: exe not found: $Exe";    exit 1 }
if (-not (Test-Path $Script)) { Write-Output "FAIL: script not found: $Script"; exit 1 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_RUN'] = $Script
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
$psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $Overlay

$p = [System.Diagnostics.Process]::Start($psi)
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); $p.WaitForExit(5000) }
$outRaw = ($outTask.Result + $errTask.Result) -replace "`r", ''
$kv = @{}
foreach ($line in ($outRaw -split "`n")) {
    $line = $line.Trim()
    if ($line -eq '') { continue }
    $sp = $line.IndexOf(' ')
    if ($sp -lt 0) { $kv[$line] = ''; continue }
    $kv[$line.Substring(0, $sp)] = $line.Substring($sp + 1)
}

$fail = 0
function Check($key, $want) {
    if (-not $kv.ContainsKey($key)) { Write-Output "FAIL ${key}: missing (got nothing)"; $script:fail++; return }
    $got = $kv[$key]
    if ($got -ne $want) { Write-Output "FAIL ${key}: got [$got] want [$want]"; $script:fail++; return }
    Write-Output "ok   $key = $got"
}

# ---- 与剪贴板无关的断言（任何环境都必须成立） ----
Check 'B-API-MISS' '0'
Check 'B-FMTNAMES' 'table Unicode Text Text'
Check 'B-GFN' 'Unicode Text Unknown null'
Check 'O-API-MISS' '0'
Check 'O-FMTNAMES' 'table Unicode Text Text'
Check 'O-GFN' 'Unicode Text Unknown null'

$clipOk = $kv['CLIP-OK']
if ($clipOk -eq '1') {
    Write-Output '--- clipboard available: 断言往返语义 ---'
    Check 'B-WRITE' '1'
    Check 'B-READ' 'nodio-clip-roundtrip'
    Check 'B-READ16' 'nodio-clip-roundtrip'
    Check 'B-TEST13' 'true'
    Check 'B-FMT-HAS13' '1'
    Check 'B-EACH-HAS13' '1'
    Check 'B-CLEAR' 'true'
    Check 'B-READ2' 'null'
    Check 'O-WRITE' '1'
    Check 'O-READ' 'nodio-clip-roundtrip'
    Check 'O-READ16' 'nodio-clip-roundtrip'
    Check 'O-TEST13' 'true'
    Check 'O-FMT-HAS13' '1'
    Check 'O-EACH-HAS13' '1'
    Check 'O-CLEAR' 'true'
    Check 'O-READ2' 'null'
}
elseif ($clipOk -eq '0') {
    Write-Output '--- clipboard locked by another process: 断言官方「静默失败」语义 ---'
    Check 'B-WRITE' '0'
    Check 'B-READ' 'null'
    Check 'B-READ16' 'null'
    Check 'B-TEST13' 'null'
    Check 'B-FMT-HAS13' 'null'
    Check 'B-EACH-HAS13' '0'
    Check 'B-CLEAR' 'null'
    Check 'B-READ2' 'null'
    Check 'O-WRITE' '0'
    Check 'O-READ' 'null'
    Check 'O-READ16' 'null'
    Check 'O-TEST13' 'null'
    Check 'O-FMT-HAS13' 'null'
    Check 'O-EACH-HAS13' '0'
    Check 'O-CLEAR' 'null'
    Check 'O-READ2' 'null'
}
else {
    Write-Output "FAIL CLIP-OK: got [$clipOk] want [0|1]"
    $fail++
}

if ($fail -eq 0) {
    Write-Output 'WIN-CLIP OK'
    exit 0
}
Write-Output "FAIL: win.clip contract mismatch ($fail)"
exit 1
