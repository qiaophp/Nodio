# tests/lsp_smoke.py — LSP smoke test: drives nodio-lsp.exe over stdio with
# Content-Length framed JSON-RPC and asserts the full editing workflow:
# initialize -> didOpen(bad source) -> diagnostics -> didChange(fixed) ->
# diagnostics cleared -> documentSymbol -> hover -> completion -> unknown
# request -> shutdown/exit.
#
#   python tests/lsp_smoke.py [path\to\nodio-lsp.exe]
import json
import subprocess
import sys
import threading

EXE = sys.argv[1] if len(sys.argv) > 1 else r"D:\Nodio\build-x64\Release\nodio-lsp.exe"

p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE)

def send(msg):
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    p.stdin.flush()

def read(timeout=10):
    result = {}

    def pump():
        headers = {}
        line = p.stdout.readline().decode("ascii", "replace").strip()
        while line:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip()] = v.strip()
            line = p.stdout.readline().decode("ascii", "replace").strip()
        n = int(headers.get("Content-Length", 0))
        result["body"] = p.stdout.read(n).decode("utf-8", "replace") if n else ""

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    t.join(timeout)
    return result.get("body")

failures = []
def check(what, cond):
    print(("ok: " if cond else "FAIL: ") + what)
    if not cond:
        failures.append(what)

send({"jsonrpc": "2.0", "method": "initialize", "params": {"capabilities": {}}, "id": 1})
init = read()
check("initialize reply", init is not None and '"capabilities"' in init)

# broken source -> publishDiagnostics with severity 1
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio", "languageId": "nodio",
                     "version": 1, "text": "var a = 1\nvar = = 3\n"}}})
diag = read()
check("diagnostics for broken source",
      diag is not None and "publishDiagnostics" in diag and '"severity":1' in diag)
check("diagnostic names the line", diag is not None and '"line":1' in diag)

# fixed source -> empty diagnostics
GOOD = "var a = 1\nfunction foo(x) { return x }\nnamespace demo { var v = 2 }\n"
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio", "version": 2},
    "contentChanges": [{"text": GOOD}]}})
diag2 = read()
check("diagnostics cleared after fix",
      diag2 is not None and '"diagnostics":[]' in diag2)

# documentSymbol
send({"jsonrpc": "2.0", "method": "textDocument/documentSymbol", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"}}, "id": 4})
syms = read()
check("documentSymbol has foo", syms is not None and "foo" in syms)
check("documentSymbol has demo", syms is not None and "demo" in syms)
check("documentSymbol has nested v", syms is not None and '"children"' in syms)

# hover over foo (line index 1)
send({"jsonrpc": "2.0", "method": "textDocument/hover", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "position": {"line": 1, "character": 12}}, "id": 5})
hov = read()
check("hover returns markdown", hov is not None and "markdown" in hov and "foo" in hov)

# completion
send({"jsonrpc": "2.0", "method": "textDocument/completion", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "position": {"line": 1, "character": 12}}, "id": 6})
comp = read()
check("completion returns items", comp is not None and '"items"' in comp)
check("completion lists keywords", comp is not None and "function" in comp)

# 第五十三轮：LSP 协议补全（八方法）
# declaration/typeDefinition/implementation 与 definition 同义：
# didOpen 的源码第 3 行应有函数定义（见上方 didOpen 段的源码），把位置
# 放在那个函数名上，别名三方法必须回出与 definition 一致的 location。
send({"jsonrpc": "2.0", "method": "textDocument/definition", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "position": {"line": 1, "character": 10}}, "id": 20})
d0 = read()
for i, m in enumerate(["textDocument/declaration",
                       "textDocument/typeDefinition",
                       "textDocument/implementation"]):
    send({"jsonrpc": "2.0", "method": m, "params": {
        "textDocument": {"uri": "file:///d:/probe.nodio"},
        "position": {"line": 1, "character": 10}}, "id": 21 + i})
    r = read()
    check("alias %s matches definition" % m.split("/")[-1],
          d0 is not None and r is not None and
          d0.replace('"id":20', "") == r.replace('"id":%d' % (21 + i), ""))

# prepareRename：位置上符号的范围 + placeholder
send({"jsonrpc": "2.0", "method": "textDocument/prepareRename", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "position": {"line": 1, "character": 10}}, "id": 30})
pr = read()
check("prepareRename range+placeholder",
      pr is not None and '"placeholder"' in pr and '"range"' in pr)

# selectionRange：词范围单层
send({"jsonrpc": "2.0", "method": "textDocument/selectionRange", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "positions": [{"line": 1, "character": 10}]}, "id": 31})
sr = read()
check("selectionRange word range",
      sr is not None and '"parent":null' in sr and '"range"' in sr)

# foldingRange：缩进折叠至少一个区（didOpen 源码含函数体）
send({"jsonrpc": "2.0", "method": "textDocument/foldingRange", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"}}, "id": 32})
fr = read()
check("foldingRange non-empty",
      fr is not None and '"startLine"' in fr and fr != '"result":[]')

# formatting：全文重排返回 textEdits 数组且 newText 带缩进
send({"jsonrpc": "2.0", "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "options": {"tabSize": 4, "insertSpaces": False}}, "id": 33})
fmt = read()
check("formatting returns textEdits",
      fmt is not None and '"newText"' in fmt and fmt != '"result":[]')

# codeAction（v1 空数组）/ signatureHelp（v1 null）：协议完整不阻塞
send({"jsonrpc": "2.0", "method": "textDocument/codeAction", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": 0, "character": 0}},
    "context": {"diagnostics": []}}, "id": 34})
ca = read()
check("codeAction replies empty array",
      ca is not None and '"result":[]' in ca)
send({"jsonrpc": "2.0", "method": "textDocument/signatureHelp", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"},
    "position": {"line": 1, "character": 10}}, "id": 35})
sh = read()
check("signatureHelp replies null",
      sh is not None and '"result":null' in sh)

# semanticTokens：语义高亮 full 返回 token 数组
send({"jsonrpc": "2.0", "method": "textDocument/semanticTokens/full", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"}}, "id": 36})
st = read()
check("semanticTokens returns data",
      st is not None and '"data":[' in st and len(st) > 30)

# unknown request -> null result, no hang
send({"jsonrpc": "2.0", "method": "textDocument/unknownXyz", "params": {
    "textDocument": {"uri": "file:///d:/probe.nodio"}}, "id": 7})
unk = read()
check("unknown request replies null", unk is not None and '"result":null' in unk)

# shutdown / exit
send({"jsonrpc": "2.0", "method": "shutdown", "params": None, "id": 99})
sd = read()
check("shutdown replies", sd is not None and '"id":99' in sd)
send({"jsonrpc": "2.0", "method": "exit"})
try:
    p.wait(timeout=5)
    check("server exits cleanly", p.returncode == 0)
except subprocess.TimeoutExpired:
    p.kill()
    check("server exits cleanly", False)

if failures:
    print("LSP-SMOKE-FAIL (%d)" % len(failures))
    sys.exit(1)
print("LSP-SMOKE-OK")
