# 结构体布局回归 —— 锁住「官方 stuct 拼写容错」这个引擎级修复
#
# 官方 lib/string/markdown/_.aardio 的 html_renderopt 类里写的是：
#
#     class html_renderopt {
#         stuct toc_data = {          <-- 官方自己的拼写错误（struct 少一个 r）
#             int header_count;
#             int current_level;
#             int level_offset;
#         };
#         INT flags;
#         pointer link_attributes;
#     }
#
# aardio 容忍这个写法。Nodio 若不认 `stuct` 这个类型名：
#   · 解析器不把它当「结构体成员声明」→ 嵌套结构体整个从布局里消失；
#   · 于是 flags 落到偏移 0、link_attributes 落到偏移 4，类只有 16 字节；
#   · 而 sundown 的 C 侧按「24 字节 / flags 在偏移 12」去读
#     → 读到的是越界的垃圾（flags 实际落在指针的高位字节上）
#     → 渲染结果在 <hr> / <hr/> 之间随机跳变，含 "://" 的链接直接崩进程。
#
# 修法（引擎侧两处）：
#   engine/common.h  IsStructType() 增加 "stuct" 别名
#   engine/ffi.cpp   ParseStructDef() 把 "stuct" 归一化成 "struct"
#
# 本回归断言：
#   1) `stuct` 与 `struct` 两种拼写，序列化出来的字节**完全相同**；
#   2) 官方 html_renderopt 形状的布局 = 12(toc_data) + 4(flags) + 指针，
#      flags 落在偏移 12（两个架构都是 12）；
#   3) 官方 string.markdown 模块导出的 html_renderopt 布局同上
#      （x86 走官方库文件 —— 那个文件里写的就是 `stuct`）。
#
# 用法: powershell -File struct_alias_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}
$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)
$ptrSize = if ($isX64) { 8 } else { 4 }
$expectLen = 16 + $ptrSize          # toc_data(12) + flags(4) + pointer

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("st_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
    [IO.File]::WriteAllText($tmp, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$tmp`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $proc = [Diagnostics.Process]::Start($psi)
    $o = $proc.StandardOutput.ReadToEnd()
    $e = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    Remove-Item $tmp -ErrorAction SilentlyContinue
    return @{ out = $o; err = $e; code = $proc.ExitCode }
}

$fail = 0
function Check([string]$name, [string]$expect, [string]$actual) {
    if ($actual -eq $expect) { "  ok   $name = $actual" }
    else { "  FAIL $name = '$actual' (期望 '$expect')"; $script:fail++ }
}

# ---- 1. `stuct` 与 `struct` 两种拼写的布局必须完全一致 ----
# optOK   用正确拼写 struct；optTYPO 用官方那样的拼写 stuct。
# 两者字段一模一样，唯一差别就是类型名的拼写。
$probe = @'
class optOK {
	struct toc_data = {
		int header_count;
		int current_level;
		int level_offset;
	};
	INT flags;
	pointer link_attributes;
}
class optTYPO {
	stuct toc_data = {
		int header_count;
		int current_level;
		int level_offset;
	};
	INT flags;
	pointer link_attributes;
}
function hx(s){ var o=''; for(i=1; #s; i++){ o = o ++ string.format('%02x', s[i]); } return o; }
var a = optOK();
var b = optTYPO();
a.flags = 0x11223344;
b.flags = 0x11223344;
var ba = raw.buffer(a);
var bb = raw.buffer(b);
io.print("LEN_OK|" ++ #ba);
io.print("LEN_TYPO|" ++ #bb);
io.print("HEX_OK|" ++ hx(raw.tostring(ba,1,#ba)));
io.print("HEX_TYPO|" ++ hx(raw.tostring(bb,1,#bb)));
'@
$r = Run-Aardio $probe
if ($r.err) { "  NOTE stderr: $($r.err.Trim())" }
$m = @{}
foreach ($ln in ($r.out -split "`r?`n")) {
    if ($ln -match '^([A-Z_]+)\|([0-9a-f]*)\s*$') { $m[$Matches[1]] = $Matches[2] }
}
Check 'struct 拼写的长度' ([string]$expectLen) $(if ($m.ContainsKey('LEN_OK')) { $m['LEN_OK'] } else { '<缺失>' })
Check 'stuct 拼写的长度' ([string]$expectLen) $(if ($m.ContainsKey('LEN_TYPO')) { $m['LEN_TYPO'] } else { '<缺失>' })
Check '两种拼写字节完全相同' $m['HEX_OK'] $(if ($m.ContainsKey('HEX_TYPO')) { $m['HEX_TYPO'] } else { '<缺失>' })
# flags 必须落在偏移 12：第 13..16 个字节 = 44 33 22 11（小端 0x11223344）
$flagsAt12 = '44332211'
$hexOK = if ($m.ContainsKey('HEX_OK')) { $m['HEX_OK'] } else { '' }
Check 'flags 落在偏移 12' $flagsAt12 $(if ($hexOK.Length -ge 32) { $hexOK.Substring(24, 8) } else { '<太短>' })

# ---- 2. 官方 string.markdown 模块导出的 html_renderopt 布局 ----
# x86 走官方库文件（那个文件里写的就是 `stuct`）；
# x64 走 Nodio overlay（写的是正确拼写 `struct`，语义相同）。
$r2 = Run-Aardio @'
import string.markdown;
var o = string.markdown.html_renderopt();
o.flags = 0x11223344;
var b = raw.buffer(o);
io.print("LEN_OFFICIAL|" ++ #b);
'@
$lenOfficial = (($r2.out -split "`r?`n") | Where-Object { $_ -match '^LEN_OFFICIAL\|' }) -replace '^LEN_OFFICIAL\|', ''
Check '官方 html_renderopt 布局长度' ([string]$expectLen) $lenOfficial.Trim()

# ---- 3. stuct 作为普通成员类型名也必须被接受 ----
$r3 = Run-Aardio @'
class simple {
	stuct inner = {
		int a;
		int b;
		int c;
	};
	INT flags;
}
var s = simple();
io.print("OK3 " ++ #raw.buffer(s));
'@
$len3 = (($r3.out -split "`r?`n") | Where-Object { $_ -match '^OK3 ' }) -replace '^OK3 ', ''
Check 'stuct 嵌套结构体（3 个 int）' '16' $len3.Trim()

if ($fail -eq 0) { "STRUCT-ALIAS-OK"; exit 0 }
"STRUCT-ALIAS-FAIL ($fail)"; exit 1
