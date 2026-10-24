# import 契约回归：官方 aardio 的「库查找顺序 + 库自建命名空间」契约
#
# 官方契约来源（docs/library-guide/import.md）：
#   · 查找顺序只有 4 条路径（公共库）：
#       ~\lib\a.b.c.aardio / ~\lib\a\b.c.aardio / ~\lib\a\b\c.aardio / ~\lib\a\b\c\_.aardio
#     没有任何「顺带加载父命名空间」的路径。
#   · 「在 aardio 中库一定要自行创建与路径相同的命名空间。如果未创建一致的
#      命名空间，则导入库失败。」→ 命名空间链由库自身的 `namespace a.b;` 建立。
#   · 官方库显式声明依赖（lib\win\ui\_.aardio: import win; import win.ui.ctrl; ...）。
#
# 官方靠这条契约设置的 guard（直接导入子模块时必须报错）：
#   lib\web\form\query.aardio            "请直接导入web.form"
#   lib\web\view\dev.aardio              "只能通过导入 web.view 导入 web.view.dev"
#   lib\web\acme\httpClient.aardio       "请导入 web.acme 库"
#   lib\winex\accObject\element.aardio   "只能在 winex.accObject中 导入 winex.accObject.element"
#   lib\sqlite\base.aardio               "sqlite.base禁止直接导入"
#
# 修这条之前，Nodio 的 import 会自动预载父命名空间，上面 5 个官方 guard 全部失效。
#
# 用法: powershell -File import_contract_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$tmp = Join-Path $env:TEMP ("import_contract_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Run-Script([string]$code) {
    $f = Join-Path $tmp 'probe.aardio'
    [IO.File]::WriteAllText($f, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$f`""
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
    return (($o.Result + $e.Result) -replace "`r", '').Trim()
}

$fail = 0
function Expect-Err([string]$code, [string]$needle, [string]$label) {
    $out = Run-Script $code
    if ($out -like "*$needle*") { Write-Host "PASS $label" }
    else { $fail++; Write-Host "FAIL $label got=[$out] want=[$needle]" }
}
function Expect-Ok([string]$code, [string]$label) {
    $out = Run-Script $code
    if ($out -notmatch '运行错误' -and $out -notmatch 'error') {
        Write-Host "PASS $label"
    } else { $fail++; Write-Host "FAIL $label got=[$out]" }
}

# ---- 1) 官方 guard：直接导入子模块必须报错（错误文案与官方一致） ----
Expect-Err 'import web.form.query;'          '请直接导入web.form'                              'guard web.form.query'
Expect-Err 'import web.view.dev;'            '只能通过导入 web.view 导入 web.view.dev'         'guard web.view.dev'
Expect-Err 'import web.acme.httpClient;'     '请导入 web.acme 库'                              'guard web.acme.httpClient'
Expect-Err 'import winex.accObject.element;' '只能在 winex.accObject中 导入 winex.accObject.element' 'guard winex.accObject.element'
Expect-Err 'import sqlite.base;'              'sqlite.base禁止直接导入'                            'guard sqlite.base'

# ---- 2) 先导入父命名空间，子模块即可正常导入（官方用法） ----
Expect-Ok  "import web.form;`nimport web.form.query;`nio.print('OK');" 'parent-first web.form'
Expect-Ok  "import win.ui;`nimport win.ui.ctrl.plus;`nio.print('OK');" 'parent-first win.ui'

# sqlite 的官方 DLL 是 64 位（lib\sqlite\.res\sqlite3.dll），32 位构建加载不了，
# 这是 32 位 DLL 物理边界（见 capability-gap.md §3），不是 import 契约问题。
# 只有在父命名空间本身能导入时才断言子模块的绑定行为。
$sqliteParent = Run-Script 'import sqlite;'
if ($sqliteParent -like '*无法加载 DLL*') {
    Write-Host "SKIP parent-first sqlite (32-bit DLL boundary: $sqliteParent)"
} else {
    Expect-Ok  "import sqlite;`nimport sqlite.base;`nio.print('OK');" 'parent-first sqlite'
}

# ---- 3) 多层命名空间仍然绑定正确（父命名空间由库自身的 namespace 声明建立） ----
Expect-Ok  @'
import nodio.ui.rect;
import nodio.ui.theme;
import nodio.ui.board;
import nodio.designer.registry;
import win.ui.ctrl.plus;
import win.ui.tracker;
io.print("BOARD " + type(nodio.ui.board));
io.print("PLUS " + type(win.ui.ctrl.plus));
io.print("TRACKER " + type(win.ui.tracker));
'@ 'nested namespace binding'

# ---- 4) 未导入的父命名空间必须保持未定义（guard 的判据） ----
$out = Run-Script 'io.print("FORM " + type(web.form));'
if ($out -like '*FORM null*') { Write-Host "PASS parent stays undefined" }
else { $fail++; Write-Host "FAIL parent stays undefined got=[$out]" }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($fail -gt 0) { "== import contract FAILED ($fail) =="; exit 1 }
"== import contract OK =="
exit 0
