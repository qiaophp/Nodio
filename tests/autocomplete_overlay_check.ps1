# com.autoComplete x64 补版回归
#
# 官方 lib/com/autoComplete/_.aardio 加载 32 位 autoComplete.dll（VC6 编译的 MFC IDispatch 封装），
# x64 进程装不下该 PE（实测「无法加载 DLL: C:\Users\QQ\AppData\Local\aardio/std/com/autoComplete.dll」）。
#
# Nodio 侧按官方同名路径提供自写实现：
#   lib/com/autoComplete/_.aardio              官方逐字副本，只换 DLL 来源
#   lib/com/autoComplete/.res/autoComplete64.cpp Nodio 自写 C++ COM 自动完成实现（零 MFC 依赖，源码入库）
#   lib/com/autoComplete/.res/autoComplete64.dll 用 MSVC 编出的 x64 DLL
#
# 导出符号与官方 32 位 DLL 一致：
#   CreateIDispatchObject(HWND hwndEdit, DWORD dwOptions)
#
# 用法: powershell -File tests/autocomplete_overlay_check.ps1 [-Exe <nodio.exe>]
param(
    [string]$Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$pass = 0
$fail = 0

function Assert-True([string]$desc, [bool]$cond) {
    if ($cond) {
        Write-Host "  PASS: $desc" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "  FAIL: $desc" -ForegroundColor Red
        $script:fail++
    }
}

function Run-Aardio([string]$code) {
    $tmp = [System.IO.Path]::GetTempFileName() + '.aardio'
    [System.IO.File]::WriteAllText($tmp, $code, [System.Text.UTF8Encoding]::new($false))
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList "`"$tmp`"" -NoNewWindow -PassThru -Wait -RedirectStandardOutput ([System.IO.Path]::GetTempPath() + '\ac_out.txt') -RedirectStandardError ([System.IO.Path]::GetTempPath() + '\ac_err.txt')
        $stdout = [System.IO.File]::ReadAllText([System.IO.Path]::GetTempPath() + '\ac_out.txt', [System.Text.Encoding]::UTF8)
        $stderr = [System.IO.File]::ReadAllText([System.IO.Path]::GetTempPath() + '\ac_err.txt', [System.Text.Encoding]::UTF8)
        return [PSCustomObject]@{
            ExitCode = $p.ExitCode
            Out      = $stdout.Trim()
            Err      = $stderr.Trim()
        }
    } finally {
        if (Test-Path $tmp) {
            # use mavis-trash or leave temp
        }
    }
}

Write-Host "=== com.autoComplete x64 overlay 回归（$Arch）===" -ForegroundColor Cyan

# 1. 静态产物断言
Write-Host "`n[1. 静态产物断言]" -ForegroundColor Yellow
$overlayAardio = 'D:\Nodio\lib\com\autoComplete\_.aardio'
$overlayCpp    = 'D:\Nodio\lib\com\autoComplete\.res\autoComplete64.cpp'
$overlayDll    = 'D:\Nodio\lib\com\autoComplete\.res\autoComplete64.dll'

Assert-True "overlay 模块落盘" (Test-Path $overlayAardio)
Assert-True "overlay C++ 源码落盘" (Test-Path $overlayCpp)
Assert-True "overlay x64 DLL 落盘" (Test-Path $overlayDll)

if (Test-Path $overlayDll) {
    $bytes = [System.IO.File]::ReadAllBytes($overlayDll)
    $peOff = [System.BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [System.BitConverter]::ToUInt16($bytes, $peOff + 4)
    Assert-True "autoComplete64.dll 为 x64 PE (0x8664)" ($machine -eq 0x8664)
} else {
    Assert-True "autoComplete64.dll 为 x64 PE (0x8664)" $false
}

# 2. 导出符号断言
Write-Host "`n[2. 导出符号断言]" -ForegroundColor Yellow
$vc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
$dumpbin = "$vc\bin\Hostx64\x64\dumpbin.exe"
if (Test-Path $dumpbin) {
    $dumpOut = & $dumpbin /exports $overlayDll 2>&1 | Out-String
    Assert-True "导出符号包含 CreateIDispatchObject" ($dumpOut -match 'CreateIDispatchObject')
} else {
    Assert-True "dumpbin 可用" $false
}

# 3. 模块解析路径断言
Write-Host "`n[3. 模块解析路径断言]" -ForegroundColor Yellow
$r = Run-Aardio @"
import io;
var p = io.libpath("com.autoComplete");
print("LIBPATH:" + tostring(p));
"@
if ($Arch -eq 'x64') {
    Assert-True "x64 命中 Nodio overlay 路径" ($r.Out -match 'D:\\Nodio\\lib\\com\\autoComplete')
} else {
    Assert-True "x86 命中官方库路径" ($r.Out -match 'D:\\aardio\\lib\\com\\autoComplete')
}

# 4. API 面断言
Write-Host "`n[4. API 面断言]" -ForegroundColor Yellow
$r = Run-Aardio @"
import com.autoComplete;
print("TYPE:" + type(com.autoComplete));
"@
Assert-True "com.autoComplete 为 class" ($r.Out -match 'TYPE:class')

# 5. 功能与生命周期断言
Write-Host "`n[5. 功能与生命周期断言]" -ForegroundColor Yellow
$r = Run-Aardio @"
import win.ui;
import com.autoComplete;

var winform = win.form(text="test autoComplete";right=300;bottom=200);
winform.add(
    edit={cls="edit";left=20;top=20;right=200;bottom=50;edge=1}
);

var aco = com.autoComplete(winform.edit);
if(!aco) { print("FAIL:null"); return; }

print("ACO_TYPE:" + type(aco));
print("ACO_SETSTRINGS:" + type(aco.setStrings));
print("ACO_SETOPTION:" + type(aco.setOption));
print("ACO_ENABLE:" + type(aco.enable));
print("ACO_SHOW:" + type(aco.show));
print("ACO_HIDE:" + type(aco.hide));

aco.setStrings({"alpha", "beta", "gamma"});
print("SETSTRINGS_OK");

aco.setOption(1 | 2);
print("SETOPTION_OK");

aco.enable(true);
aco.enable(false);
aco.enable(true);
print("ENABLE_OK");

// 重复获取应返回同一实例
var aco2 = com.autoComplete(winform.edit);
print("SAME_INSTANCE:" + tostring(aco == aco2));

// 显式 delete
aco.delete();
print("DELETE_OK");

// 窗体关闭触发 onDestroy
winform.close();
print("CLOSE_OK");
"@

Assert-True "aco 实例为 table" ($r.Out -match 'ACO_TYPE:table')
Assert-True "setStrings 方法存在" ($r.Out -match 'ACO_SETSTRINGS:table')
Assert-True "setOption 方法存在" ($r.Out -match 'ACO_SETOPTION:table')
Assert-True "enable 方法存在" ($r.Out -match 'ACO_ENABLE:table')
Assert-True "show 方法存在" ($r.Out -match 'ACO_SHOW:function')
Assert-True "hide 方法存在" ($r.Out -match 'ACO_HIDE:function')
Assert-True "setStrings 执行成功" ($r.Out -match 'SETSTRINGS_OK')
Assert-True "setOption 执行成功" ($r.Out -match 'SETOPTION_OK')
Assert-True "enable(true/false) 执行成功" ($r.Out -match 'ENABLE_OK')
Assert-True "重复构造返回同一单例对象" ($r.Out -match 'SAME_INSTANCE:true')
Assert-True "delete 执行成功" ($r.Out -match 'DELETE_OK')
Assert-True "窗体正常关闭无异常" ($r.Out -match 'CLOSE_OK')

# 6. 参数防护断言
Write-Host "`n[6. 参数防护断言]" -ForegroundColor Yellow
$r = Run-Aardio @"
import com.autoComplete;
try {
    com.autoComplete(null);
    print("PARAM_FAIL");
} catch(e) {
    print("PARAM_GUARD_OK");
}
"@
Assert-True "非法参数抛出防护异常" ($r.Out -match 'PARAM_GUARD_OK')

Write-Host "`n============================================================"
Write-Host "  结果: $pass passed, $fail failed" -ForegroundColor ($fail -eq 0 ? 'Green' : 'Red')
Write-Host "============================================================"

if ($fail -ne 0) { exit 1 }
