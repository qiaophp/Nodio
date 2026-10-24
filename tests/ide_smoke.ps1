# Self-hosted IDE smoke test — runs ide/nodio/main.nodio on the Nodio VM in
# headless smoke mode (NODIO_IDE_SMOKE=1): the IDE builds its DSG layout,
# wires the standard-library docks (nodio.ui.*), exercises the designer
# (nodio.designer.*), writes a probe script, runs it through the F5
# process.popen pipeline and reports whether the child's output round-tripped
# to the output pane.
#
# The IDE replaces the retired native Win32 + Scintilla shell, so this is the
# IDE acceptance test (it no longer pokes Scintilla child windows).
#
#   powershell -File tests/ide_smoke.ps1 [-Exe <nodio.exe>] [-Ide <main.nodio>]
param(
    [string]$Exe     = 'D:\Nodio\build-x64\Release\nodio.exe',
    [string]$Ide     = 'D:\Nodio\ide\nodio\main.nodio',
    [string]$LibDir  = 'D:\aardio\lib',
    [int]   $TimeoutSec = 90
)

if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }
if (-not (Test-Path $Ide)) { Write-Output "FAIL: IDE script not found: $Ide"; exit 1 }

# overlay source guard: the official aardio for-step is `1` (not `1++`).
# `1++` increments a literal, which the parser now rejects outright — but
# only when the module is actually compiled, so a typo in a module the IDE
# does not import would stay hidden. Scan the whole overlay instead.
$bad = @()
foreach ($dir in @('D:\Nodio\lib', 'D:\Nodio\ide')) {
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem $dir -Recurse -Include '*.aardio', '*.nodio' -File | ForEach-Object {
        $n = 0
        foreach ($line in [IO.File]::ReadAllLines($_.FullName, [Text.Encoding]::UTF8)) {
            $n++
            # 只看 for 头的第三个子句（步长）——循环体里的 `++` 是合法的
            # （`for(i=1;#lines;1) out[i] = INDENT ++ lines[i];`）。
            if ($line -match 'for\s*\(([^;]*;[^;]*;[^)]*)\)') {
                $clauses = $matches[1] -split ';'
                if ($clauses.Count -ge 3 -and $clauses[2] -match '\+\+') {
                    $bad += ("{0}:{1}: {2}" -f $_.FullName, $n, $line.Trim())
                }
            }
        }
    }
}
if ($bad.Count -gt 0) {
    Write-Output 'FAIL: overlay has a constant-increment loop step (aardio wants `1`)'
    $bad | ForEach-Object { Write-Output ("   " + $_) }
    exit 1
}

$env:NODIO_LIBDIR    = $LibDir
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$env:NODIO_EXE       = $Exe
$env:NODIO_RUN       = $Ide
$env:NODIO_IDE_SMOKE = '1'

# the IDE's own project file drives the project-model check
$proj = Join-Path (Split-Path -Parent $Ide) 'ide.nproj'
if (Test-Path $proj) { $env:NODIO_IDE_PROJECT = $proj }

$log = Join-Path $env:TEMP ("nodio_ide_smoke_{0}.log" -f $PID)
$err = "$log.err"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $log -RedirectStandardError $err
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    Write-Output "FAIL: IDE timed out after ${TimeoutSec}s"
    exit 1
}

# the engine writes UTF-8; Windows PowerShell 5.1 would read it as the ANSI
# code page and turn every Chinese string into mojibake
#
# ⚠ 刚退出就读重定向文件偶尔会撞上「另一个进程正在使用此文件」——
#   Start-Process 的管道句柄释放有竞态。读失败重试几次即可。
function Read-TextRetry([string]$path) {
    for ($i = 0; $i -lt 20; $i++) {
        try {
            if (Test-Path $path) { return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) }
            return ''
        } catch {
            Start-Sleep -Milliseconds 150
        }
    }
    return ''
}
$out = Read-TextRetry $log
$errOut = Read-TextRetry $err
Remove-Item $log, $err -Force -ErrorAction SilentlyContinue
if ($null -eq $out) { $out = '' }
if ($null -eq $errOut) { $errOut = '' }

Write-Output $out.TrimEnd()

if ([string]::IsNullOrWhiteSpace($out)) {
    Write-Output ('FAIL: IDE produced no stdout' + $(if ($errOut.Trim()) { " (stderr: " + $errOut.Trim() + ")" } else { '' }))
    exit 1
}
if ($out -notmatch 'IDE-SMOKE-OK') {
    if ($out -match 'IDE-SMOKE-NO-ECHO') {
        Write-Output 'FAIL: run pipeline produced no echoed output'
    } else {
        Write-Output 'FAIL: IDE did not reach completion'
    }
    if ($errOut.Trim()) { Write-Output ('   stderr: ' + $errOut.Trim()) }
    exit 1
}

# project model: ide.nproj must parse and resolve its start file
if ($env:NODIO_IDE_PROJECT) {
    if ($out -notmatch 'PROJ-FILES:\d+') { Write-Output 'FAIL: project files not parsed'; exit 1 }
    if ($out -notmatch 'PROJ-START:')     { Write-Output 'FAIL: project start file not resolved'; exit 1 }
    # 工程树 = 实时文件系统视图：节点数 > 1（根 + 子项）
    if ($out -notmatch 'TREE-NODES:\d+')   { Write-Output 'FAIL: project tree was not built'; exit 1 }
    # 左栏「搜索」页：范围来自工程文件，搜索能真跑出结果
    if ($out -notmatch 'SEARCH-SCOPE:\d+') { Write-Output 'FAIL: search scope is empty'; exit 1 }
    if ($out -notmatch 'SEARCH-HITS:[1-9]\d*') { Write-Output 'FAIL: full-text search found nothing'; exit 1 }
    if ($out -notmatch 'SEARCH-ROWS:[1-9]\d*') { Write-Output 'FAIL: search results were not listed'; exit 1 }
    # 工程清单维护：加一条 → 改一条 → 写回 → 删一条 → 再写回
    if ($out -notmatch 'PROJOPS-COUNT:2')   { Write-Output 'FAIL: manifest parse count wrong'; exit 1 }
    if ($out -notmatch 'PROJOPS-AFTER:3')   { Write-Output 'FAIL: manifest add did not persist'; exit 1 }
    if ($out -notmatch 'PROJOPS-REMOVED:2') { Write-Output 'FAIL: manifest remove did not persist'; exit 1 }
    if ($out -notmatch 'PROJOPS-PATHS:app\\c\.nodio,app\\b\.nodio') {
        Write-Output 'FAIL: manifest rename did not persist'; exit 1
    }
    if ($out -notmatch 'PROJOPS-REL:app\\c\.nodio') { Write-Output 'FAIL: relPath is wrong'; exit 1 }
}

# 钩子链路：shell.hooks.onOpenFile → main.nodio 的 openFile → 真的打开页签
# （模块级钩子曾被模块帧的局部副本遮蔽，整片是死的 —— 见 capability-gap §1.20.3）
if ($out -notmatch 'SMOKE-HOOK-OPEN:.*nodio_ide_hook\.aardio') {
    Write-Output 'FAIL: shell hook onOpenFile did not open the file'
    exit 1
}

# layout: the three docks (left/right/bottom) must be materialised
if ($out -notmatch 'DOCK-LEFT-STRIP:1')    { Write-Output 'FAIL: left dock strip missing'; exit 1 }
if ($out -notmatch 'DOCK-RIGHT-CONTENT:1') { Write-Output 'FAIL: right dock content missing'; exit 1 }
if ($out -notmatch 'DOCK-BOTTOM-CONTENT:1'){ Write-Output 'FAIL: bottom dock content missing'; exit 1 }
if ($out -notmatch 'LAYOUT-OK:')           { Write-Output 'FAIL: layout not computed'; exit 1 }

# designer: a custom component must register, land on the canvas and emit DSG
if ($out -notmatch 'DESIGNER-ITEM:\w')   { Write-Output 'FAIL: designer canvas item missing'; exit 1 }
if ($out -notmatch 'DESIGNER-DSG:.*DSG') { Write-Output 'FAIL: designer did not emit DSG'; exit 1 }
if ($out -notmatch 'TOOLBOX-COUNT:\d+')  { Write-Output 'FAIL: component registry empty'; exit 1 }

# DSG must carry the component geometry as its own keys.
# text= is optional: a component's 标题 property defaults to the component
# name (VS behaviour), so the DSG usually carries it too.
if ($out -notmatch 'button1=\{cls="button";(text="[^"]*";)?left=\d+;top=\d+;right=\d+;bottom=\d+\}') {
    Write-Output 'FAIL: DSG does not carry component geometry'
    exit 1
}

# property grid write-back: the value typed in the grid must reach the control
if ($out -notmatch 'PROP-WRITEBACK:item=\d+ ctrl=\d+ prop=\d+') {
    Write-Output 'FAIL: property grid did not write back the numeric value'
    exit 1
}
# 设计坐标是**逻辑像素**，控件实际尺寸 = 逻辑值 × DPI 缩放系数
# （官方 win.ui 控件按 DPI 缩放，设计器必须跟着缩放才是所见即所得）
$scale = 1.0
if ($out -match 'DPI-SCALE:([0-9.]+)') { $scale = [double]$Matches[1] }
$expectedCtrl = [int][Math]::Round(160 * $scale)
if ($out -notmatch ("PROP-WRITEBACK:item=160 ctrl=$expectedCtrl prop=160")) {
    Write-Output "FAIL: property write-back did not resize the control to 160 (scale=$scale, expected ctrl=$expectedCtrl)"
    exit 1
}
if ($out -notmatch 'PROP-TEXT:.+') {
    Write-Output 'FAIL: property grid did not write back the text value'
    exit 1
}

# DSG round-trip: types survive and the regenerated DSG is byte-identical
if ($out -notmatch 'DSG-ROUNDTRIP:true items=\d+') {
    Write-Output 'FAIL: DSG round-trip is not stable'
    exit 1
}
if ($out -notmatch 'DSG-TYPES:left=number text=string') {
    Write-Output 'FAIL: DSG lost value types across the round-trip'
    exit 1
}

# self-drawn document tab strip: add / remove must keep the count in sync
if ($out -notmatch 'TABSTRIP-COUNT:1')        { Write-Output 'FAIL: document tab strip did not open the first tab'; exit 1 }
if ($out -notmatch 'TABSTRIP-AFTER-ADD:2')    { Write-Output 'FAIL: document tab strip did not add a tab'; exit 1 }
if ($out -notmatch 'TABSTRIP-AFTER-REMOVE:1') { Write-Output 'FAIL: document tab strip did not remove a tab'; exit 1 }

# 事件代码生成：双击组件 → 源码里出现官方形状的事件桩；再来一次只跳转不重复
if ($out -notmatch 'EVENT-STUB:1')     { Write-Output 'FAIL: double-click did not generate the event stub'; exit 1 }
if ($out -notmatch 'EVENT-STUB-DUP:1') { Write-Output 'FAIL: second double-click duplicated the event stub'; exit 1 }
if ($out -notmatch 'EVENT-CODE-VIEW:1'){ Write-Output 'FAIL: double-click did not switch to the code view'; exit 1 }

# 设计 ↔ 源码同步（switchActiveForm）：开设计器载入源码的设计块，
# 关设计器把当前设计写回源码（编辑器缓冲）
if ($out -notmatch 'SYNC-ITEMS:1')      { Write-Output 'FAIL: opening the designer did not load the DSG from source'; exit 1 }
if ($out -notmatch 'SYNC-WRITEBACK:1')  { Write-Output 'FAIL: closing the designer did not write the design back to source'; exit 1 }
if ($out -notmatch 'SYNC-BLOCKS:1')     { Write-Output 'FAIL: design sync duplicated the DSG block'; exit 1 }

# 官方 ide.customCtrl 替身映射：textbox 用 edit 代画，DSG 仍写真实类名
if ($out -notmatch 'CUSTOMCTRL-MAP:edit')          { Write-Output 'FAIL: official ide.customCtrl mapping missing'; exit 1 }
if ($out -notmatch 'CUSTOMCTRL-DRAW:textbox/edit') { Write-Output 'FAIL: surrogate must draw as edit while the DSG keeps the real class'; exit 1 }
# 窗体设置：选中窗体本身 → 属性格改 标题/宽度/高度 → DSG 头跟着变
if ($out -notmatch 'FORM-SET:111')      { Write-Output 'FAIL: form settings did not reach the DSG header'; exit 1 }
# 网格吸附：拖动/缩放坐标钉到网格（30→32、33→32）
if ($out -notmatch 'SNAP:32/32')        { Write-Output 'FAIL: grid snapping is broken'; exit 1 }

# 文档闭环：脏标记是推导的（当前文本 vs 上次保存的文本）
if ($out -notmatch 'DOC-DIRTY-INIT:false') { Write-Output 'FAIL: a freshly opened document must be clean'; exit 1 }
if ($out -notmatch 'DOC-DIRTY-SET:true')  { Write-Output 'FAIL: dirty marker was not set'; exit 1 }
if ($out -notmatch 'DOC-TEXT-LEN:\d+')     { Write-Output 'FAIL: document text is not readable'; exit 1 }
if ($out -notmatch 'DOC-SAVE-LEN:\d+')     { Write-Output 'FAIL: save text is not derivable'; exit 1 }
# 编辑器组件：行号栏 / 行列号回调 / 缩进 / 注释 / 撤销
if ($out -notmatch 'DOC-GUTTER:\d+')       { Write-Output 'FAIL: line number gutter missing'; exit 1 }
if ($out -notmatch 'DOC-LINES:\d+')        { Write-Output 'FAIL: editor line count missing'; exit 1 }
if ($out -notmatch 'DOC-CURSOR:')          { Write-Output 'FAIL: cursor position is not reported to the status bar'; exit 1 }
if ($out -notmatch 'DOC-INDENT-LEN:\d+')    { Write-Output 'FAIL: indent command did not change the text'; exit 1 }
if ($out -notmatch 'DOC-COMMENT-LEN:\d+')   { Write-Output 'FAIL: comment toggle did not change the text'; exit 1 }
if ($out -notmatch 'DOC-UNDO-LEN:30')       { Write-Output 'FAIL: editor undo did not revert the last edit'; exit 1 }

# every dock / editor must be visible once the window is shown
foreach ($v in @('VIS-LEFT','VIS-RIGHT','VIS-BOTTOM','VIS-DOC','VIS-ED','VIS-TOOLBOX')) {
    if ($out -notmatch ($v + ':true')) { Write-Output "FAIL: $v is not visible"; exit 1 }
}

# 属性格分组带（VS WinForms 属性窗口的「分类」）：
# 分类映射 / 显示行表 / 折叠 / 就地编辑框落位 —— 见 capability-gap §1.22
$probe = Join-Path $PSScriptRoot 'dsg_propgrid_probe.aardio'
if (Test-Path $probe) {
    $plog = Join-Path $env:TEMP ("nodio_propgrid_{0}.log" -f $PID)
    $perr = "$plog.err"
    $env:NODIO_RUN = $probe
    $pp = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
            -RedirectStandardOutput $plog -RedirectStandardError $perr
    if (-not $pp.WaitForExit(60 * 1000)) { try { $pp.Kill() } catch {} }
    $pout = if (Test-Path $plog) { [IO.File]::ReadAllText($plog, [Text.Encoding]::UTF8) } else { '' }
    Remove-Item $plog, $perr -Force -ErrorAction SilentlyContinue
    $env:NODIO_RUN = $Ide
    if ($pout -notmatch 'PROPGRID-OK') {
        Write-Output 'FAIL: property grid category bands probe failed'
        Write-Output $pout
        exit 1
    }
}

Write-Output 'SMOKE OK'
exit 0
