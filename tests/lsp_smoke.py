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
