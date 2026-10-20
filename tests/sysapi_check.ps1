# 系统 API 表回归：引擎的 `::Name` 解析表（engine/sysapi.cpp）
#
# 官方契约来源：
#   D:\aardio\lib\win\region\_.aardio:5-9
#       ::SetWindowRgn := ::User32.api("SetWindowRgn", "bool(addr hWnd, pointer hRgn, bool bRedraw)");
#       ::CreateRoundRectRgn := ::Gdi32.api( "CreateRoundRectRgn", "pointer(int,int,int,int,int,int)" );
#       ::CreateRectRgn := ::Gdi32.api("CreateRectRgn", "...");
#       ::GetWindowRgn := ::User32.api("GetWindowRgn", "int(addr hWnd, pointer hRgn)");
#       ::CombineRgn := ::Gdi32.api("CombineRgn", "...");
#   region API 实属 gdi32.dll。引擎的 API 表若把它们标成 user32.dll，则
#     · 裸 `::CreateRoundRectRgn` 解析不到导出；
#     · `::CreateRoundRectRgn := ...` 赋值语句的**左值**走同一张表 → 直接报
#       "找不到 API: CreateRoundRectRgn"，整个 win.region 模块导入失败。
#
# 用法: powershell -File sysapi_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$probe = @'
import gdi;
import win.ui;
import win.region;

io.print("RGN-NS " + type(win.region));
io.print("RGN-BARE " + type(::CreateRoundRectRgn));
io.print("RGN-BARE2 " + type(::CreateRectRgn));
io.print("RGN-BARE3 " + type(::CombineRgn));
io.print("RGN-GDI " + type(::Gdi32.api("CreateRoundRectRgn","pointer(int,int,int,int,int,int)")));
io.print("RGN-U32 " + type(::User32.api("SetWindowRgn","bool(addr,pointer,bool)")));
'@

$tmp = Join-Path $env:TEMP ("sysapi_check_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
[IO.File]::WriteAllText($tmp, $probe, (New-Object Text.UTF8Encoding($false)))

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$tmp`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
$psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $OverlayDir
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$p = [System.Diagnostics.Process]::Start($psi)
$o = $p.StandardOutput.ReadToEndAsync()
$e = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(3000) }
$out = (($o.Result + $e.Result) -replace "`r", '')
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

$fail = 0
function Check([string]$key, [string]$want) {
    if ($script:out -match ("(?m)^" + [regex]::Escape($key) + " " + [regex]::Escape($want) + "\s*$")) {
        Write-Host "PASS $key = $want"
    } else { $script:fail++; Write-Host "FAIL $key want=$want`n$script:out" }
}
Check 'RGN-NS'  'table'
Check 'RGN-BARE'  'native function'
Check 'RGN-BARE2' 'native function'
Check 'RGN-BARE3' 'native function'
Check 'RGN-GDI'   'native function'
Check 'RGN-U32'   'native function'

if ($fail -gt 0) { "== sysapi check FAILED ($fail) =="; exit 1 }
"== sysapi check OK =="
exit 0
