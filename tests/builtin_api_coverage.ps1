# 第二十三轮收口核对：官方内建成员清单 vs Nodio 运行期实际成员
# 用法: pwsh -NoProfile -File tests/builtin_api_coverage.ps1
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$Official = 'D:\Nodio\tests\official_builtin_members.json')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }
if (-not (Test-Path $Official)) { "OFFICIAL LIST NOT FOUND: $Official"; exit 2 }

# 抽取官方内建文件时的正则误匹配（枚举器/元方法/关键字），不属于「成员」
$noise = @('Next', 'Skip', 'nothing', 'first', 'each', 'if', 'elseif', 'else',
           'for', 'while', 'do', 'switch', 'case', 'break', 'continue',
           'function', 'return', 'var', 'global', 'local', 'type', 'parent',
           'owner', 'this', 'f', '_json', '_set', '_get', '_call', '_tostring',
           '_len', '_length', 'left', 'top', 'right', 'bottom', 'width', 'height')
# 嵌套表里的成员被抽取脚本误记成命名空间成员（逐条核对过官方源码）
$noiseByNs = @{
    'thread' = @('release', '_serialize', '_callableSerialize')
    'string' = @('table')
}

$officialJson = Get-Content $Official -Raw | ConvertFrom-Json
$officialMap = @{}
foreach ($p in $officialJson.PSObject.Properties) {
    # struct.* 是 RECT 的成员（不是命名空间），第二十二轮口径里已剔除
    if ($p.Name -eq 'struct') { continue }
    $keep = @()
    foreach ($m in $p.Value) {
        if ($noise -contains $m) { continue }
        if ($noiseByNs.ContainsKey($p.Name) -and $noiseByNs[$p.Name] -contains $m) { continue }
        $keep += $m
    }
    $officialMap[$p.Name] = $keep
}

# 运行期枚举 Nodio 的实际成员
$tmp = Join-Path $env:TEMP ("ns_dump_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$probe = Join-Path $tmp 'dump.aardio'
$code = @'
var nss = { "com", "io", "math", "raw", "string", "table", "thread" }
for (i = 1; #nss; i++) {
    var ns = nss[i]
    var tab = _G[ns]
    if (type(tab) != "table") { io.print(ns, '\t<not a table>'); continue }
    for (k, v in tab) { io.print(ns, '\t', k) }
}
'@
[IO.File]::WriteAllText($probe, $code, (New-Object Text.UTF8Encoding($false)))
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "`"$probe`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
$p = [System.Diagnostics.Process]::Start($psi)
$o = $p.StandardOutput.ReadToEndAsync()
$e = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(60000)) { $p.Kill(); $p.WaitForExit(3000) }
$out = (($o.Result + $e.Result) -replace "`r", '')
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

$actual = @{}
foreach ($line in ($out -split "`n")) {
    if ($line -notmatch "^(\S+)\t(.+)$") { continue }
    $ns = $Matches[1]; $m = $Matches[2].Trim()
    if (-not $actual.ContainsKey($ns)) { $actual[$ns] = @{} }
    $actual[$ns][$m] = $true
}

$total = 0; $covered = 0; $missing = @()
foreach ($ns in ($officialMap.Keys | Sort-Object)) {
    foreach ($m in $officialMap[$ns]) {
        $total++
        if ($actual.ContainsKey($ns) -and $actual[$ns].ContainsKey($m)) { $covered++ }
        else { $missing += "$ns.$m" }
    }
}
# 已知未闭合的缺口（见 docs/aardio-stdlib-delivery.md §10.4）：
#   需要「按 ITypeInfo 动态构造原始接口 vtable」的设施，官方库零调用点。
#   新增缺口会让本脚本失败 —— 它同时是回归闸门。
$known = @('com.ImplInterfaceFromTypelib')
$unexpected = @($missing | Where-Object { $known -notcontains $_ })
Write-Host ("官方内建成员 {0} 个，Nodio 覆盖 {1} 个（{2:P1}），缺 {3} 个" -f `
    $total, $covered, ($covered / $total), $missing.Count)
foreach ($m in $missing) {
    if ($known -contains $m) { Write-Host "  KNOWN-MISSING $m" }
    else { Write-Host "  MISSING $m" }
}
if ($unexpected.Count -gt 0) { exit 1 }
Write-Host "== builtin api coverage OK (known gaps: $($known -join ', ')) =="
exit 0
