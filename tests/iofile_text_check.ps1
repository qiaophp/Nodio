# io.file 官方文本模式回归锁。
#
# 契约来源（官方文档）：
#   docs/language-reference/datatype/newline.md §读写文件的文本模式：
#     默认文本模式 —— 读 CRLF→'\n'、写 '\n'→CRLF；'b' = 二进制。
#   docs/library-guide/builtin/io/file.md：
#     文本模式下 '\0'/'\x1A' 是终止符；要读它们必须显式 'b'。
#
# 引擎实现（engine/builtins.cpp）：CRT fd 恒为二进制（活管道在 CRT
# 文本模式会死锁），官方文本模式翻译在字节层逐调用完成——
# 读 CRLF→LF + '\0'/'\x1A' 终止（不消耗终止符，后续读停在原地）、
# 写 裸 LF→CRLF；io.file / io.open 打开时默认文本，'b' 退出。
#
# 夹具 tests/iofile_text/text.nodio 用十六进制字节串断言，
# x64 / x86 期望值完全一致。
#
#   pwsh -File tests/iofile_text_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot = Split-Path $PSScriptRoot -Parent
$fixture  = Join-Path $repoRoot 'tests\iofile_text\text.nodio'

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}
$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$fixture`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
$proc = [Diagnostics.Process]::Start($psi)
$o = $proc.StandardOutput.ReadToEnd()
$e = $proc.StandardError.ReadToEnd()
$proc.WaitForExit()

$kv = @{}
foreach ($line in ($o -split "`r?`n")) {
    $line = $line.Trim()
    if ($line -eq '') { continue }
    $sp = $line.IndexOf(' ')
    if ($sp -lt 0) { $kv[$line] = ''; continue }
    $kv[$line.Substring(0, $sp)] = $line.Substring($sp + 1)
}

$fail = 0
function Check([string]$key, [string]$want) {
    $got = if ($kv.ContainsKey($key)) { $kv[$key] } else { '<missing>' }
    if ($got -eq $want) { Write-Host "  ok   $key = $got" }
    else { Write-Host "  FAIL $key = '$got' (期望 '$want')"; $script:fail++ }
}

# ---- 与文件内容无关的断言 ----
Check 'IOFILE-TEXT-DONE' ''
if ($kv.ContainsKey('IOFILE-TEXT-DONE') -and $kv['IOFILE-TEXT-DONE'] -ne '') {
    Write-Host "  (夹具尾行异常输出: $($kv['IOFILE-TEXT-DONE']))"
}

# ---- 官方文本模式七项契约（十六进制字节级） ----
Check 'W-TEXT'         '610D0A620D0A'        # 'w'   : a\nb\n → a\r\nb\r\n
Check 'W-TEXT-EXPLICIT' '630D0A64'           # 'wt'  : c\nd   → c\r\nd
Check 'R-TEXT'         '630A64'              # 'r'   : CRLF 读回 \n
Check 'R-BIN'          '630D0A64'            # 'rb'  : 字节原样
Check 'R-BIN-TERM'     '780A79007A0D0A1A77'  # 'rb'  : \0/\x1A 都读得到
Check 'R-TEXT-TERM'    '780A79'              # 'r'   : \0 起读不到
Check 'R-TEXT-TERM2'   'null'                #        不越过终止符
Check 'L1'             '780A'                # readLine 行尾无 \r
Check 'L2'             '79'                  #        停在 \0 前
Check 'L3'             'null'                #        之后为空
Check 'WL'             '6F6E650D0A'          # writeLine 默认文本 = CRLF
Check 'W-BIN'          '650A66'              # 'wb'  : \n 原样

$arch = if ($isX64) { 'x64' } else { 'x86' }
if ($fail -eq 0) { Write-Host "== IOFILE-TEXT OK ($arch) =="; exit 0 }
Write-Host "== IOFILE-TEXT FAIL ($arch): $fail =="
exit 1
