# 官方内建 API 契约回归（第二十三轮补齐的那批）
#
# 覆盖官方 lib/builtin/*.aardio 的 intellisense 声明里、而 Nodio 此前缺失的
# 引擎原生函数，以及本轮修正的运算符/多返回值语义：
#
#   string.split / string.splitLiteral   —— 官方 4 种形式（1 参逐字符 / 2 参单字节集合
#                                           / '<整词>' / maxCount）
#   io.lines / io.libpath                 —— 官方库有真实调用点（lib/autos、lib/py2 等）
#   math.fmod / math.lerp / table.setAttr —— 官方 builtin/math.aardio、table.aardio
#   com.NewObject / NewControl / SafeArray / IsIUnknown / IsMember / GetPointer
#                                           —— 官方只在 aardio.exe 里（引擎原生）
#   `%` 运算符                            —— 官方是向下取整（floor）语义，符号跟除数
#
# 官方契约来源：
#   D:\aardio\lib\builtin\string.aardio:389-391
#   D:\aardio\lib\builtin\math.aardio:97,107
#   D:\aardio\lib\builtin\table.aardio:508-511
#   D:\aardio\lib\builtin\com.aardio:149-161,209-215
#   D:\aardio\lib\com\activeX.aardio:159-180   （NewObject 三值返回形状）
#   D:\aardio\docs\language-reference\operator\arithmetic.md:46  （% 的 floor 语义）
#
# 用法: powershell -File builtin_api_check.ps1 [-Exe <nodio.exe>]
param($Exe = 'D:\Nodio\build-x64\Release\nodio.exe',
      [string]$LibDir = 'D:\aardio\lib',
      [string]$OverlayDir = 'D:\Nodio\lib')
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (-not (Test-Path $Exe)) { "EXE NOT FOUND: $Exe"; exit 2 }

$tmp = Join-Path $env:TEMP ("builtin_api_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
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
    if (-not $p.WaitForExit(60000)) { $p.Kill(); $p.WaitForExit(3000) }
    return (($o.Result + $e.Result) -replace "`r", '').Trim()
}

$fail = 0
function Expect-Section([string]$code, [string]$label, [string]$okMark) {
    $out = Run-Script $code
    $hasFail = ($out -match '(?m)^\s*FAIL ')
    if ($hasFail -or ($out -notlike "*$okMark*")) {
        $script:fail++
        Write-Host "FAIL $label"
        ($out -split "`n") | ForEach-Object { if ($_ -ne '') { Write-Host "    $_" } }
    } else {
        Write-Host "PASS $label"
    }
}

# ================================================================
# 1) string.split / string.splitLiteral
#    注意：aardio 里 **双引号 = 原样串**（不处理转义），**单引号 = 转义串**（C 风格转义）
# ================================================================
$splitCode = @'
var fail = 0;
function chk(name, got, want) {
  var ok = true;
  if (type(got) != type(want)) ok = false;
  else if (type(got) == "table") {
    if (#got != #want) ok = false;
    else {
      for (i = 1; #got; i++) {
        if (got[i] !== want[i]) { ok = false; break; }
      }
    }
  } else if (got !== want) ok = false;
  if (ok) { io.print("PASS ", name); }
  else {
    fail++;
    io.print("FAIL ", name);
    if (type(got) == "table") {
      io.print("   got:");  for (i = 1; #got; i++) io.print("     [", i, "]=", got[i]);
      io.print("   want:"); for (i = 1; #want; i++) io.print("     [", i, "]=", want[i]);
    } else { io.print("   got=", got, " want=", want); }
  }
}
// 1 参：按 UTF-8 逐字符拆分
chk("split/char", string.split("abc"), ["a","b","c"]);
chk("split/char-utf8", string.split("中a"), ["中","a"]);
chk("split/empty", string.split(""), []);
// 2 参：多个单字节分隔符（集合语义）
chk("split/byteset", string.split("a,b.c", ",."), ["a","b","c"]);
chk("split/byteset-slash", string.split('a/b\\c', '/\\'), ["a","b","c"]);
chk("split/single", string.split("a,b,c", ","), ["a","b","c"]);
chk("split/tab-nl", string.split('a\nb\tc', '\n\t'), ["a","b","c"]);
// '<整词>' 形式
chk("split/literal", string.split("a.b.c", "<.>"), ["a","b","c"]);
chk("split/literal-multi", string.split("a::b", "<::>"), ["a","b"]);
chk("split/literal-crlf", string.split('a\r\n\r\nb', '<\r\n\r\n>'), ["a","b"]);
chk("split/literal-raw-bs", string.split("a\\b", "<\\>"), ["a","b"]);
// maxCount：结果元素数上限，剩余并入最后一项
chk("split/maxcount", string.split("a b c d", " ", 2), ["a","b c d"]);
chk("split/maxcount1", string.split("a b c", " ", 1), ["a b c"]);
chk("split/maxcount3", string.split("a b c d", " ", 3), ["a","b","c d"]);
chk("split/maxcount0", string.split("a b c", " ", 0), ["a","b","c"]);
// splitLiteral：整词分隔
chk("splitLiteral", string.splitLiteral("a.b.c", "."), ["a","b","c"]);
chk("splitLiteral/multi", string.splitLiteral("a::b", "::"), ["a","b"]);
chk("splitLiteral/empty", string.splitLiteral("", "."), []);
chk("splitLiteral/maxcount", string.splitLiteral("a b c d", " ", 2), ["a","b c d"]);
chk("splitLiteral/keepsep", string.splitLiteral("a..b", "."), ["a","","b"]);
if (fail) { io.print("== SPLIT CHECK FAILED: ", fail); return 1; }
io.print("== string.split contract OK ==");
'@
Expect-Section $splitCode 'string.split / splitLiteral' 'string.split contract OK'

# ================================================================
# 2) io.lines / io.libpath
# ================================================================
$ioCode = @'
var fail = 0;
function chk(name, ok, detail) {
  if (ok) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " ", detail); }
}
var tmp = io.appData("/nodio_test_lines.txt");
io.writeFile(tmp, 'alpha\r\nbeta\r\ngamma');
var n = 0;
var last = "";
for (line in io.lines(tmp)) { n++; last = line; }
chk("lines/count", n == 3, "n=" + n);
chk("lines/no-cr", last == "gamma", "last=" + last);
var f = io.file(tmp);
var m = 0;
for (line in io.lines(f)) { m++; }
chk("lines/fileobj", m == 3, "m=" + m);
var tmp2 = io.appData("/nodio_test_lines2.txt");
io.writeFile(tmp2, 'a\r\nb\r\n');
var joined = "";
for (p in io.lines(tmp2)) { joined = joined + "[" + p + "]"; }
chk("lines/join", joined == "[a][b]", joined);
var tmp3 = io.appData("/nodio_test_lines3.txt");
io.writeFile(tmp3, '');
var z = 0;
for (line in io.lines(tmp3)) { z++; }
chk("lines/empty", z == 0, "z=" + z);
var p, d = io.libpath("py2");
chk("libpath/py2-path", p != null && string.indexOf(p, "py2") > 0, "" + p);
chk("libpath/py2-dir", d != null && string.indexOf(d, "py2") > 0, "" + d);
var p2, d2 = io.libpath("builtin.math");
chk("libpath/builtin-math", p2 != null && string.indexOf(p2, "math") > 0, "" + p2);
var p3 = io.libpath("no.such.module.xyz");
chk("libpath/missing", p3 == null, "" + p3);
if (fail) { io.print("== IO CHECK FAILED: ", fail); return 1; }
io.print("== io.lines / io.libpath OK ==");
'@
Expect-Section $ioCode 'io.lines / io.libpath' 'io.lines / io.libpath OK'

# ================================================================
# 3) math.fmod / math.lerp / table.setAttr
#    官方 % 是 floor 语义（符号跟除数），math.fmod 是 C fmod（符号跟被除数）
# ================================================================
$mathCode = @'
var fail = 0;
function chk(name, got, want) {
  var ok = (got === want);
  if (ok) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " got=", got, " want=", want); }
}
// math.fmod：C 的 fmod，余数符号与被除数 x 相同
chk("fmod/pos", math.fmod(7.5, 2), 1.5);
chk("fmod/neg-x", math.fmod(-7.5, 2), -1.5);
chk("fmod/neg-y", math.fmod(7.5, -2), 1.5);
chk("fmod/neg-both", math.fmod(-7.5, -2), -1.5);
chk("fmod/zero", math.fmod(5, 0), 0);
// 官方 % 是向下取整：余数符号跟除数 y
chk("mod/pos", 7 % 2, 1);
chk("mod/neg-x", -7 % 2, 1);
chk("mod/neg-y", 7 % -2, -1);
chk("mod/neg-both", -7 % -2, -1);
chk("mod/frac", -7.5 % 2, 0.5);
// math.lerp
chk("lerp/mid", math.lerp(0, 10, 0.5), 5);
chk("lerp/zero", math.lerp(0, 10, 0), 0);
chk("lerp/one", math.lerp(0, 10, 1), 10);
chk("lerp/extrap", math.lerp(0, 10, 2), 20);
chk("lerp/neg", math.lerp(10, 0, 0.25), 7.5);
// table.setAttr：已有键直改
var t = { a = 1, b = 2 };
table.setAttr(t, "a", 111);
chk("setAttr/existing", t.a, 111);
chk("setAttr/existing-other", t.b, 2);
// 无 _set 元方法时落回普通成员写入
table.setAttr(t, "c", 333);
chk("setAttr/new", t.c, 333);
// 走 _meta._set 元方法（官方元方法在 _meta 表里，签名 (k,v,ownerAttr)）
var hit = { _meta = { _set = function(k, v, ownerAttr) {
    this[k] = "set:" + k + "=" + v + ":" + ownerAttr;
    return true;
} } };
table.setAttr(hit, "x", 9);
chk("setAttr/meta", hit.x, "set:x=9:true");
// 数字下标命中数组部分（1-based）
var t5 = { "hello", "world" };
table.setAttr(t5, 1, "WORLD");
chk("setAttr/elem", t5[1], "WORLD");
chk("setAttr/elem2", t5[2], "world");
if (fail) { io.print("== MATH/TABLE CHECK FAILED: ", fail); return 1; }
io.print("== math.fmod / math.lerp / table.setAttr OK ==");
'@
Expect-Section $mathCode 'math.fmod / math.lerp / table.setAttr / %' 'math.fmod / math.lerp / table.setAttr OK'

# ================================================================
# 4) com 官方引擎原生 API
# ================================================================
$comCode = @'
var fail = 0;
function chk(name, ok, detail) {
  if (ok) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " ", detail); }
}
// com.SafeArray(vt, ...) -> { _safearray = 元素表, _type = vt }
var sa = com.SafeArray(0x11, 1, 2, 3);
chk("SafeArray/type", type(sa) == "table", "" + type(sa));
chk("SafeArray/count", #sa._safearray == 3, "" + #sa._safearray);
chk("SafeArray/vt", sa._type == 0x11, "" + sa._type);
chk("SafeArray/elem", sa._safearray[2] == 2, "" + sa._safearray[2]);
// com.IsIUnknown
chk("IsIUnknown/table", com.IsIUnknown({}) == false, "");
chk("IsIUnknown/null", com.IsIUnknown(null) == false, "");
// com.NewObject 失败路径：返回 (null, null, 错误信息)
var obj, ev, err = com.NewObject(null, "no.such.progid.xyz");
chk("NewObject/fail-obj", obj == null, "" + obj);
chk("NewObject/fail-event", ev == null, "" + ev);
chk("NewObject/fail-err", type(err) == "string", "" + type(err));
chk("NewObject/fail-err-notempty", err != null && #err > 0, "" + err);
var o0, e0, x0 = com.NewObject(null, "");
chk("NewObject/no-progid", x0 != null && #x0 > 0, "" + x0);
// com.NewControl 同形状
var o2, e2, r2 = com.NewControl(null, "no.such.progid.xyz");
chk("NewControl/fail-obj", o2 == null, "" + o2);
chk("NewControl/fail-err", type(r2) == "string", "" + type(r2));
// 成功路径：event 是锚点表，e = null
var o4, e4, x4 = com.NewObject(null, "Scripting.FileSystemObject");
if (o4 != null) {
  chk("NewObject/ok-obj", o4 != null, "");
  chk("NewObject/ok-event", type(e4) == "table", "" + type(e4));
  chk("NewObject/ok-err", x4 == null, "" + x4);
} else {
  io.print("SKIP NewObject success path (COM 未注册: ", x4, ")");
}
// com.IsMember / com.GetPointer：非 COM 对象
chk("IsMember/noncom", com.IsMember({}, "Foo") == false, "");
chk("GetPointer/noncom", com.GetPointer({}) == null, "");
// 多返回首值为 null 时 mrets 不能丢（回归护栏）
function f3() { return null, "B", "C"; }
var a5, b5, c5 = f3();
chk("multi-return/null-first-a", a5 == null, "" + a5);
chk("multi-return/null-first-b", b5 == "B", "" + b5);
chk("multi-return/null-first-c", c5 == "C", "" + c5);
if (fail) { io.print("== COM CHECK FAILED: ", fail); return 1; }
io.print("== com official-native API OK ==");
'@
Expect-Section $comCode 'com official-native API' 'com official-native API OK'

# ================================================================
# 5) com.Connect / com.AddConnection / com.ReleaseConnection
#    COM 事件接收器：官方 builtin/com.aardio 的 ConnectWeak / CreateEmbedEx
#    都调用引擎原生的 Connect，所以必须由内核提供。
# ================================================================
$connCode = @'
var fail = 0;
function chk(name, ok, detail) {
  if (ok) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " ", detail); }
}
// 没有事件连接点的对象 -> 明确报错（不能静默失败）
var fso = com.NewObject(null, "Scripting.FileSystemObject");
if (fso != null) {
  var errTxt = "";
  try { com.Connect(fso, { OnEvent = function() {} }); }
  catch (e) { errTxt = "" + e; }
  chk("Connect/no-connection-point-err",
      string.find(errTxt, "事件连接点") != null, errTxt);
}
// 有 dispinterface 事件连接点的对象（MSXML DOMDocument）
var doc = com.NewObject(null, "MSXML2.DOMDocument.6.0");
if (doc == null) {
  io.print("SKIP Connect roundtrip (MSXML2.DOMDocument 未注册)");
} else {
  var fired = 0;
  var sinkTbl = { onreadystatechange = function(a, b) { fired++; } };
  var sink, cookie = com.Connect(doc, sinkTbl);
  chk("Connect/sink-is-table", type(sink) == "table", "" + type(sink));
  chk("Connect/cookie", cookie != null && cookie > 0, "" + cookie);
  // 通过 sink 自身的 IDispatch 走一遍
  // GetIDsOfNames -> Invoke -> DISPID -> 事件名 -> aardio 处理函数
  sink.onreadystatechange(1, 2);
  chk("Connect/event-dispatch", fired == 1, "fired=" + fired);
  // 官方 ConnectWeak（aardio 源码，内部调用引擎原生的 Connect）
  var fired2 = 0;
  var realSink = { onreadystatechange = function(a, b) { fired2++; } };
  var sink2, cookie2 = com.ConnectWeak(doc, realSink);
  chk("ConnectWeak/cookie", cookie2 != null && cookie2 > 0, "" + cookie2);
  sink2.onreadystatechange(1, 2);
  chk("ConnectWeak/event-dispatch", fired2 == 1, "fired2=" + fired2);
  // 释放连接
  chk("ReleaseConnection/count", com.ReleaseConnection(doc) >= 1, "");
  chk("ReleaseConnection/again", com.ReleaseConnection(doc) == 0, "");
}
if (fail) { io.print("== COM CONNECT CHECK FAILED: ", fail); return 1; }
io.print("== com.Connect / ReleaseConnection OK ==");
'@
Expect-Section $connCode 'com.Connect / ReleaseConnection' 'com.Connect / ReleaseConnection OK'

# ================================================================
# 6) comTypeInfo 类型信息契约 + 官方类型库消费者
#    官方契约来源：
#      D:\aardio\lib\builtin\com.aardio:269-341  (!comTypeInfo / !comTypeAttr /
#                                                   !comTypeFunc / !comTypeParameter)
#      D:\aardio\lib\com\typeAttrs.aardio:11-20  (com.typeAttrs.load/summary)
#      D:\aardio\lib\com\tlbDoc.aardio:32-47      (com.tlbDoc)
#      D:\aardio\lib\com\interface\_.aardio:143-168 (com.interface.loadTypelibrary)
# ================================================================
$tlbCode = @'
var fail = 0;
function chk(name, got, want) {
  var ok = true;
  if (type(got) != type(want)) ok = false;
  else if (got !== want) ok = false;
  if (ok) { io.print("PASS ", name); }
  else { io.print("FAIL ", name, " got=", "" + got, " want=", "" + want); fail++; }
}
import com;
import com.typeAttrs;
import com.tlbDoc;
import com.interface;

var tlbPath = "C:\Windows\System32\stdole2.tlb";
var tlb = com.LoadTypeLibrary(tlbPath);
chk("LoadTypeLibrary", tlb != null, true);
var cnt = tlb.GetTypeInfoCount();
chk("GetTypeInfoCount", cnt > 10, true);

// ---- GetTypeAttr：官方 !comTypeAttr ----
var attr = null;
for (i = 1; cnt; 1) {
    var info = tlb.GetTypeInfo(i);
    var a = info.GetTypeAttr();
    if (a.typekind == "interface") { attr = a; break; }
}
chk("GetTypeAttr/typekind", attr.typekind, "interface");
chk("GetTypeAttr/GUID-len", #attr.GUID, 38);
chk("GetTypeAttr/GUID-prefix", string.left(attr.GUID, 1), "{");
chk("GetTypeAttr/Funcs", attr.Funcs > 0, true);
chk("GetTypeAttr/Vars", attr.Vars >= 0, true);
chk("GetTypeAttr/ImplTypes", attr.ImplTypes >= 0, true);
chk("GetTypeAttr/flags-table", type(attr.flags), "table");
chk("GetTypeAttr/flags-cancreate", type(attr.flags.cancreate), "boolean");
chk("GetTypeAttr/flags-dispatchable", type(attr.flags.dispatchable), "boolean");

// ---- GetFuncDesc：官方 !comTypeFunc（索引从 1 开始）----
var iu = null;
for (i = 1; cnt; 1) {
    var inf = tlb.GetTypeInfo(i);
    if (inf.GetDocumentation().name == "IUnknown") { iu = inf; break; }
}
var fd = iu.GetFuncDesc(1);
chk("GetFuncDesc/name", fd.name, "QueryInterface");
chk("GetFuncDesc/invkind", fd.invkind, "function");
chk("GetFuncDesc/Params", fd.Params, 2);
chk("GetFuncDesc/ParamsOpt", fd.ParamsOpt, 0);
chk("GetFuncDesc/type", fd.type, "int");
chk("GetFuncDesc/vt", fd.vt, 25);
chk("GetFuncDesc/memid", fd.memid, 1610612736);
chk("GetFuncDesc/dispatchable", type(fd.dispatchable), "boolean");
chk("GetFuncDesc/restricted", fd.restricted, true);
chk("GetFuncDesc/description-len", #fd.description > 0, true);
chk("GetFuncDesc/parameters-len", #fd.parameters, 2);
chk("GetFuncDesc/param-name", fd.parameters[1].name, "riid");
chk("GetFuncDesc/param-in", fd.parameters[1].in, true);
chk("GetFuncDesc/param-out", fd.parameters[1].out, false);
chk("GetFuncDesc/param-opt", fd.parameters[1].opt, false);
chk("GetFuncDesc/param-type", fd.parameters[1].type, "pointer");

// ---- 官方库消费者 ----
var sum = com.typeAttrs.summary(tlbPath);
chk("typeAttrs/summary", sum != null && #sum.interfaces > 0, true);
chk("typeAttrs/summary-coclass", #sum.coclasses > 0, true);
var all = com.typeAttrs.load(tlbPath);
chk("typeAttrs/load", #all, cnt);
var doc = com.tlbDoc(tlbPath);
chk("tlbDoc/length", #doc > 1000, true);
chk("tlbDoc/has-name", string.find(doc, "QueryInterface") != null, true);

// ---- 官方 com.interface.loadTypelibrary：动态生成接口声明类 ----
com.interface.loadTypelibrary(tlbPath, "IFont");
var cls = com.interface["IFont"];
chk("loadTypelibrary/class", cls != null, true);
chk("loadTypelibrary/IID", cls.IID, "{BEF6E002-A874-101A-8BBA-00AA00300CAB}");
chk("loadTypelibrary/instantiate", cls() != null, true);

if (fail) { io.print("== TYPELIB CHECK FAILED: ", fail); return 1; }
io.print("== comTypeInfo / 类型库 OK ==");
'@
Expect-Section $tlbCode 'comTypeInfo / 官方类型库消费者' 'comTypeInfo / 类型库 OK'

# ================================================================
# 7) com.ImplInterface(3 参) / com.ImplInterfaceFromTypelib /
#    原始（非 IDispatch）接口的事件连接点 Advise
#
#    官方契约来源：
#      D:\aardio\lib\builtin\com.aardio:206-207
#        ImplInterface(.(implTable,progID,interfaceName)
#          例如：com.ImplInterface( flash.callevent ,"ShockwaveFlash.ShockwaveFlash","_IShockwaveFlashEvents")
#        ImplInterfaceFromTypelib(.(implTable,typelibPath,interfaceName,coclassNname)
#      D:\aardio\docs\library-guide\builtin\com\ImplInterface.md:19-24
#        var ocxEvents2 = { MediaChange = function(item){...} }
#        var eventSink = com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")
#
#    实测要点（Windows Media Player 的 _WMPOCXEvents 在类型库里是
#    dispinterface，但服务器的连接点源接口 IID 不是 IID_IDispatch）：
#      - 服务器按**源接口 IID** 查 sink，纯 IDispatch 接收器会被拒
#        （0x80040202 EVENT_E_ALL_SUBSCRIBERS_FAILED）
#      - 服务器随后用 IDispatch::Invoke 派发事件
#      - IDispatch::Invoke 的 pVarResult 是 VARIANT* 出参，按类型信息
#        推导会被误判成「按值 VARIANT」而读到未初始化内存 —— 必须硬编码
# ================================================================
$implCode = @'
var fail = 0;
function chk(name, ok, detail) {
  if (ok) { io.print("PASS ", name); }
  else { fail++; io.print("FAIL ", name, " ", detail); }
}
import com;

// ---- 3 参形式：官方文档示例的接口名不存在时必须报错 ----
var errTxt = "";
try { com.ImplInterface({ X = function(){} }, "WMPlayer.OCX.7", "_NoSuchEventsIface"); }
catch (e) { errTxt = "" + e; }
chk("ImplInterface/unknown-interface-err",
    string.find(errTxt, "没有接口") != null, errTxt);

// ---- 真实事件链路：Windows Media Player 的 _WMPOCXEvents ----
// WMP 的源接口 IID 不是 IID_IDispatch：接收器必须是原始 vtable，
// 服务器用 IDispatch::Invoke 派发事件。
var wmp = com.CreateObject("WMPlayer.OCX");
if (wmp == null) {
  io.print("SKIP ImplInterface event chain (WMPlayer.OCX 未注册)");
} else {
  var fired = {};
  var events = {
    OpenStateChange = function(newState){
      fired[#fired + 1] = "OpenStateChange:" ++ newState;
    };
    PlayStateChange = function(newState){
      fired[#fired + 1] = "PlayStateChange:" ++ newState;
    };
    StatusChange = function(){
      fired[#fired + 1] = "StatusChange";
    };
  };

  // A) com.ImplInterface(implTable, progID, interfaceName)
  var sink = com.ImplInterface(events, "WMPlayer.OCX.7", "_WMPOCXEvents");
  chk("ImplInterface/3arg-sink", type(sink) == "table", "" + type(sink));
  var cookie = com.AddConnection(wmp, sink);
  chk("ImplInterface/3arg-cookie", cookie != null && cookie > 0, "" + cookie);

  // B) com.ImplInterfaceFromTypelib(implTable, typelibPath, interfaceName, coclassName)
  var fired2 = {};
  var events2 = {
    OpenStateChange = function(s){ fired2[#fired2 + 1] = "OpenStateChange:" ++ s; };
    StatusChange = function(){ fired2[#fired2 + 1] = "StatusChange"; };
  };
  var sink2 = com.ImplInterfaceFromTypelib(events2,
        "C:\\Windows\\System32\\wmp.dll", "_WMPOCXEvents", "WindowsMediaPlayer");
  chk("ImplInterfaceFromTypelib/sink", type(sink2) == "table", "" + type(sink2));
  var cookie2 = com.AddConnection(wmp, sink2);
  chk("ImplInterfaceFromTypelib/cookie", cookie2 != null && cookie2 > 0, "" + cookie2);

  // C) com.Connect 自动路径（默认事件接口）
  var fired3 = {};
  var events3 = {
    OpenStateChange = function(s){ fired3[#fired3 + 1] = "OpenStateChange:" ++ s; };
    StatusChange = function(){ fired3[#fired3 + 1] = "StatusChange"; };
  };
  var sink3, cookie3 = com.Connect(wmp, events3);
  chk("Connect/auto-sink", type(sink3) == "table", "" + type(sink3));
  chk("Connect/auto-cookie", cookie3 != null && cookie3 > 0, "" + cookie3);

  // 触发事件：设置 url 会让 WMP 连续派发 OpenStateChange/StatusChange/PlayStateChange
  wmp.url = "C:\\Windows\\Media\\ding.wav";
  sleep(1500);

  chk("ImplInterface/3arg-event-fired", #fired > 0, "fired=" + #fired);
  chk("ImplInterface/3arg-event-name",
      #fired > 0 && string.find(fired[1], "OpenStateChange") == 1, "" + (fired[1] || ""));
  chk("ImplInterfaceFromTypelib/event-fired", #fired2 > 0, "fired2=" + #fired2);
  chk("Connect/auto-event-fired", #fired3 > 0, "fired3=" + #fired3);
  // 参数值正确（OpenStateChange 的 newState 是整数）
  chk("ImplInterface/3arg-event-arg",
      #fired > 0 && string.find(fired[1], ":") != null, "" + (fired[1] || ""));

  com.ReleaseConnection(wmp, sink, cookie);
  com.ReleaseConnection(wmp, sink2, cookie2);
  com.ReleaseConnection(wmp);
}

if (fail) { io.print("== IMPLINTERFACE CHECK FAILED: ", fail); return 1; }
io.print("== com.ImplInterface / 原始接口事件 OK ==");
'@
Expect-Section $implCode 'com.ImplInterface / 原始接口事件' 'com.ImplInterface / 原始接口事件 OK'

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($fail -gt 0) { "== builtin api FAILED ($fail) =="; exit 1 }
"== builtin api OK =="
exit 0
