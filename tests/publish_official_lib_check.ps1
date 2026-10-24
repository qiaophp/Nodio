# 发布产物对官方标准库的支持（第二十六轮新增）
#
# 覆盖本轮修掉的两个真实缺陷：
#
#   1) 扩展名依赖外部配置文件
#      `nodio.json` 找不到时默认扩展名只剩 `.nodio`，官方标准库的唯一源码
#      扩展名 `.aardio` 消失 → 整个官方库静默失效。
#      发布产物是**自包含 exe**（官方库编译进 payload，外部没有 nodio.json），
#      所以这个缺陷在发布形态下是致命的。
#
#   2) 字节码序列化丢值
#      `Node::Bool` 的值存在 `num` 字段，但序列化端只为 `Node::Num` 写 double
#      → 布尔字面量在字节码往返后变成 false。
#      官方 `time/performance.aardio:4` 的 `raw.long(0,true)` 因此变成
#      `raw.long(0,false)`，发布产物一启动就在 ntdll 里访问冲突。
#      同时 `CollectStrings` 跳过空字符串，而解析器**故意**会产出空名
#      （缺位的命名空间段、`...` 可变参数标记），于是写出了 0xFFFFFFFF
#      这个越界哨兵 → 发布时报「字节码读取错误: 越界的符号名索引」。
#
# 用法: pwsh -File publish_official_lib_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$tmp = Join-Path $env:TEMP ("pubofflib_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

# ---- 1) 工程：import 官方库，用到结构体类、布尔字面量、辅助函数 ----
$main = @'
import win.ui;
import builtin.struct;
import string.format;

io.print("win.ui=", type(win.ui));
io.print("RECT=", type(::RECT));
io.print("HIWORD=", ::HIWORD(0x12345678));
'@
[IO.File]::WriteAllText((Join-Path $tmp 'main.aardio'), $main, (New-Object Text.UTF8Encoding($false)))
$proj = @'
<?xml version="1.0" encoding="utf-8"?>
<project>
	<file path="main.aardio" />
</project>
'@
[IO.File]::WriteAllText((Join-Path $tmp 'default.aproj'), $proj, (New-Object Text.UTF8Encoding($false)))

$outExe = Join-Path $tmp 'app.exe'

# ---- 2) 发布 ----
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_PUBLISH'] = Join-Path $tmp 'main.aardio'
$psi.EnvironmentVariables['NODIO_PUBOUT'] = $outExe
$psi.EnvironmentVariables['NODIO_LIBDIR'] = $LibDir
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$p = [System.Diagnostics.Process]::Start($psi)
$pubOut = $p.StandardOutput.ReadToEndAsync()
$pubErr = $p.StandardError.ReadToEndAsync()
$p.WaitForExit()
$pubText = $pubOut.Result + $pubErr.Result

$fail = 0
if (-not (Test-Path $outExe)) {
    "FAIL publish: output exe not created"
    "--- publisher output ---"
    $pubText
    exit 1
}
"PASS publish: output exe created"

# ---- 3) 在**隔离目录**运行（确定没有 nodio.json / 没有官方 lib 树）----
# 这是发布形态的真实约束：产物必须自包含。
$iso = Join-Path $tmp 'isolated'
New-Item -ItemType Directory -Path $iso | Out-Null
$isoExe = Join-Path $iso 'app.exe'
Copy-Item $outExe $isoExe -Force

if (Test-Path (Join-Path $iso 'nodio.json')) {
    "FAIL 前置条件: 隔离目录里存在 nodio.json，测试无效"
    exit 1
}

$run = New-Object System.Diagnostics.ProcessStartInfo
$run.FileName = $isoExe
$run.WorkingDirectory = $iso
$run.UseShellExecute = $false
$run.RedirectStandardOutput = $true
$run.RedirectStandardError = $true
$run.StandardOutputEncoding = [Text.Encoding]::UTF8
$run.StandardErrorEncoding = [Text.Encoding]::UTF8
# 刻意不设 NODIO_LIBDIR：产物不能依赖外部库树
$r = [System.Diagnostics.Process]::Start($run)
$runOut = $r.StandardOutput.ReadToEndAsync()
$runErr = $r.StandardError.ReadToEndAsync()
$r.WaitForExit()
$runText = $runOut.Result + $runErr.Result
$runText = ($runText -replace "`r", '').Trim()

$want = @(
    'win.ui=table',
    'RECT=class',
    'HIWORD=4660'
)
$lines = $runText -split "`n" | Where-Object { $_ -ne '' }
for ($i = 0; $i -lt $want.Count; $i++) {
    if ($lines.Count -gt $i -and $lines[$i].Trim() -eq $want[$i]) {
        "PASS $($want[$i])"
    } else {
        $got = if ($lines.Count -gt $i) { $lines[$i] } else { '<missing>' }
        "FAIL want=[$($want[$i])] got=[$got]"
        $fail++
    }
}

if ($fail -eq 0) {
    "== publish official lib OK =="
    exit 0
} else {
    "--- published output ---"
    $runText
    "--- publisher output ---"
    $pubText
    "== publish official lib FAILED: $fail =="
    exit $fail
}
