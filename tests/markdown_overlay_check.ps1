# string.markdown x64 补版回归 —— 锁住「按官方同名路径补 Nodio 自己实现」这一批
#
# 官方 lib/string/markdown/_.aardio 把 32 位 sundown.dll 用 $"..." 内嵌进源码
# 再内存加载，x64 进程装不下该 PE（实测「无法加载 DLL: (内嵌的内存映像)」）。
# 官方**没有随库发布 sundown 源码**（.res/README.TXT 声明该组件是作者魔改过的
# sundown，许可条款也禁止脱离 aardio 单独分发），因此无法字节级复刻。
#
# Nodio 侧按官方同名同路径提供自己的版本（overlay 目录优先于官方库）：
#   lib/string/markdown/_.aardio           官方逐字副本，只换 DLL 来源
#   lib/string/markdown/.res/markdown64.c  Nodio 自写实现（源码入库，可复现）
#   lib/string/markdown/.res/markdown64.dll 上面源码用 MSVC 编出的 x64 DLL
#
# 导出符号与官方 sundown.dll 同名同签名同调用约定(cdecl)，共 17 个：
#   bufcstr/bufdata/bufgrow/bufnew/bufprefix/bufput/bufputc/bufrelease/bufsize
#   sd_markdown_free/sd_markdown_new/sd_markdown_render/sd_version
#   sdhtml_is_tag/sdhtml_renderer/sdhtml_smartypants/sdhtml_toc_renderer
#
# 下面的期望值全部来自**官方 32 位 sundown 的实测**（x86 构建走官方库，
# x64 构建走 Nodio overlay），两个架构必须给出完全一样的结果。
#
# 注意：Windows 下 io.print 会把串里的 LF 转成 CRLF（CRT 文本模式），
# 所以本回归在 aardio 侧用十六进制回传字符串字节，避免行尾被改写。
#
# x86 构建不命中 overlay（engine/import.cpp 的 GetOverlayLibDir：32 位构建
# 直接用 aardio 官方 32 位标准库），所以本回归在两个架构上都能跑。
#
# 用法: powershell -File markdown_overlay_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$repoRoot   = Split-Path $PSScriptRoot -Parent
$overlayDir = Join-Path $repoRoot 'lib\string\markdown'
# overlay 模块已迁移为 .nodio 扩展名（.aardio 保留兜底，旧检出兼容）
$overlayMod = if (Test-Path (Join-Path $overlayDir '_.nodio')) { Join-Path $overlayDir '_.nodio' } else { Join-Path $overlayDir '_.aardio' }
$overlaySrc = Join-Path $overlayDir '.res\markdown64.c'
$overlayDll = Join-Path $overlayDir '.res\markdown64.dll'

function Get-PEMachine([string]$path) {
    $pe = [IO.File]::ReadAllBytes($path)
    $off = [BitConverter]::ToInt32($pe, 0x3C)
    return [BitConverter]::ToUInt16($pe, $off + 4)
}

$isX64 = ((Get-PEMachine $Exe) -eq 0x8664)

function Run-Aardio([string]$code) {
    $tmp = Join-Path $env:TEMP ("md_ovl_" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".aardio")
    [IO.File]::WriteAllText($tmp, $code, (New-Object Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "`"$tmp`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.EnvironmentVariables['NODIO_PAUSE'] = '0'
    $psi.EnvironmentVariables['NODIO_CONSOLE'] = '1'
    $proc = [Diagnostics.Process]::Start($psi)
    $o = $proc.StandardOutput.ReadToEnd()
    $e = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    Remove-Item $tmp -ErrorAction SilentlyContinue
    return @{ out = $o; err = $e; code = $proc.ExitCode }
}

# 十六进制 -> UTF-8 字符串（aardio 侧按字节回传，规避 CRLF 转换）
function From-Hex([string]$hex) {
    if (-not $hex) { return '' }
    $bytes = New-Object byte[] ($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return [Text.Encoding]::UTF8.GetString($bytes)
}

$fail = 0
function Check([string]$name, [string]$expect, [string]$actual) {
    if ($actual -eq $expect) { "  ok   $name" }
    else {
        "  FAIL $name"
        "       got = $($actual.Replace("`n", '\n'))"
        "       exp = $($expect.Replace("`n", '\n'))"
        $script:fail++
    }
}

# 期望值：按行拼接，行尾统一 LF（官方 sundown 输出用 LF）
$LF = [char]10
function E([string[]]$lines) { return (($lines -join $LF) + $LF) }

# ---- 1. overlay 三个文件必须在库里（源码入库，保证可复现）----
foreach ($f in @($overlayMod, $overlaySrc, $overlayDll)) {
    if (Test-Path $f) { "  ok   overlay 文件存在 $(Split-Path $f -Leaf)" }
    else { "  FAIL overlay 文件缺失 $f"; $script:fail++ }
}
if (Test-Path $overlayDll) {
    Check 'overlay DLL 架构(0x8664)' '34404' ([string](Get-PEMachine $overlayDll))
}

# ---- 2. DLL 导出符号与官方 sundown.dll 一致（扫描导出名字符串）----
if (Test-Path $overlayDll) {
    $dllText  = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($overlayDll))
    $exports = @('bufcstr', 'bufdata', 'bufgrow', 'bufnew', 'bufprefix', 'bufput',
                 'bufputc', 'bufrelease', 'bufsize', 'sd_markdown_free',
                 'sd_markdown_new', 'sd_markdown_render', 'sd_version',
                 'sdhtml_is_tag', 'sdhtml_renderer', 'sdhtml_smartypants',
                 'sdhtml_toc_renderer')
    $missing = @()
    foreach ($e in $exports) {
        if ($dllText -notmatch [regex]::Escape($e)) { $missing += $e }
    }
    if ($missing.Count -eq 0) { "  ok   DLL 导出 17 个符号齐全" }
    else { "  FAIL DLL 缺少导出: $($missing -join ',')"; $script:fail++ }
}

# ---- 3. 渲染黄金样本：期望值来自官方 32 位 sundown 实测 ----
# 用例表以 JSON 内嵌（src = Markdown 原文, exp = 官方渲染出的 HTML）。
$casesJson = @'
[
  {
    "name": "h1",
    "src": "# Header 1",
    "exp": "<h1>Header 1</h1>\n"
  },
  {
    "name": "h2",
    "src": "## Header 2 ##",
    "exp": "<h2>Header 2</h2>\n"
  },
  {
    "name": "h3",
    "src": "### Header 3",
    "exp": "<h3>Header 3</h3>\n"
  },
  {
    "name": "h_nospace",
    "src": "#NoSpace",
    "exp": "<p>#NoSpace</p>\n"
  },
  {
    "name": "h6",
    "src": "###### H6",
    "exp": "<h6>H6</h6>\n"
  },
  {
    "name": "setext",
    "src": "Title\n=====",
    "exp": "<h1>Title</h1>\n"
  },
  {
    "name": "setext2",
    "src": "Title\n-----",
    "exp": "<h2>Title</h2>\n"
  },
  {
    "name": "para",
    "src": "Just a paragraph.",
    "exp": "<p>Just a paragraph.</p>\n"
  },
  {
    "name": "para2",
    "src": "Line one\nLine two",
    "exp": "<p>Line one\nLine two</p>\n"
  },
  {
    "name": "em",
    "src": "*italic* text",
    "exp": "<p><em>italic</em> text</p>\n"
  },
  {
    "name": "em_us",
    "src": "_italic_ text",
    "exp": "<p><em>italic</em> text</p>\n"
  },
  {
    "name": "strong",
    "src": "**bold** text",
    "exp": "<p><strong>bold</strong> text</p>\n"
  },
  {
    "name": "strong_us",
    "src": "__bold** text",
    "exp": "<p>__bold** text</p>\n"
  },
  {
    "name": "strong_em",
    "src": "***both***",
    "exp": "<p><strong><em>both</em></strong></p>\n"
  },
  {
    "name": "code_span",
    "src": "use `code` here",
    "exp": "<p>use <code>code</code> here</p>\n"
  },
  {
    "name": "code_span2",
    "src": "``a ` b``",
    "exp": "<p><code>a ` b</code></p>\n"
  },
  {
    "name": "fence",
    "src": "```\ncode line\n```",
    "exp": "<pre><code>code line\n</code></pre>\n"
  },
  {
    "name": "fence_lang",
    "src": "```js\nvar a = 1;\n```",
    "exp": "<pre><code class=\"js language-js\">var a = 1;\n</code></pre>\n"
  },
  {
    "name": "fence_tilde",
    "src": "~~~\ntilde code\n~~~",
    "exp": "<pre><code>tilde code\n</code></pre>\n"
  },
  {
    "name": "indent_code",
    "src": "    indented code\n    line2",
    "exp": "<pre><code>indented code\nline2\n</code></pre>\n"
  },
  {
    "name": "quote",
    "src": "> quoted text",
    "exp": "<blockquote>\n<p>quoted text</p>\n</blockquote>\n"
  },
  {
    "name": "quote_multi",
    "src": "> line1\n> line2",
    "exp": "<blockquote>\n<p>line1\nline2</p>\n</blockquote>\n"
  },
  {
    "name": "ul",
    "src": "- one\n- two",
    "exp": "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n"
  },
  {
    "name": "ul_star",
    "src": "* one\n* two",
    "exp": "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n"
  },
  {
    "name": "ol",
    "src": "1. one\n2. two",
    "exp": "<ol>\n<li>one</li>\n<li>two</li>\n</ol>\n"
  },
  {
    "name": "ul_nested",
    "src": "- a\n    - b\n- c",
    "exp": "<ul>\n<li>a\n\n<ul>\n<li>b</li>\n</ul></li>\n<li>c</li>\n</ul>\n"
  },
  {
    "name": "ol_nested",
    "src": "1. a\n    1. b\n2. c",
    "exp": "<ol>\n<li>a\n\n<ol>\n<li>b</li>\n</ol></li>\n<li>c</li>\n</ol>\n"
  },
  {
    "name": "task",
    "src": "- [ ] todo\n- [x] done",
    "exp": "<ul>\n<li><input type=\"checkbox\" disabled> todo</li>\n<li><input type=\"checkbox\" checked disabled> done</li>\n</ul>\n"
  },
  {
    "name": "hr",
    "src": "---",
    "exp": "<hr>\n"
  },
  {
    "name": "hr_star",
    "src": "***",
    "exp": "<hr>\n"
  },
  {
    "name": "hr_under",
    "src": "___",
    "exp": "<hr>\n"
  },
  {
    "name": "link",
    "src": "[text](http://example.com)",
    "exp": "<p><a href=\"http://example.com\">text</a></p>\n"
  },
  {
    "name": "link_title",
    "src": "[text](http://example.com \"Title\")",
    "exp": "<p><a href=\"http://example.com\" title=\"Title\">text</a></p>\n"
  },
  {
    "name": "link_ref",
    "src": "[text][ref]\n\n[ref]: http://example.com",
    "exp": "<p><a href=\"http://example.com\">text</a></p>\n"
  },
  {
    "name": "img",
    "src": "![alt](img.png)",
    "exp": "<p><img src=\"img.png\" alt=\"alt\"></p>\n"
  },
  {
    "name": "img_title",
    "src": "![alt](img.png \"T\")",
    "exp": "<p><img src=\"img.png\" alt=\"alt\" title=\"T\"></p>\n"
  },
  {
    "name": "autolink",
    "src": "<http://example.com>",
    "exp": "<p><a href=\"http://example.com\">http://example.com</a></p>\n"
  },
  {
    "name": "autolink_email",
    "src": "<a@b.com>",
    "exp": "<p><a href=\"mailto:a@b.com\">a@b.com</a></p>\n"
  },
  {
    "name": "bare_url",
    "src": "see http://example.com now",
    "exp": "<p>see <a href=\"http://example.com\">http://example.com</a> now</p>\n"
  },
  {
    "name": "table",
    "src": "| a | b |\n|---|---|\n| 1 | 2 |",
    "exp": "<table><thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead><tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n</tr>\n</tbody></table>\n"
  },
  {
    "name": "table_align",
    "src": "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |",
    "exp": "<table><thead>\n<tr>\n<th style=\"text-align:left;\">a</th>\n<th style=\"text-align:center;\">b</th>\n<th style=\"text-align:right;\">c</th>\n</tr>\n</thead><tbody>\n<tr>\n<td style=\"text-align:left;\">1</td>\n<td style=\"text-align:center;\">2</td>\n<td style=\"text-align:right;\">3</td>\n</tr>\n</tbody></table>\n"
  },
  {
    "name": "strike",
    "src": "~~gone~~",
    "exp": "<p><del>gone</del></p>\n"
  },
  {
    "name": "sup",
    "src": "x^2^",
    "exp": "<p>x^2^</p>\n"
  },
  {
    "name": "raw_html",
    "src": "<div>raw</div>",
    "exp": "<div>raw</div>\n"
  },
  {
    "name": "raw_inline",
    "src": "a <b>bold</b> c",
    "exp": "<p>a <b>bold</b> c</p>\n"
  },
  {
    "name": "escape",
    "src": "\\*not em\\*",
    "exp": "<p>*not em*</p>\n"
  },
  {
    "name": "entity",
    "src": "& &copy;",
    "exp": "<p>&amp; &copy;</p>\n"
  },
  {
    "name": "empty",
    "src": "",
    "exp": ""
  },
  {
    "name": "blank",
    "src": "\n\n",
    "exp": ""
  },
  {
    "name": "frontmatter",
    "src": "---\ntitle: x\n---\n\nbody text",
    "exp": "<p>body text</p>\n"
  },
  {
    "name": "hardbreak",
    "src": "line one  \nline two",
    "exp": "<p>line one<br>\nline two</p>\n"
  },
  {
    "name": "amp",
    "src": "a & b",
    "exp": "<p>a &amp; b</p>\n"
  },
  {
    "name": "lt",
    "src": "a < b",
    "exp": "<p>a &lt; b</p>\n"
  },
  {
    "name": "quote_in_list",
    "src": "- item\n\n    > quoted",
    "exp": "<ul>\n<li><p>item</p>\n\n<blockquote>\n<p>quoted</p>\n</blockquote></li>\n</ul>\n"
  },
  {
    "name": "task_loose",
    "src": "- [ ] a\n\n- [x] b",
    "exp": "<ul>\n<li><p><input type=\"checkbox\" disabled> a</p></li>\n<li><p><input type=\"checkbox\" checked disabled> b</p></li>\n</ul>\n"
  },
  {
    "name": "task_nested",
    "src": "- [ ] a\n    - [x] b",
    "exp": "<ul>\n<li><input type=\"checkbox\" disabled> a\n\n<ul>\n<li><input type=\"checkbox\" checked disabled> b</li>\n</ul></li>\n</ul>\n"
  },
  {
    "name": "task_ol",
    "src": "1. [ ] a\n2. [x] b",
    "exp": "<ol>\n<li><input type=\"checkbox\" disabled> a</li>\n<li><input type=\"checkbox\" checked disabled> b</li>\n</ol>\n"
  },
  {
    "name": "task_blank",
    "src": "- [ ]",
    "exp": "<ul>\n<li>[ ]</li>\n</ul>\n"
  },
  {
    "name": "quote_char",
    "src": "it's \"q\" & <b>",
    "exp": "<p>it&#39;s &quot;q&quot; &amp; <b></p>\n"
  },
  {
    "name": "bslash",
    "src": "a\\\\b",
    "exp": "<p>a\\b</p>\n"
  },
  {
    "name": "html_tag_in_text",
    "src": "2 < 3 and 4 > 5",
    "exp": "<p>2 &lt; 3 and 4 &gt; 5</p>\n"
  }
]
'@
$cases = $casesJson | ConvertFrom-Json

# 把 PowerShell 字符串转成 aardio 单引号字面量（单引号串里 \n \r 是真转义）
function AaLit([string]$s) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("'")
    foreach ($ch in $s.ToCharArray()) {
        if     ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($ch -eq "'") { [void]$sb.Append("\'") }
        elseif ($ch -eq "`n") { [void]$sb.Append('\n') }
        elseif ($ch -eq "`r") { [void]$sb.Append('\r') }
        elseif ($ch -eq "`t") { [void]$sb.Append('\t') }
        else   { [void]$sb.Append($ch) }
    }
    [void]$sb.Append("'")
    return $sb.ToString()
}

$probe = New-Object Text.StringBuilder
[void]$probe.AppendLine("import string.markdown;")
[void]$probe.AppendLine("function hx(s){ var o=''; for(i=1; #s; i++){ o = o ++ string.format('%02x', s[i]); } return o; }")
[void]$probe.AppendLine("var nok = 0, nbad = 0;")
[void]$probe.AppendLine("function t(name, src, exp){")
[void]$probe.AppendLine("    var r = string.markdown(src);")
[void]$probe.AppendLine("    if(r == exp){ nok = nok + 1; }")
[void]$probe.AppendLine("    else {")
[void]$probe.AppendLine("        nbad = nbad + 1;")
[void]$probe.AppendLine("        io.print('FAIL ' ++ name);")
[void]$probe.AppendLine("        io.print('  got=' ++ hx(r));")
[void]$probe.AppendLine("        io.print('  exp=' ++ hx(exp));")
[void]$probe.AppendLine("    }")
[void]$probe.AppendLine("}")
foreach ($c in $cases) {
    [void]$probe.AppendLine(("t({0},{1},{2});" -f (AaLit $c.name), (AaLit $c.src), (AaLit $c.exp)))
}
[void]$probe.AppendLine("io.print('RESULT ok=' ++ nok ++ ' bad=' ++ nbad);")

$r = Run-Aardio $probe.ToString()
if ($r.err) { "  NOTE stderr: $($r.err.Trim())" }
$resultLine = (($r.out -split "`n") | Where-Object { $_ -match '^RESULT ' }) -join ''
if ($resultLine -match 'ok=(\d+) bad=(\d+)') {
    $nok = [int]$Matches[1]; $nbad = [int]$Matches[2]
    if ($nbad -eq 0 -and $nok -eq $cases.Count) { "  ok   渲染用例 $nok/$($cases.Count) 全部与官方一致" }
    else { "  FAIL 渲染用例 ok=$nok bad=$nbad / 共 $($cases.Count)"; $script:fail++ }
}
else {
    "  FAIL 未拿到用例统计行: $($r.out.Trim())"; $script:fail++
}
foreach ($ln in ($r.out -split "`n")) {
    if ($ln -match '^FAIL ' -or $ln -match '^\s+got=' -or $ln -match '^\s+exp=') { "  $ln" }
}

# ---- 4. API 面：与官方一致（类型、实例方法、渲染标志、任务列表开关）----
$apiProbe = @'
import string.markdown;
function hx(s){ var o=''; for(i=1; #s; i++){ o = o ++ string.format('%02x', s[i]); } return o; }
io.print("T_CLASS|" ++ hx(type(string.markdown)));
io.print("T_BUF|" ++ hx(type(string.markdown.buffer)));
io.print("T_OPT|" ++ hx(type(string.markdown.html_renderopt)));
io.print("T_CB|" ++ hx(type(string.markdown.sd_callbacks)));
io.print("T_PROTO|" ++ hx(type(string.markdown.sd_callbacks_proto)));
io.print("T_CODEBLOCK|" ++ hx(type(string.markdown.codeBlock)));
var md = string.markdown(8,16,0);
io.print("T_INST|" ++ hx(type(md)));
io.print("RENDER|" ++ hx(md.render("# H")));
io.print("RENDER2|" ++ hx(md.render("- [x] a")));
md.setTaskCheckboxEnabled(true);
io.print("TASK_ON|" ++ hx(md.render("- [x] a")));
md.setTaskCheckboxEnabled(false);
io.print("TASK_OFF|" ++ hx(md.render("- [x] a")));
md.setRenderFlags(0x80, false);
io.print("HARDWRAP|" ++ hx(md.render('a\nb')));
md.setRenderFlags(0x80, true);
io.print("NOWRAP|" ++ hx(md.render('a\nb')));
io.print("CODE|" ++ hx(md.renderCode("var a=1;","js")));
io.print("DIRECT|" ++ hx(string.markdown("# D")));
md.free();
'@
$r2 = Run-Aardio $apiProbe
$api = @{}
foreach ($ln in ($r2.out -split "`r?`n")) {
    if ($ln -match '^([A-Z_0-9]+)\|([0-9a-f]*)\s*$') { $api[$Matches[1]] = From-Hex $Matches[2] }
}
$apiExpected = [ordered]@{
    'T_CLASS'     = 'class'
    'T_BUF'       = 'class'
    'T_OPT'       = 'class'
    'T_CB'        = 'class'
    'T_PROTO'     = 'table'
    'T_CODEBLOCK' = 'class'
    'T_INST'      = 'table'
    'RENDER'      = E @('<h1>H</h1>')
    'RENDER2'     = E @('<ul>', '<li><input type="checkbox" checked disabled> a</li>', '</ul>')
    'TASK_ON'     = E @('<ul>', '<li><label><input type="checkbox" checked> a</label></li>', '</ul>')
    'TASK_OFF'    = E @('<ul>', '<li><input type="checkbox" checked disabled> a</li>', '</ul>')
    'HARDWRAP'    = E @('<p>a<br>', 'b</p>')
    'NOWRAP'      = E @('<p>a', 'b</p>')
    'CODE'        = E @('<p><code>js', 'var a=1;', '</code></p>')
    'DIRECT'      = E @('<h1>D</h1>')
}
foreach ($k in $apiExpected.Keys) {
    $got = if ($api.ContainsKey($k)) { $api[$k] } else { '<缺失>' }
    Check $k $apiExpected[$k] $got
}

# ---- 5. 模块解析路径：x64 必须命中 Nodio overlay，x86 必须走官方库 ----
$r4 = Run-Aardio 'import string.markdown; io.print("LIBPATH " + io.libpath("string.markdown"));'
$libPath = ''
foreach ($ln in ($r4.out -split "`r?`n")) {
    if ($ln -match '^LIBPATH\s+(.*?)\s*$') { $libPath = $Matches[1] }
}
$expectOverlay = $overlayMod
if ($isX64) {
    Check 'x64 命中 Nodio overlay' $expectOverlay $libPath
}
else {
    if ($libPath -and ($libPath -ne $expectOverlay)) { "  ok   x86 走官方库 ($libPath)" }
    else { "  FAIL x86 未走官方库: '$libPath'"; $script:fail++ }
}

if ($fail -eq 0) { "MARKDOWN-OVERLAY-OK"; exit 0 }
"MARKDOWN-OVERLAY-FAIL ($fail)"; exit 1
