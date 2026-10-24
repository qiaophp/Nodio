# py2 / py3 overlay 回归 —— 锁住「按官方同名路径补 Nodio 自己的 Python 扩展库版本」
#
# 官方 py2 / py3 的命名空间结构：
#   lib/py2/_.aardio        父模块：载入 python27.dll，导出 ::Python2
#   lib/py2/{dict,export,list,module,object,tuple}.aardio   子模块
#   lib/py3/_.aardio        父模块：载入 python38.dll，导出 ::Python3
#   lib/py3/{dict,export,list,module,object,tuple}.aardio   子模块
#
# 两个硬约束：
#   1) 官方只带 32 位 python27.dll / python38.dll，x64 进程装不下；
#      仓库内已备好 64 位 python38.dll（lib/py3/.res/DLLs/），py3 因此在 x64 上可用。
#   2) 官方约定「子模块必须先 import 父命名空间」——子模块在类体里直接调
#      ::Python3.api(...)，父命名空间没载入时 ::Python3 为空。
#
# Nodio 侧按**官方同名路径**提供自己的版本（overlay 目录优先于官方库）：
#   lib/py3/_.aardio              官方副本，::Python3 与 ::Py3Runtime 指向同一句柄
#   lib/py3/{...}.aardio          官方副本，子模块自行载入运行时 -> 可独立导入
#   lib/py2/{...}.aardio          官方副本，64 位无 python27.dll 时优雅降级
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），此时行为与官方完全一致，本回归按架构分别断言。
#
# 用法: powershell -File py_overlay_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

# ---- 判定构建架构（overlay 只在 x64 生效）----
$pe = [IO.File]::ReadAllBytes($Exe)
$peOff = [BitConverter]::ToInt32($pe, 0x3C)
$machine = [BitConverter]::ToUInt16($pe, $peOff + 4)
$isX64 = ($machine -eq 0x8664)

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("py_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
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

# ---- 1. py3 基础功能：两个架构都必须可用 ----
$probe1 = @'
import py3;
io.print("PYVER " + py3.version);
io.print("PYEXEC " + type(py3.exec));
io.print("PYEVAL " + py3.eval("1+2"));
var d = py3.dict();
d.setItem("k","v");
var back = d.parseValue();
io.print("PYDICT " + (back && back.k == "v" ? "ok" : "bad"));
io.print("PYOBJ " + py3.object("hello").parseValue());
'@
$r = Run-Aardio $probe1
Check 'PYVER'  '3.8.10' (($r.out -split "`n" | Where-Object { $_ -match '^PYVER ' }) -replace '^PYVER ', '').Trim()
Check 'PYEXEC' 'function' (($r.out -split "`n" | Where-Object { $_ -match '^PYEXEC ' }) -replace '^PYEXEC ', '').Trim()
Check 'PYEVAL' '3' (($r.out -split "`n" | Where-Object { $_ -match '^PYEVAL ' }) -replace '^PYEVAL ', '').Trim()
Check 'PYDICT' 'ok' (($r.out -split "`n" | Where-Object { $_ -match '^PYDICT ' }) -replace '^PYDICT ', '').Trim()
Check 'PYOBJ'  'hello' (($r.out -split "`n" | Where-Object { $_ -match '^PYOBJ ' }) -replace '^PYOBJ ', '').Trim()

# ---- 2. py3 六个子模块的类型 ----
$probe2 = @'
import py3;
io.print("PYSUB " + type(py3.dict) + "," + type(py3.export) + "," + type(py3.list)
    + "," + type(py3.module) + "," + type(py3.object) + "," + type(py3.tuple));
'@
$r = Run-Aardio $probe2
Check 'PYSUB' 'class,class,class,class,class,class' (($r.out -split "`n" | Where-Object { $_ -match '^PYSUB ' }) -replace '^PYSUB ', '').Trim()

# ---- 3. 子模块独立导入（官方约定必须先 import 父命名空间）----
# x64：Nodio 的 overlay 让子模块可独立导入
# x86：走官方 32 位库，行为与官方一致 —— 独立导入会报错（::Python3 为空）
$subs = 'dict', 'export', 'list', 'module', 'object', 'tuple'
foreach ($s in $subs) {
    $r = Run-Aardio ("import py3.$s; io.print(`"SUBOK`");")
    if ($isX64) {
        if ($s -eq 'export') {
            # py3.export 在导入期就要执行 Python 代码（..py3.exec），官方同样要求先 import py3；
            # Nodio overlay 给出明确提示，而不是"尝试调用非函数值"这类内部错误。
            $ok = ($r.out -match 'SUBOK')
            Check 'py3.export 独立导入(需先 import py3)' 'expect-error' $(if ($ok) { 'no-error' } else { 'expect-error' })
            $msg = (($r.out + $r.err) -replace '\s+', ' ')
            if ($msg -match 'py3\.export 需要先 import py3') { "  ok   py3.export 提示明确" }
            else { "  FAIL py3.export 提示缺失: $msg"; $script:fail++ }
        }
        else {
            Check "py3.$s 独立导入" 'SUBOK' (($r.out -split "`n" | Where-Object { $_ -match '^SUBOK' }) -join '').Trim()
        }
    }
    else {
        $ok = ($r.out -match 'SUBOK')
        Check "py3.$s 独立导入(官方同款报错)" 'expect-error' $(if ($ok) { 'no-error' } else { 'expect-error' })
    }
}

# ---- 4. py2 ----
if ($isX64) {
    # x64：官方只带 32 位 python27.dll -> Nodio overlay 优雅降级
    $probe4 = @'
import py2;
io.print("PY2SUP " + (py2.isSupported === false ? "false" : "other"));
var threw = false, msg = "";
try { py2.exec("print(1)"); } catch(e) { threw = true; msg = e; }
io.print("PY2EXEC " + (threw && string.find(msg,"python27.dll") != null ? "clear" : "bad"));
'@
    $r = Run-Aardio $probe4
    Check 'PY2SUP'  'false' (($r.out -split "`n" | Where-Object { $_ -match '^PY2SUP ' }) -replace '^PY2SUP ', '').Trim()
    Check 'PY2EXEC' 'clear' (($r.out -split "`n" | Where-Object { $_ -match '^PY2EXEC ' }) -replace '^PY2EXEC ', '').Trim()

    # py2 子模块独立导入不崩溃；使用时给出明确提示
    foreach ($s in $subs) {
        $r = Run-Aardio ("import py2.$s; io.print(`"SUBOK`");")
        Check "py2.$s 独立导入不崩溃" 'SUBOK' (($r.out -split "`n" | Where-Object { $_ -match '^SUBOK' }) -join '').Trim()
    }
    $probe5 = @'
import py2.dict;
var threw = false, msg = "";
try { py2.dict(); } catch(e) { threw = true; msg = e; }
io.print("PY2DICT " + (threw && string.find(msg,"python27.dll") != null ? "clear" : "bad"));
'@
    $r = Run-Aardio $probe5
    Check 'PY2DICT' 'clear' (($r.out -split "`n" | Where-Object { $_ -match '^PY2DICT ' }) -replace '^PY2DICT ', '').Trim()
}
else {
    # x86：官方 32 位 python27.dll 可用，py2 真实执行
    $probe6 = @'
import py2;
io.print("PY2EVAL " + py2.eval("2+3"));
'@
    $r = Run-Aardio $probe6
    Check 'PY2EVAL' '5' (($r.out -split "`n" | Where-Object { $_ -match '^PY2EVAL ' }) -replace '^PY2EVAL ', '').Trim()
}

if ($fail -eq 0) { "PY-OVERLAY-OK"; exit 0 }
"PY-OVERLAY-FAIL ($fail)"; exit 1
