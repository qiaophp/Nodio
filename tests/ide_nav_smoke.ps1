# tests/ide_nav_smoke.ps1 — headless acceptance test for the code-navigation
# chain (go to definition / find references / rename / workspace symbols).
#
# The IDE runs in NODIO_IDE_NAV_SMOKE mode: it opens app/shell.nodio from the
# real project and asks the LSP server where `app.editor.contextMenu` is
# defined. contextMenu lives in app/editor.nodio, so a correct project-level
# symbol index must resolve it **across files**.
#
#   powershell -File tests/ide_nav_smoke.ps1 [-Exe <nodio.exe>] [-Ide <main.nodio>]
param(
    [string]$Exe    = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Ide    = 'D:\Nodio\ide\nodio\main.nodio',
    [string]$LibDir = 'D:\aardio\lib',
    [int]   $TimeoutSec = 120
)

if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }
if (-not (Test-Path $Ide)) { Write-Output "FAIL: IDE script not found: $Ide"; exit 1 }

$env:NODIO_LIBDIR         = $LibDir
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE            = $Exe
$env:NODIO_RUN            = $Ide
$env:NODIO_IDE_NAV_SMOKE  = '1'

$proj = Join-Path (Split-Path -Parent $Ide) 'ide.nproj'
if (Test-Path $proj) { $env:NODIO_IDE_PROJECT = $proj }

$log = Join-Path $env:TEMP ("nodio_ide_nav_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: IDE timed out after ${TimeoutSec}s"
    exit 1
}

$out = ''
$errOut = ''
# IDE 退出后子进程（LSP）可能还短暂持有日志句柄 —— 重试几次
for ($try = 1; $try -le 20; $try++) {
    try {
        if (Test-Path $log) { $out = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8) }
        if (Test-Path $err) { $errOut = [IO.File]::ReadAllText($err, [Text.Encoding]::UTF8) }
        break
    } catch {
        Start-Sleep -Milliseconds 250
    }
}
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue
if ($null -eq $out) { $out = '' }
if ($null -eq $errOut) { $errOut = '' }

Write-Output $out.TrimEnd()

if ($out -notmatch 'NAV-SMOKE-DONE') {
    Write-Output 'FAIL: navigation smoke did not complete'
    if ($errOut.Trim()) { Write-Output ('   stderr: ' + $errOut.Trim()) }
    exit 1
}
if ($out -notmatch 'NAV-SITE:[1-9]\d*,[1-9]\d*') { Write-Output 'FAIL: call site not located'; exit 1 }
# 导航栏（面包屑）：工程 › 目录 › 文件 › 所在函数
if ($out -notmatch 'NAV-SYMTREE:[1-9]\d*') {
    Write-Output 'FAIL: document symbol tree is empty (LSP documentSymbol)'
    exit 1
}
if ($out -notmatch 'NAV-CHAIN:nodio\|app\|shell\.nodio\|') {
    Write-Output 'FAIL: navigation bar chain is not "project > dir > file"'
    exit 1
}
if ($out -notmatch 'NAV-FNCHAIN:.*\|darkenRichEdit\|') {
    Write-Output 'FAIL: navigation bar does not show the enclosing function'
    exit 1
}
# 点击符号段必须跳到那个函数的定义行。
# ⚠ 不能写死行号：被测文件（ide/nodio/app/shell.nodio）一改，行号就变，
# 断言会莫名其妙地红（这次就是因为给 richedit 补 autoResize 多了 7 行）。
# 改成跟 IDE 自己报出来的目标行号比。
$fnLine = if ($out -match 'NAV-FNLINE:(\d+)') { $Matches[1] } else { '' }
$fnCur  = if ($out -match 'NAV-FNCUR:(\d+)')   { $Matches[1] } else { '' }
if (-not $fnLine -or -not $fnCur) {
    Write-Output 'FAIL: navigation smoke did not report the symbol line'
    exit 1
}
if ($fnCur -ne $fnLine) {
    Write-Output ("FAIL: clicking the symbol segment did not jump to it (want {0}, got {1})" -f $fnLine, $fnCur)
    exit 1
}
# 钩子链路：光标移动 → shell 的 hooks.onCursorMove → 组件 navChain 更新
if ($out -notmatch 'NAV-COMPCHAIN:.*darkenRichEdit') {
    Write-Output 'FAIL: shell hook onCursorMove did not reach the navigation bar'
    exit 1
}
# IDE 自己工程的源文件必须零诊断。LSP 服务端曾经把 JSON 里的 "\r"
# 解成了字面字母 'r'，于是 CRLF 缓冲区里凭空多出一个标识符，
# 引擎报出一条根本不存在的语法错误。
if ($out -notmatch 'NAV-DIAGS:0') {
    Write-Output 'FAIL: the IDE reported diagnostics on its own (valid) sources'
    exit 1
}
if ($out -notmatch 'NAV-DEF-COUNT:[1-9]\d*')      { Write-Output 'FAIL: go to definition found nothing'; exit 1 }
if ($out -notmatch 'NAV-DEF-PATH:.*app\\editor\.nodio') {
    Write-Output 'FAIL: definition did not resolve into app/editor.nodio (cross-file)'
    exit 1
}
if ($out -notmatch 'NAV-DEF-LINE:[1-9]\d*')       { Write-Output 'FAIL: definition has no line'; exit 1 }
if ($out -notmatch 'NAV-REF-COUNT:[2-9]\d*')      { Write-Output 'FAIL: find references did not span files'; exit 1 }
if ($out -notmatch 'NAV-RENAME-COUNT:[2-9]\d*')    { Write-Output 'FAIL: rename did not produce edits'; exit 1 }
if ($out -notmatch 'NAV-SYM-COUNT:[1-9]\d*')       { Write-Output 'FAIL: workspace symbol search found nothing'; exit 1 }
if ($out -notmatch 'NAV-REF-ROWS:[1-9]\d*')       { Write-Output 'FAIL: references panel was not filled'; exit 1 }

# 完整链路：F12 回调 → 跨文件打开 → 光标落到定义行
if ($out -notmatch 'NAV-JUMP-PATH:.*app\\editor\.nodio') {
    Write-Output 'FAIL: go to definition did not open the defining file'
    exit 1
}
if ($out -notmatch 'NAV-JUMP-LINE:44') {
    Write-Output 'FAIL: cursor did not land on the definition line'
    exit 1
}

# 跨文件重命名（临时工程）：两个文件都要改，旧的标识符要消失
if ($out -notmatch 'NAV-REN2-COUNT:[1-9]\d*') { Write-Output 'FAIL: rename produced no edits'; exit 1 }
if ($out -notmatch 'NAV-REN2-A:OK')  { Write-Output 'FAIL: rename did not update the closed file'; exit 1 }
if ($out -notmatch 'NAV-REN2-B:OK')  { Write-Output 'FAIL: rename did not update the open buffer'; exit 1 }
if ($out -notmatch 'NAV-REN2-AOLD:GONE') { Write-Output 'FAIL: old identifier survived the rename'; exit 1 }

Write-Output 'NAV SMOKE OK'
exit 0
