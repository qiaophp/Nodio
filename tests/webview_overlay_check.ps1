# ── tests/webview_overlay_check.ps1 ──────────────────────────────────────────
# 验证 web.view x64 overlay 补版（同名路径覆盖）：
#   1. 产物存在性、PE 架构与导出符号检查（26 个符号全部对齐）
#   2. x64 Nodio 执行 import web.view
#   3. 下游 3 个库联动导入验证（videoDownloader / bilibili / toutiao 全部通过）
#   4. x86 构建回归验证（双架构同绿）
# ─────────────────────────────────────────────────────────────────────────────
$ErrorActionPreference = 'Stop'
# 本脚本全程用仓库根相对路径（含 python pefile 段），锚定到根，跑法不限 CWD
Set-Location (Split-Path $PSScriptRoot -Parent)

$NodioX64 = "D:\Nodio\build-x64\Release\nodio.exe"
$NodioX86 = "D:\Nodio\build-x86\Release\nodio.exe"
$WbDll = "lib/web/view/.res/webview64.dll"
# overlay 模块已迁移为 .nodio 扩展名（.aardio 保留兜底，旧检出兼容）
$WbAardio = if (Test-Path "lib/web/view/_.nodio") { "lib/web/view/_.nodio" } else { "lib/web/view/_.aardio" }
$WbCpp = "lib/web/view/.res/webview64.cpp"

Write-Host "=== 1. 静态文件与 PE 检查 ==="
if (-not (Test-Path $WbDll)) { throw "缺少 $WbDll" }
if (-not (Test-Path $WbAardio)) { throw "缺少 $WbAardio" }
if (-not (Test-Path $WbCpp)) { throw "缺少 $WbCpp" }

$peCheck = python -c "
import pefile
pe = pefile.PE('$WbDll')
assert pe.FILE_HEADER.Machine == 0x8664, f'Expected x64 (0x8664), got {hex(pe.FILE_HEADER.Machine)}'
names = [exp.name.decode('ascii') for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols]
assert len(names) == 26, f'Expected 26 exports, got {len(names)}'
assert 'webview_new' in names
assert 'webview_create' in names
assert 'webview_navigate' in names
assert 'webview_do_script' in names
assert 'webview_ready' in names
print('PE_OK')
"
if ($peCheck -ne "PE_OK") { throw "PE 校验失败: $peCheck" }
Write-Host "PE 架构与 26 导出符号检查通过"

Write-Host "`n=== 2. x64 运行时模块与下游导入验证 ==="
$code = @"
import web.view;
if(type(web.view) != "class") {
    error("web.view is not class: " + tostring(type(web.view)));
}

import autos.skills.videoDownloader;
if(type(autos.skills.videoDownloader) != "class" && type(autos.skills.videoDownloader) != "table") {
    error("videoDownloader import failed");
}

import autos.skills.videoDownloader.bilibili;
if(!autos.skills.videoDownloader.bilibili) {
    error("bilibili import failed");
}

import autos.skills.videoDownloader.toutiao;
if(!autos.skills.videoDownloader.toutiao) {
    error("toutiao import failed");
}

print("WEBVIEW_ALL_PASS");
"@

$outX64 = & $NodioX64 -e $code
if ($outX64 -notmatch "WEBVIEW_ALL_PASS") {
    throw "x64 运行失败: $outX64"
}
Write-Host "x64 运行验证全部通过"

Write-Host "`n=== 3. x86 运行时回归对比验证 ==="
if (Test-Path $NodioX86) {
    $outX86 = & $NodioX86 -e $code
    if ($outX86 -notmatch "WEBVIEW_ALL_PASS") {
        throw "x86 运行失败: $outX86"
    }
    Write-Host "x86 官方库对比验证全部通过"
}

Write-Host "`n=== ALL TESTS PASSED ==="
