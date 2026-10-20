# 官方 "=aardio" 预编译块：容器层解析与诊断回归
#
# 背景（docs/aardio-bytecode.md）：
#   · 官方 aardio.exe 内部称该格式为 "precompiled chunk"（官方报错文案
#     "bad header" / "in precompiled chunk"）。
#   · 容器文法已由 aardio.exe 加载器反编译还原，并经 tools/aardio_bytecode.py
#     字节级回归（重序列化与原文件逐字节相同）。
#   · 64 条 opcode 的语义尚未还原，因此官方预编译模块**仍不可执行**。
#     Nodio 现在能做的是：把块结构读出来，给出结构化诊断，而不是笼统的
#     「不支持的字节码格式」。
#
# 本脚本验证：
#   1) 官方唯一样本 lib\ide\aifix.aardio 的解析计数与 Python 参考实现一致
#      （97 个对象 / 8734 条指令 / 2173 个常量 / 最大嵌套 4 层）；
#   2) 结构损坏的 "=aardio" 块报「结构不完整」而不是笼统格式错误；
#   3) 非 "=aardio" 的编译期魔数仍走原来的「不支持的字节码格式」分支。
#
# 用法: powershell -File aardio_chunk_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$tmp = Join-Path $env:TEMP ("aardio_chunk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Run-Script([string]$code, [string]$libDir) {
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
    $psi.EnvironmentVariables['NODIO_LIBDIR'] = $libDir
    $psi.EnvironmentVariables['NODIO_OVERLAY_LIBDIR'] = $OverlayDir
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(3000) }
    return (($o.Result + $e.Result) -replace "`r", '').Trim()
}

$fail = 0
function Expect([bool]$cond, [string]$label, [string]$got) {
    if ($cond) { Write-Host "PASS $label" }
    else { $script:fail++; Write-Host "FAIL $label got=[$got]" }
}

# ---- 1) 官方唯一样本：结构化诊断 ----
if (-not (Test-Path (Join-Path $LibDir 'ide\aifix.aardio'))) {
    Write-Host "SKIP official sample not found under $LibDir"
} else {
    $out = Run-Script 'import ide.aifix;' $LibDir
    Expect ($out -like '*官方预编译模块 (=aardio)*') 'official sample diagnosed' $out
    Expect ($out -like '*暂不可执行*')              'states not executable'      $out
    Expect ($out -like '*97 个对象*')                'object count 97'            $out
    Expect ($out -like '*8734 条指令*')              'code word count 8734'       $out
    Expect ($out -like '*2173 个常量*')              'constant count 2173'        $out
    Expect ($out -like '*最大嵌套 4 层*')            'max depth 4'                $out
    Expect ($out -like '*尚未实现该字节码的解释器*') 'explains the missing part'  $out
    # 第十一轮新增：记录流按官方运行期布局解码后的结论（官方 aardio 给不出）
    Expect ($out -like '*记录流已按官方运行期布局解码*') 'record stream decoded'  $out
    Expect ($out -like '*8734 条记录*')                'record count 8734'        $out
    Expect ($out -like '*用到 43/61 种记录种类*')       '43 of 61 kinds used'      $out
    Expect ($out -like '*非法种类 0 条*')               'no illegal kinds'         $out
    Expect ($out -like '*631 个跳转目标中 631 个落在指令边界上*') 'all jumps resolve' $out
    # 第十三轮新增：官方校验器 FUN_004D4450 等价实现的结论
    Expect ($out -like '*结构校验：97 个对象 / 8734 条记录 / 0 处违规*') 'validate result reported' $out
}

# ---- 1b) `-disasm` CLI：反汇编官方预编译块（官方 aardio 完全没有这个能力）----
if (-not (Test-Path (Join-Path $LibDir 'ide\aifix.aardio'))) {
    Write-Host "SKIP official sample not found under $LibDir"
} else {
    $sample = Join-Path $LibDir 'ide\aifix.aardio'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "-disasm `"$sample`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.WaitForExit(3000) }
    $dis = (($o.Result + $e.Result) -replace "`r", '').Trim()
    Expect ($p.ExitCode -eq 0)                        'disasm exits 0'            $dis
    Expect ($dis -like '*官方预编译块 =aardio 反汇编*') 'disasm header'            $dis
    Expect ($dis -like '*对象: 97*')                   'disasm object count'       $dis
    Expect ($dis -like '*记录: 8734*')                 'disasm record count'       $dis
    Expect ($dis -like '*用到 43/61*')                 'disasm kind coverage'      $dis
    Expect ($dis -like '*非法种类: 0*')                 'disasm no illegal kinds'   $dis
    Expect ($dis -like '*跳转: 631（可解引用 631）*')   'disasm jumps resolvable'   $dis
    Expect ($dis -like '*LOAD_OUTER=2128*')             'disasm kind histogram'     $dis
    Expect ($dis -like '*GET_MEMBER2*')                 'disasm prints opcode names' $dis
    Expect ($dis -like '*rel18=21(next) -> 35*')        'disasm resolves jump target' $dis
    # ---- S-6：「未实现记录种类」一节（把「Nodio 还缺什么」量化到记录种类）----
    Expect ($dis -like '*== 未实现记录种类 ==*')        'disasm has unimplemented section' $dis
    Expect ($dis -like '*可执行 */61*')                 'disasm reports implemented count' $dis
    # 第二十轮：61 种记录全部落地 → 「未实现记录种类」一节为空
    Expect ($dis -like '*可执行 61/61*')              'disasm reports 61/61 executable' $dis
    # 第十四轮：0x00 / 0x18 已在记录 VM 里落地 → 不再出现在「未实现」一节
    Expect ($dis -notlike '*0x00 LOAD_OUTER*')          'disasm omits implemented kind 0x00' $dis
    Expect ($dis -notlike '*0x18 ADD*')                 'disasm omits implemented kind 0x18' $dis
    # 第十五轮：成员访问族落地 → 0x05 / 0x09 / 0x0B / 0x11 / 0x17 / 0x32 也不再出现
    Expect ($dis -notlike '*0x05 GET_MEMBER *')         'disasm omits implemented kind 0x05' $dis
    Expect ($dis -notlike '*0x09 GET_MEMBER3*')         'disasm omits implemented kind 0x09' $dis
    Expect ($dis -notlike '*0x0B INDEX*')               'disasm omits implemented kind 0x0B' $dis
    Expect ($dis -notlike '*0x11 SET_MEMBER3*')         'disasm omits implemented kind 0x11' $dis
    Expect ($dis -notlike '*0x17 METHOD_PREP*')         'disasm omits implemented kind 0x17' $dis
    Expect ($dis -notlike '*0x32 BUILD_ARRAY*')         'disasm omits implemented kind 0x32' $dis
    # 第十六轮：调用帧与闭包落地 → 0x03 / 0x0E / 0x0F / 0x29 / 0x2A / 0x2B / 0x34 也不再出现
    Expect ($dis -notlike '*0x0E STORE_UPVALUE*')       'disasm omits implemented kind 0x0E' $dis
    Expect ($dis -notlike '*0x0F LOAD_IF_SET*')         'disasm omits implemented kind 0x0F' $dis
    Expect ($dis -notlike '*0x29 TAILCALL*')             'disasm omits implemented kind 0x29' $dis
    Expect ($dis -notlike '*0x2A CALL*')                'disasm omits implemented kind 0x2A' $dis
    Expect ($dis -notlike '*0x2B CALL_NATIVE*')         'disasm omits implemented kind 0x2B' $dis
    Expect ($dis -notlike '*0x34 NEW_CLOSURE*')         'disasm omits implemented kind 0x34' $dis
    # 第十七轮：命名空间与导入落地 → 0x15 / 0x16 也不再出现
    Expect ($dis -notlike '*0x15 NEW_NATIVE*')          'disasm omits implemented kind 0x15' $dis
    Expect ($dis -notlike '*0x16 IMPORT_MEMBER*')       'disasm omits implemented kind 0x16' $dis
    # 第十八轮：拼接与循环落地 → 0x21 / 0x2D / 0x2E 也不再出现
    Expect ($dis -notlike '*0x21 CONCAT*')              'disasm omits implemented kind 0x21' $dis
    Expect ($dis -notlike '*0x2D ADDEQ*')               'disasm omits implemented kind 0x2D' $dis
    Expect ($dis -notlike '*0x2E SUBEQ*')               'disasm omits implemented kind 0x2E' $dis
    $unimpl = @($dis -split "`n" | Where-Object { $_ -match '^\s+0x[0-9A-F]{2}\s' })
    # 第二十轮：记录 VM 已落地 61 种，「未实现记录种类」一节为空
    Expect ($unimpl.Count -eq 0)                        'disasm lists no remaining kinds' $unimpl.Count
    Expect ($dis -like '*可执行 61/61*')                'disasm reports 61/61 executable' $dis
    # 第二十轮：动态调用族（0x2F / 0x30 / 0x31 / 0x3C）也不再出现在「未实现」一节
    Expect ($dis -notlike '*0x2F CALL_MULTI*A=*')       'disasm omits implemented kind 0x2F' $dis
    Expect ($dis -notlike '*0x30 CALL_VALUE*A=*')       'disasm omits implemented kind 0x30' $dis
    Expect ($dis -notlike '*0x31 CALL_NS*A=*')          'disasm omits implemented kind 0x31' $dis
    Expect ($dis -notlike '*0x3C CALL_N*A=*')           'disasm omits implemented kind 0x3C' $dis
    # 第十六轮：调用约定（实参窗口 / 返回槽）按官方反汇编写进种类表
    Expect ($dis -like '*CALL_MULTI=13*')               'disasm keeps call_multi histogram entry' $dis
    Expect ($dis -like '*CALL_VALUE=13*')               'disasm keeps call_value histogram entry' $dis
    Expect ($dis -like '*CALL_NS=12*')                  'disasm keeps call_ns histogram entry' $dis
    # ---- S-1b：「结构校验」一节（官方校验器 FUN_004D4450 的等价实现）----
    Expect ($dis -like '*== 结构校验 ==*')             'disasm has validate section' $dis
    Expect ($dis -like '*对象 97  记录 8734  违规 0*') 'disasm validate counts' $dis
    Expect ($dis -like '*结论：通过*')                 'disasm validate passed' $dis
    Expect ($dis -like '*FUN_004D4450*')               'disasm names the official validator' $dis
    # ---- 操作数寻址模型：9 位操作数的 bit8 指向常量表（第十三轮修正）----
    Expect ($dis -like '*常量下标(bit8=1)*')           'disasm marks constant operands' $dis
    Expect ($dis -like '*K=*')                         'disasm shows 18-bit operand' $dis
}

# ---- 2) 结构损坏的官方块 → 精确的「结构不完整」 ----
$badLib = Join-Path $tmp 'badlib'
New-Item -ItemType Directory -Path $badLib | Out-Null
$hdr = [byte[]](0x3d,0x61,0x61,0x72,0x64,0x69,0x6f,0x10,0x00,0x01,0x04,0x04,0x04,0x08,0x00)
$body = [byte[]](0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f)
[IO.File]::WriteAllBytes((Join-Path $badLib 'broken.aardio'), ($hdr + $body))
$out = Run-Script 'import broken;' $badLib
Expect ($out -like '*官方预编译模块 (=aardio)*') 'broken sample diagnosed' $out
Expect ($out -like '*结构不完整*')                'broken sample says broken'  $out

# ---- 3) 非 =aardio 的编译期魔数仍走原分支 ----
$otherLib = Join-Path $tmp 'otherlib'
New-Item -ItemType Directory -Path $otherLib | Out-Null
[IO.File]::WriteAllBytes((Join-Path $otherLib 'other.aardio'), ([byte[]](0x3d,0x62,0x79,0x74,0x65,0x63,0x6f,0x64,0x65,0x00,0x00,0x00)))
$out = Run-Script 'import other;' $otherLib
Expect ($out -like '*不支持的字节码格式*') 'non-aardio magic unchanged' $out

& 'C:\Users\QQ\.minimax\bin\mavis-trash.cmd' -- $tmp | Out-Null
if ($fail -gt 0) { "== aardio chunk FAILED ($fail) =="; exit 1 }
"== aardio chunk OK =="
exit 0
