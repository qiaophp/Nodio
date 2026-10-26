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
# 冒烟不许写用户的真实配置（%APPDATA%\Nodio\ide.cfg）—— 用临时文件隔离
$env:NODIO_IDE_CFG  = Join-Path $env:TEMP 'nodio_ide_smoke_cfg.nodie'
if (Test-Path $env:NODIO_IDE_CFG) { Remove-Item $env:NODIO_IDE_CFG -Force }
$env:NODIO_EXE       = $Exe
$env:NODIO_RUN       = $Ide
$env:NODIO_IDE_SMOKE = '1'
# AI 面板冒烟要写一条 API Key —— 必须用**隔离的凭据名**，
# 否则 apiKeyRemove() 会把用户自己配的那条凭据抹掉。
$env:NODIO_AI_CRED_TARGET = 'NodioIDE/test/ide_smoke'

# ---- 本地 mock OpenAI 服务器（AI 面板的「成功回包」路径）----
#
# 面板的成功分支（AI 的回答进日志 + 进会话）以前从没被断言过 ——
# 只有失败分支。用一个只依赖标准库的 mock 把这条路径也变成无头断言：
#   · 不依赖外网、不依赖用户真实 Key
#   · 回包内容固定（"MOCK-OK …"），断言不会飘
#
# ⚠ **Python 是硬依赖**：没有它就起不来 mock → AI 成功路径根本没法断言 →
#   这条回归就只剩失败路径，等于白跑。所以这里**明确报错**，不做静默降级
#   （静默降级 = 锁不住东西，却看起来是绿的）。
#   Python 的找法与 tests/run_vm_tests.ps1 一致：先找 PATH，再找固定路径。
$mockProc = $null
$mockLog  = Join-Path $env:TEMP ("nodio_ai_mock_{0}.out" -f $PID)
$mockErr  = "$mockLog.err"
$mockScript = Join-Path $PSScriptRoot 'ai_mock_server.py'
if (-not (Test-Path $mockScript)) {
    Write-Output "FAIL: AI mock server script not found: $mockScript"
    exit 1
}
$pyCmd = Get-Command 'python' -ErrorAction SilentlyContinue
$py = ''
if ($pyCmd) {
    # ApplicationInfo → 取完整路径；命中别名/函数就退回命令名（Start-Process 自己解析 PATH）
    if ($pyCmd -is [System.Management.Automation.ApplicationInfo]) { $py = $pyCmd.Path }
    else { $py = 'python' }
}
if (-not $py -and (Test-Path 'C:\Python311\python.exe')) { $py = 'C:\Python311\python.exe' }
if (-not $py) {
    Write-Output 'FAIL: python not found (neither PATH nor C:\Python311\python.exe) - the IDE smoke needs it to run the local AI mock server'
    exit 1
}
# 别让外层环境里残留的端口号混进来
$env:NODIO_IDE_AI_MOCK_PORT = ''

# ---- 主体全包在 try 里 ----
# 下面有十几条 exit 1 的断言失败路径，任何一条都得先把 mock 收掉，
# 否则它会变成孤儿进程留着（还会占着调用方的输出管道）。
# PowerShell 的 exit 在 try 内也会走 finally，所以这里统一在 finally 收尾。
try {

# 起 mock。--parent-pid 让它「父进程一死就自己退出」：
# 不给这个参数的话，mock 的 stdin 在 Start-Process 下**立刻 EOF**
# → 它会自己 shutdown 掉 → 端口拿到了但一连就 Connection refused。
try {
    $mockProc = Start-Process -FilePath $py -ArgumentList @($mockScript, '--parent-pid', "$PID") `
        -PassThru -NoNewWindow -RedirectStandardOutput $mockLog -RedirectStandardError $mockErr
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 200
        if (Test-Path $mockLog) {
            $line = Get-Content $mockLog -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($line -and $line -match 'READY (\d+)') {
                $env:NODIO_IDE_AI_MOCK_PORT = $Matches[1]
                break
            }
        }
    }
} catch {
    $mockProc = $null
}
if (-not $env:NODIO_IDE_AI_MOCK_PORT) {
    Write-Output 'FAIL: local AI mock server did not become ready'
    try {
        if (Test-Path $mockErr) {
            $e = [IO.File]::ReadAllText($mockErr, [Text.Encoding]::UTF8)
            if ($e.Trim()) { Write-Output ('   stderr: ' + $e.Trim()) }
        }
    } catch {}
    exit 1
}

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
    # 类视图工程级扫描（第五十二轮）：ide.nproj 真跑，文件节点 ≥ 1
    if ($out -notmatch 'CVPROJ-N:[1-9]\d*') { Write-Output 'FAIL: class-view project scan found nothing'; exit 1 }
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

# 官方 intellisense 词库接入：词库前缀直呼 onComplete，候选必须出现
# kernel.txt 的常量（第五十一轮）
if ($out -notmatch 'INTEL-ENTRY:true') {
    Write-Output 'FAIL: intellisense word-lib entry missing from completion'
    exit 1
}

# 打印管线（第五十六轮）：页数测量链路通（无打印机/无 DC 时为 0 也算通）
if ($out -notmatch 'PRINT-PAGES:\d+') {
    Write-Output 'FAIL: print page-count pipeline did not run'
    exit 1
}

# 类视图：class → 成员函数层级 + 顶层变量（第五十一轮）
if ($out -notmatch 'CLASSVIEW:truetruetrue') {
    Write-Output 'FAIL: class view hierarchy scan broken'
    exit 1
}

# 模板面板：双击模板 → 新建文档落内容（第五十二轮）
if ($out -notmatch 'TPL-NEW:true')    { Write-Output 'FAIL: template pick did not create a document'; exit 1 }

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
# 编辑器字号：主编辑器 / 设置里的值 / 分栏对比编辑器 三处必须一致
if ($out -notmatch 'DOC-FONT:(\d+)/(\d+)') {
    Write-Output 'FAIL: editor font size was not reported'; exit 1
}
$fontEditor = [int]$Matches[1]
$fontCfg    = [int]$Matches[2]
if ($out -notmatch 'DOC-FONT-DIFF:(\d+)') {
    Write-Output 'FAIL: split-diff editor font size was not reported'; exit 1
}
$fontDiff = [int]$Matches[1]
if ($fontEditor -ne $fontCfg) { Write-Output "FAIL: editor font ($fontEditor) != settings font ($fontCfg)"; exit 1 }
if ($fontDiff -ne $fontCfg)  { Write-Output "FAIL: split-diff editor font ($fontDiff) != settings font ($fontCfg)"; exit 1 }

# every dock / editor must be visible once the window is shown
foreach ($v in @('VIS-LEFT','VIS-RIGHT','VIS-BOTTOM','VIS-DOC','VIS-ED','VIS-TOOLBOX')) {
    if ($out -notmatch ($v + ':true')) { Write-Output "FAIL: $v is not visible"; exit 1 }
}

# 「立即窗口」入口（第五十五轮表达式求值）：底部页签 + 菜单项都得在。
# 求值链路做出来了但入口没人找得到，等于白做 —— 页签默认排最后一位，
# 很容易被当成「不存在」，所以把两处入口都锁进冒烟自检。
if ($out -notmatch 'IMMEDIATE-TAB:OK')  { Write-Output 'FAIL: immediate window tab is missing'; exit 1 }
if ($out -notmatch 'IMMEDIATE-MENU:OK') { Write-Output 'FAIL: immediate window menu entry is missing'; exit 1 }

# 「监视窗/变量树」入口（第五十九轮续）：同上——页签 + 菜单都得在。
if ($out -notmatch 'WATCH-TAB:OK') { Write-Output 'FAIL: watch window tab is missing'; exit 1 }
if ($out -notmatch 'VARS-TAB:OK')  { Write-Output 'FAIL: variables tree tab is missing'; exit 1 }
if ($out -notmatch 'WATCH-MENU:OK') { Write-Output 'FAIL: watch window menu entry is missing'; exit 1 }

# 监视窗/变量树面板逻辑（第五十九轮续）：假求值器/假数据源走面板全链
if ($out -notmatch 'WATCH-ADD:OK')  { Write-Output 'FAIL: watch panel add/eval chain broken'; exit 1 }
if ($out -notmatch 'VARS-TREE:OK')  { Write-Output 'FAIL: variables tree rebuild broken'; exit 1 }
if ($out -notmatch 'VARS-LAZY:OK')  { Write-Output 'FAIL: variables tree lazy expand broken'; exit 1 }

# 窗体属性对话框（第五十九轮续）：DSG 头 border/bgcolor 往返 + 对话框全链
if ($out -notmatch 'FORM-DSG-RT:1') { Write-Output 'FAIL: form DSG border/bgcolor round-trip broken'; exit 1 }
if ($out -notmatch 'FORM-DLG:1')    { Write-Output 'FAIL: form properties dialog chain broken'; exit 1 }

# Tab 顺序（第五十九轮续）：真实消息点击 → add() 顺序重排 → toDSG 输出
if ($out -notmatch 'TAB-ORDER:1')   { Write-Output 'FAIL: tab order reorder chain broken'; exit 1 }

# 设计器拖拽落点（第五十六轮）：工具箱 → 画布的拖拽入口 + 坐标换算 + 避让。
#   DROP-WIRED:1   工具箱 onDrop 回调已接线
#   SPOT-FREE:OK   已有控件占着 (20,20) 时，新落点不与它重叠
#   COORD-RT:OK    设计坐标 → 客户区 → 设计坐标 往返一致（误差 ≤1 设计像素）
#   SURFACE-HIT:OK 窗体表面内命中、表面外不命中
if ($out -notmatch 'DROP-WIRED:1')    { Write-Output 'FAIL: designer toolbox drop target is not wired'; exit 1 }
if ($out -notmatch 'SPOT-FREE:OK')   { Write-Output 'FAIL: designer free-spot overlaps an existing control'; exit 1 }
if ($out -notmatch 'COORD-RT:OK')    { Write-Output 'FAIL: designer coordinate round-trip is inconsistent'; exit 1 }
if ($out -notmatch 'SURFACE-HIT:OK') { Write-Output 'FAIL: designer form-surface hit test is wrong'; exit 1 }
# DROP-E2E:1    端到端：模拟一次真实拖拽（按下→移动→松开）后，控件真的落到了画布上
if ($out -notmatch 'DROP-E2E:1')     { Write-Output 'FAIL: designer drag-and-drop did not create a control'; exit 1 }
# DROP-MSG:1     真实窗口消息路径的拖拽（PostMessage → 窗口过程 → 状态机）
if ($out -notmatch 'DROP-MSG:1')      { Write-Output 'FAIL: designer drag via real window messages failed'; exit 1 }
# 事件一览（第五十六轮）
#   EVENT-LIST:2/1   button 的事件清单有 2 项且含 oncommand
#   EVENTS-EDIT:11   edit 的事件表含 onChange + onFocusGot（对齐官方 edit.aardio:104）
#   EVENT-STUB-EV:1  指定事件（onSelChange）能生成处理函数桩
if ($out -notmatch 'EVENT-LIST:2/1')   { Write-Output 'FAIL: designer event list is wrong'; exit 1 }
if ($out -notmatch 'EVENTS-EDIT:11')   { Write-Output 'FAIL: designer event table does not match official win.ui'; exit 1 }
if ($out -notmatch 'EVENT-STUB-EV:1')  { Write-Output 'FAIL: designer did not generate the stub for a picked event'; exit 1 }
# 窗体级事件一览（第五十九轮续）
#   FORM-EVENTS:9/1  选中窗体 → 事件页列 9 个窗体级事件，说明列全有字
#   FORM-STUB:1      双击窗体事件 → 生成 winform.onclose = function(){...}（无控件段）
if ($out -notmatch 'FORM-EVENTS:9/1')  { Write-Output 'FAIL: designer form-level event list is wrong'; exit 1 }
if ($out -notmatch 'FORM-STUB:1')      { Write-Output 'FAIL: designer did not generate a form-level event stub'; exit 1 }
# SHOW-REDRAW:0  连续 show(true) 不再反复整块重画（第五十八轮修的闪烁）
if ($out -notmatch 'SHOW-REDRAW:0')   { Write-Output 'FAIL: board.show keeps repainting on every call (flicker)'; exit 1 }

# AI 面板（app.ai）：页签切过去要显示、切回来要隐藏；控件与会话都在
if ($out -notmatch 'AI-TAB:true/false') { Write-Output 'FAIL: AI panel tab visibility is wrong'; exit 1 }
if ($out -notmatch 'AI-CTRLS:1/1')      { Write-Output 'FAIL: AI panel controls are missing'; exit 1 }

# AI 面板发送链路（端到端，两段）：
#   ① 成功 —— base_url 指向本地 mock 服务器（tests/ai_mock_server.py）：
#      必须看到「用户消息进了日志」+「**AI 的回答进了日志**」+「会话多了一条
#      assistant 消息」+「状态回到空闲」。
#   ② 失败 —— base_url 指向 127.0.0.1:1（没人监听）→ 请求必然失败，
#      必须看到「用户消息进了日志」+「有一条 [错误]」，且用户消息仍留在会话里。
#   （凭据名不是隔离的测试名时整段跳过 —— 见 main.nodio 的 aiIsolated 判断）
if ($out -notmatch 'AI-SEND-SKIP:1') {
    if ($out -notmatch 'AI-SESSION-INIT:0')  { Write-Output 'FAIL: AI session is not empty on startup'; exit 1 }
    if ($out -notmatch 'AI-SEND-START:1')    { Write-Output 'FAIL: AI panel send could not be started'; exit 1 }
    # 成功回包（对本地 mock 服务器）：AI 的回答要进日志 + 进会话
    if ($out -notmatch 'AI-SEND-MOCK:1') {
        Write-Output 'FAIL: local AI mock server was not used'
        exit 1
    }
    if ($out -notmatch 'AI-SEND-USER:1')     { Write-Output 'FAIL: user message did not reach the AI log'; exit 1 }
    if ($out -notmatch 'AI-SEND-REPLY:1')    { Write-Output 'FAIL: AI reply did not reach the AI log'; exit 1 }
    if ($out -notmatch 'AI-SEND-BUSY:false') { Write-Output 'FAIL: AI panel stayed busy after the request finished'; exit 1 }
    if ($out -notmatch 'AI-SEND-MSGS:2')     { Write-Output 'FAIL: AI reply did not enter the AI session'; exit 1 }
    # 失败回包（对没人监听的端口）：要报错 + 用户消息仍留在会话里
    if ($out -notmatch 'AI-SEND-ERRUSER:1')  { Write-Output 'FAIL: failed request did not log the user message'; exit 1 }
    if ($out -notmatch 'AI-SEND-ERR:1')      { Write-Output 'FAIL: failed request did not report an error in the AI log'; exit 1 }
    if ($out -notmatch 'AI-SEND-ERRMSGS:3')  { Write-Output 'FAIL: failed request left the session in a wrong state'; exit 1 }
}

# 发布链路（端到端）：IDE 入口 → 发布器 → 独立 exe 跑起来。
# 菜单「工程 → 发布为独立 exe」与冒烟走同一条 publishRun 链路（capability-gap §1.46）：
#   PUB-START:1  发布子进程起来了（runner 单槽已被占用）
#   PUB-EXE:1    发布产物落盘
#   PUB-RUN:1    发布产物**真的**跑了自己的 payload（内容带每次不同的 token，
#                旧的产物混进来也骗不过 —— 断绝「拿上次结果假绿」）
if ($out -notmatch 'PUB-START:1') { Write-Output 'FAIL: publish was not started from the IDE'; exit 1 }
if ($out -notmatch 'PUB-EXE:1')   { Write-Output 'FAIL: published exe was not created'; exit 1 }
if ($out -notmatch 'PUB-RUN:1')   { Write-Output 'FAIL: published exe did not run its payload'; exit 1 }

# 最近文件（capability-gap §5.2「最近使用的文档」→ §1.47 落地）：
#   RECENT-AUTO:1     openFile 链路自动入队（钩子打开 hookTarget 后 cfg 里就有）
#   RECENT-TOP-OK:1   再 push 后置顶
#   RECENT-DUP-OK:1   同一路径反复 push 不翻倍（去重置顶）
#   RECENT-MENU-HIT:1 「文件」菜单重建后含 "1.  <路径>" 动态条目
if ($out -notmatch 'RECENT-AUTO:1')     { Write-Output 'FAIL: openFile did not record recent file'; exit 1 }
if ($out -notmatch 'RECENT-TOP-OK:1')   { Write-Output 'FAIL: recent file is not on top'; exit 1 }
if ($out -notmatch 'RECENT-DUP-OK:1')   { Write-Output 'FAIL: recent push duplicated entry'; exit 1 }
if ($out -notmatch 'RECENT-MENU-HIT:1') { Write-Output 'FAIL: file menu missing recent entry'; exit 1 }

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

}
finally {
    # ---- mock 收尾 ----
    # ① 杀掉自己起的那个（连进程树一起杀：mock 自己也可能派生子进程）
    if ($mockProc) {
        try { if (-not $mockProc.HasExited) { & taskkill /PID $mockProc.Id /T /F 2>$null | Out-Null } } catch {}
        try { [void]$mockProc.WaitForExit(3000) } catch {}
    }
    # ② 按命令行扫一遍，把**上一次被中断**（Ctrl+C / 崩了）留下的孤儿也回收掉。
    #    与 tests/run_vm_tests.ps1 同一套做法：按**脚本路径**匹配，不按 "python"。
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -match 'ai_mock_server\.py' } |
        ForEach-Object { & taskkill /PID $_.ProcessId /T /F 2>$null | Out-Null }
    Remove-Item $mockLog, $mockErr -Force -ErrorAction SilentlyContinue
}
