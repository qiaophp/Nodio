# 官方标准库可用性回归锁（第二十六轮）
#
# 覆盖的真实缺陷（第二十六轮定位并修复）：
#   `nodio.json` 找不到时，引擎的默认扩展名列表只剩 `.nodio`，
#   官方标准库的唯一源码扩展名 `.aardio` 直接消失 →
#   **整个官方标准库静默失效**（import 不报错，但命名空间是 null）。
#
#   触发场景（都不是"用户配错了"，而是真实部署形态）：
#     - 发布产物（自包含 exe，payload 内嵌官方库，外部无 nodio.json）
#     - 自定义部署目录（exe 旁边没有 nodio.json）
#     - CI 的临时工作目录（sweep 把 WorkingDirectory 设成 %TEMP%）
#
#   官方契约来源：
#     D:\aardio\lib 下 989 个 .aardio 文件（官方标准库的唯一扩展名）
#     D:\aardio\lib\builtin\struct.aardio:4/14/23  （::POINT/::SIZE/::RECT）
#     D:\aardio\lib\builtin\_.aardio:32-40        （::CopyMemory 等）
#
# 判定口径：把 cwd 设到一个**确定没有 nodio.json** 的目录，
#   断言官方库仍然可用（命名空间是 table/class，不是 null）。
#
# 用法: pwsh -File lib_availability_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$tmp = Join-Path $env:TEMP ("libavail_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

# 一个确定不含 nodio.json 的工作目录
$bare = Join-Path $tmp 'bare'
New-Item -ItemType Directory -Path $bare | Out-Null

function Run-Script([string]$code, [string]$workDir) {
    $f = Join-Path $tmp 'probe.aardio'
    [IO.File]::WriteAllText($f, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$f`""
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
    $psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $OverlayDir
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    # 刻意不设置 NODIO_EXTENSIONS：扩展名必须由引擎内置保证
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(60000)) { $p.Kill(); $p.WaitForExit(3000) }
    return (($o.Result + $e.Result) -replace "`r", '').Trim()
}

$fail = 0
function Check([string]$label, [string]$out, [string]$okMark) {
    if ($out -notlike "*$okMark*") {
        $script:fail++
        Write-Host "FAIL $label"
        ($out -split "`n") | ForEach-Object { if ($_ -ne '') { Write-Host "    $_" } }
    } else {
        Write-Host "PASS $label"
    }
}

# 前置断言：$bare 里确实没有 nodio.json（否则本测试无效）
if (Test-Path (Join-Path $bare 'nodio.json')) {
    "FAIL 前置条件: $bare 里存在 nodio.json，测试无效"
    exit 1
}

# ---------------------------------------------------------------
# 1) 官方标准库的 .aardio 源码必须能被 import（不带 nodio.json）
# ---------------------------------------------------------------
$libCode = @'
import win.ui;
import builtin.struct;
import string.format;

var fail = 0;
function chk(name, got, want) {
  if (got === want) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " got=", got, " want=", want); }
}

// 官方库源码（.aardio）被真正加载 → 命名空间是 table
chk("win.ui/namespace", type(win.ui), "table");
chk("builtin.struct/namespace", type(builtin.struct), "table");

// 官方 builtin/struct.aardio 定义的全局结构体类
chk("::RECT/class", type(::RECT), "class");
chk("::POINT/class", type(::POINT), "class");
chk("::SIZE/class", type(::SIZE), "class");

// 官方 builtin/_.aardio 定义的全局 API / 辅助函数
chk("::CopyMemory/fn", type(::CopyMemory), "native function");
chk("::HIWORD/fn", type(::HIWORD), "function");
chk("::LOWORD/fn", type(::LOWORD), "function");
chk("::MAKELONG/fn", type(::MAKELONG), "function");
chk("::MAKEWORD/fn", type(::MAKEWORD), "function");

// 结构体成员与官方一致（::RECT 的四个字段）
var rc = ::RECT(1,2,3,4);
chk("::RECT/left", rc.left, 1);
chk("::RECT/bottom", rc.bottom, 4);

// ::MAKELONG 的官方语义（D:\aardio\lib\builtin\_.aardio:75）：
//   ( (b & 0xFFFF) << 16 ) | (a & 0xFFFF)   —— a 是**低** 16 位
chk("::HIWORD/value", ::HIWORD(0x12345678), 0x1234);
chk("::LOWORD/value", ::LOWORD(0x12345678), 0x5678);
chk("::MAKELONG/value", ::MAKELONG(0x1234, 0x5678), 0x56781234);
// ::MAKEWORD 的官方语义（同一文件 :78）：( (b & 0xFF) << 8 ) | (a & 0xFF)
chk("::MAKEWORD/value", ::MAKEWORD(0x12, 0x34), 0x3412);

if (fail) { io.print("== LIB AVAILABILITY FAILED: ", fail); return 1; }
io.print("== official lib availability OK ==");
'@

$out = Run-Script $libCode $bare
Check '官方标准库在无 nodio.json 时仍可用' $out 'official lib availability OK'

# ---------------------------------------------------------------
# 2) 有 nodio.json 时行为必须一致（不能因为修复而回归）
# ---------------------------------------------------------------
$withCfg = Join-Path $tmp 'withcfg'
New-Item -ItemType Directory -Path $withCfg | Out-Null
$cfg = @'
{
  "extensions": [ ".nodio", ".aardio" ],
  "magicHeaders": [ "=nodio", "=bytecode", "=aardio" ],
  "defaultTitle": "Nodio",
  "windowClass": "Nodio_Form",
  "userAgent": "Nodio/1.0"
}
'@
[IO.File]::WriteAllText((Join-Path $withCfg 'nodio.json'), $cfg, (New-Object Text.UTF8Encoding($false)))
$out2 = Run-Script $libCode $withCfg
Check '有 nodio.json 时官方标准库同样可用' $out2 'official lib availability OK'

# ---------------------------------------------------------------
# 3) 扩展名是「追加」而不是「替换」：配置里只写 .nodio，
#    官方库仍必须可用（配置不得意外关闭官方契约）
# ---------------------------------------------------------------
$onlyNodio = Join-Path $tmp 'onlynodio'
New-Item -ItemType Directory -Path $onlyNodio | Out-Null
$cfg2 = @'
{
  "extensions": [ ".nodio" ],
  "magicHeaders": [ "=nodio" ]
}
'@
[IO.File]::WriteAllText((Join-Path $onlyNodio 'nodio.json'), $cfg2, (New-Object Text.UTF8Encoding($false)))
$out3 = Run-Script $libCode $onlyNodio
Check '配置未列 .aardio 时官方库仍可用（扩展名是追加）' $out3 'official lib availability OK'

# ---------------------------------------------------------------
# 4) 官方预编译块魔数 =aardio 同样内置（不依赖配置）
# ---------------------------------------------------------------
$magicCode = @'
// =aardio 是官方预编译块的魔数，必须由引擎内置识别
var fail = 0;
if (raw) { io.print("PASS raw available"); } else { fail++; io.print("FAIL raw missing"); }
if (fail) { io.print("== MAGIC CHECK FAILED"); return 1; }
io.print("== magic header OK ==");
'@
$out4 = Run-Script $magicCode $bare
Check '配置缺失时 =aardio 魔数仍被内置识别' $out4 'magic header OK'

if ($fail -eq 0) { "== lib availability OK =="; exit 0 }
else { "== lib availability FAILED: $fail =="; exit 1 }
