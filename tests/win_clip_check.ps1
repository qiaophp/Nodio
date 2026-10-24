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
# 剪贴板是**全局**系统资源，被同机其它进程占用时官方语义是「静默失败」，
# 且占用随时可能开始/结束——开头探测一次判定全程会有假红（roadmap 第 27 轮
# 挂账项）。夹具（win_clip/clip.nopio）因此**每一步操作前重新探测**，
# 把 `CLIP0/CLIP1` 门拼在该步输出末尾；本脚本按门选择期望值：
#   CLIP1 → 成功语义；CLIP0 → 官方「静默失败」语义；
#   依赖「我们写入的内容」的断言只在对应 write 真正成功时判定。
# 注意：判定函数里的提示行必须用 Write-Host——Write-Output 会混进返回值
# 管道，把 CheckWrite 的布尔返回值变成数组。
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
function CheckRaw([string]$key, [string]$want, [string]$got) {
    if ($got -eq $want) { Write-Host "ok   $key = $got" }
    else { Write-Host "FAIL ${key}: got [$got] want [$want]"; $script:fail++ }
}
function Check([string]$key, [string]$want) {
    $got = if ($kv.ContainsKey($key)) { $kv[$key] } else { '' }
    if ($got -eq '') { Write-Host "FAIL ${key}: missing (got nothing)"; $script:fail++; return }
    CheckRaw $key $want $got
}

# 解析 `值 CLIP0|CLIP1` 形式的输出 → @(值, 门)；无门返回 @($raw, '')
function ParseGated([string]$raw) {
    $parts = $raw -split ' '
    if ($parts.Count -ge 2 -and $parts[$parts.Count - 1] -match '^CLIP[01]$') {
        return , @( ($parts[0..($parts.Count - 2)] -join ' '), $parts[$parts.Count - 1] )
    }
    return , @($raw, '')
}

# write 步：门 CLIP1 → 必须 1（成功）；CLIP0 → 必须 0（官方静默失败）。
# 返回 write 是否真正成功（后续内容类断言只在这个前提下判定）。
function CheckWrite([string]$key) {
    if (-not $kv.ContainsKey($key)) { Write-Host "FAIL ${key}: missing"; $script:fail++; return $false }
    $g = ParseGated $kv[$key]
    $val = $g[0]; $gate = $g[1]
    if ($gate -eq 'CLIP1') { CheckRaw $key '1' $val; return ($val -eq '1') }
    if ($gate -eq 'CLIP0') { CheckRaw $key '0' $val; return $false }
    Write-Host "FAIL ${key}: no CLIP gate in [$($kv[$key])]"; $script:fail++; return $false
}

# 内容类步：writeOk=false → 内容不可控，跳过；否则 CLIP1 → 成功值，CLIP0 → 静默失败值。
# 返回该步的实际值（供 B-READ2/O-READ2 依赖 clear 结果时使用）。
function CheckGated([string]$key, [bool]$writeOk, [string]$wantAvail, [string]$wantLocked) {
    if (-not $kv.ContainsKey($key)) { Write-Host "FAIL ${key}: missing"; $script:fail++; return '' }
    $g = ParseGated $kv[$key]
    $val = $g[0]; $gate = $g[1]
    if (-not $writeOk) { Write-Host "skip $key = $val (write 未成功，板上内容不可控)"; return $val }
    if ($gate -eq 'CLIP1') { CheckRaw $key $wantAvail $val }
    elseif ($gate -eq 'CLIP0') { CheckRaw $key $wantLocked $val }
    else { Write-Host "FAIL ${key}: no CLIP gate in [$($kv[$key])]"; $script:fail++ }
    return $val
}

if ($kv.ContainsKey('CLIP-OK')) { Write-Host "--- 开局剪贴板可用性: CLIP-OK=$($kv['CLIP-OK'])（逐项判定看各步自己的门） ---" }

# ---- 与剪贴板无关的断言（任何环境都必须成立） ----
Check 'B-API-MISS' '0'
Check 'B-FMTNAMES' 'table Unicode Text Text'
Check 'B-GFN' 'Unicode Text Unknown null'
Check 'O-API-MISS' '0'
Check 'O-FMTNAMES' 'table Unicode Text Text'
Check 'O-GFN' 'Unicode Text Unknown null'

# ---- B 路径：引擎内置 ----
$bOk = CheckWrite 'B-WRITE'
CheckGated 'B-READ'       $bOk 'nodio-clip-roundtrip' 'null' | Out-Null
CheckGated 'B-READ16'     $bOk 'nodio-clip-roundtrip' 'null' | Out-Null
CheckGated 'B-TEST13'     $bOk 'true'  'null' | Out-Null
CheckGated 'B-FMT-HAS13'  $bOk '1'     'null' | Out-Null
CheckGated 'B-EACH-HAS13' $bOk '1'     '0'    | Out-Null
$bClearVal = CheckGated 'B-CLEAR' $bOk 'true' 'null'
if ($bOk -and $bClearVal -eq 'true') { CheckGated 'B-READ2' $true 'null' 'null' | Out-Null }
else { Write-Host "skip B-READ2（未成功写入或 clear 未成功，板上内容不可控）" }

# ---- O 路径：官方包 ----
$oOk = CheckWrite 'O-WRITE'
CheckGated 'O-READ'       $oOk 'nodio-clip-roundtrip' 'null' | Out-Null
CheckGated 'O-READ16'     $oOk 'nodio-clip-roundtrip' 'null' | Out-Null
CheckGated 'O-TEST13'     $oOk 'true'  'null' | Out-Null
CheckGated 'O-FMT-HAS13'  $oOk '1'     'null' | Out-Null
CheckGated 'O-EACH-HAS13' $oOk '1'     '0'    | Out-Null
$oClearVal = CheckGated 'O-CLEAR' $oOk 'true' 'null'
if ($oOk -and $oClearVal -eq 'true') { CheckGated 'O-READ2' $true 'null' 'null' | Out-Null }
else { Write-Host "skip O-READ2（未成功写入或 clear 未成功，板上内容不可控）" }

if ($fail -eq 0) {
    Write-Output 'WIN-CLIP OK'
    exit 0
}
Write-Output "FAIL: win.clip contract mismatch ($fail)"
exit 1
