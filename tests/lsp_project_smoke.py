# tests/lsp_project_smoke.py — project-level LSP features:
# nodio/setProject -> textDocument/definition -> textDocument/references ->
# textDocument/rename -> workspace/symbol.
#
# The fixture is the real IDE project (D:\Nodio\ide\nodio): `contextMenu` is
# defined in app/editor.nodio and used from app/shell.nodio, so a correct
# index must find the definition in another file.
#
#   python tests/lsp_project_smoke.py [path\to\nodio-lsp.exe]
import json
import os
import subprocess
import sys
import threading

EXE = sys.argv[1] if len(sys.argv) > 1 else r"D:\Nodio\build-x64\Release\nodio-lsp.exe"
ROOT = os.environ.get("NODIO_IDE_ROOT", r"D:\Nodio\ide\nodio")

p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE)


def send(msg):
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    p.stdin.flush()


def read(timeout=15):
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


def jbody(raw):
    """Pull the JSON-RPC payload out of a framed message body."""
    if not raw:
        return None
    try:
        return json.loads(raw)
    except Exception:
        return None


send({"jsonrpc": "2.0", "method": "initialize", "params": {"capabilities": {}}, "id": 1})
init = jbody(read())
check("initialize reply", init is not None and "capabilities" in init.get("result", {}))
caps = init.get("result", {}).get("capabilities", {})
check("advertises definitionProvider", caps.get("definitionProvider") is True)
check("advertises referencesProvider", caps.get("referencesProvider") is True)
check("advertises renameProvider", caps.get("renameProvider") is True)
check("advertises workspaceSymbolProvider", caps.get("workspaceSymbolProvider") is True)

# --- project index ---
files = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    for fn in filenames:
        if fn.endswith(".nodio") or fn.endswith(".aardio"):
            files.append(os.path.relpath(os.path.join(dirpath, fn), ROOT).replace("\\", "/"))
check("fixture files found", len(files) > 0)
send({"jsonrpc": "2.0", "method": "nodio/setProject",
      "params": {"root": ROOT, "files": files}})

shell_path = os.path.join(ROOT, "app", "shell.nodio")
shell_uri = "file:///" + shell_path.replace("\\", "/")
shell_text = open(shell_path, encoding="utf-8").read()
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": shell_uri, "languageId": "nodio", "version": 1,
                     "text": shell_text}}})
diag = read()
check("didOpen diagnostics", diag is not None)

# --- definition: `contextMenu` is used in shell.nodio but defined in editor.nodio ---
lines = shell_text.split("\n")
row = None
col = None
for i, ln in enumerate(lines):
    j = ln.find("app.editor.contextMenu")
    if j >= 0:
        row = i
        col = j + len("app.editor.")
        break
check("fixture call site found", row is not None)

send({"jsonrpc": "2.0", "method": "textDocument/definition", "params": {
    "textDocument": {"uri": shell_uri},
    "position": {"line": row, "character": col}}, "id": 10})
d = jbody(read())
res = d.get("result") if d else None
check("definition returns a location", isinstance(res, list) and len(res) > 0)
if isinstance(res, list) and res:
    loc = res[0]
    check("definition points at editor.nodio",
          loc["uri"].endswith("app/editor.nodio"))
    check("definition has a real column", loc["range"]["start"]["character"] >= 0)

# --- references ---
send({"jsonrpc": "2.0", "method": "textDocument/references", "params": {
    "textDocument": {"uri": shell_uri},
    "position": {"line": row, "character": col},
    "context": {"includeDeclaration": True}}, "id": 11})
d = jbody(read())
refs = d.get("result") if d else None
check("references returns a list", isinstance(refs, list))
if isinstance(refs, list):
    uris = set(r["uri"] for r in refs)
    check("references span editor.nodio and shell.nodio",
          any(u.endswith("app/editor.nodio") for u in uris) and
          any(u.endswith("app/shell.nodio") for u in uris))
    check("references carry columns", all("character" in r["range"]["start"] for r in refs))

# --- rename ---
send({"jsonrpc": "2.0", "method": "textDocument/rename", "params": {
    "textDocument": {"uri": shell_uri},
    "position": {"line": row, "character": col},
    "newName": "ctxMenuRenamed"}, "id": 12})
d = jbody(read())
ed = d.get("result") if d else None
check("rename returns a workspace edit", isinstance(ed, dict) and "changes" in ed)
if isinstance(ed, dict) and "changes" in ed:
    ch = ed["changes"]
    check("rename touches editor.nodio",
          any(u.endswith("app/editor.nodio") for u in ch))
    check("rename touches shell.nodio",
          any(u.endswith("app/shell.nodio") for u in ch))
    texts = set()
    for u, lst in ch.items():
        for e in lst:
            texts.add(e["newText"])
    check("rename uses the new name", texts == {"ctxMenuRenamed"})

# --- workspace symbols ---
send({"jsonrpc": "2.0", "method": "workspace/symbol", "params": {"query": "contextMenu"},
      "id": 13})
d = jbody(read())
ws = d.get("result") if d else None
check("workspace/symbol returns a list", isinstance(ws, list) and len(ws) > 0)
if isinstance(ws, list) and ws:
    check("workspace symbol names the function",
          any("contextMenu" in s.get("name", "") for s in ws))
    check("workspace symbol has a location",
          all("location" in s for s in ws))

send({"jsonrpc": "2.0", "method": "shutdown", "params": None, "id": 99})
read()
send({"jsonrpc": "2.0", "method": "exit"})
try:
    p.wait(timeout=5)
    check("server exits cleanly", p.returncode == 0)
except subprocess.TimeoutExpired:
    p.kill()
    check("server exits cleanly", False)

if failures:
    print("LSP-PROJECT-SMOKE-FAIL (%d)" % len(failures))
    sys.exit(1)
print("LSP-PROJECT-SMOKE-OK")
