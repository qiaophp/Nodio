# tests/dap_smoke.py — DAP smoke test: drives nodio-dap.exe over stdio with
# Content-Length framed JSON-RPC and asserts a full debug session:
# initialize -> launch -> stopped -> threads/stackTrace/scopes/variables ->
# next -> stopped -> continue -> disconnect.
#
#   python tests/dap_smoke.py [path\to\nodio-dap.exe] [path\to\nodio.exe]
import json
import os
import subprocess
import sys
import threading
import time

DAP = sys.argv[1] if len(sys.argv) > 1 else r"D:\Nodio\build-x64\Release\nodio-dap.exe"
NODIO = sys.argv[2] if len(sys.argv) > 2 else r"D:\Nodio\build-x64\Release\nodio.exe"

# debuggee script: 3 statements, no infinite loops
SCRIPT = os.path.join(os.environ.get("TEMP", "."), "nodio_dap_smoke.aardio")
with open(SCRIPT, "w", encoding="utf-8") as f:
    f.write('var a = 1\nvar b = 2\nio.print("c=" ++ (a + b))\n')

p = subprocess.Popen([DAP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE)

def send(msg):
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    p.stdin.flush()

def read_one(timeout=15):
    out = {}
    def pump():
        h = {}
        line = p.stdout.readline()
        while line.strip():
            if b":" in line:
                k, v = line.split(b":", 1)
                h[k.strip().decode()] = v.strip().decode()
            line = p.stdout.readline()
        n = int(h.get("Content-Length", 0))
        out["b"] = p.stdout.read(n).decode("utf-8", "replace") if n else ""
    t = threading.Thread(target=pump, daemon=True)
    t.start()
    t.join(timeout)
    return out.get("b")

def expect(what, marker, timeout=15):
    """read messages until one contains marker; return it or None"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = read_one(timeout=max(1, deadline - time.time()))
        if m is None:
            break
        if marker in m:
            check(what, True)
            return m
    check(what, False)
    return None

failures = []
def check(what, cond):
    print(("ok: " if cond else "FAIL: ") + what)
    if not cond:
        failures.append(what)

send({"seq": 1, "type": "request", "command": "initialize",
      "arguments": {"adapterID": "nodio"}})
init = read_one()
check("initialize reply", init is not None and '"supportsConfigurationDoneRequest":true' in init)

send({"seq": 2, "type": "request", "command": "launch",
      "arguments": {"program": SCRIPT, "request": "launch", "type": "nodio"}})
expect("launch reply", '"request_seq":2')
expect("process event", '"event":"process"')
expect("initialized event", '"event":"initialized"')

# engine stops at the first statement -> stopped event
expect("stopped event after launch", '"event":"stopped"', timeout=10)

send({"seq": 3, "type": "request", "command": "setBreakpoints",
      "arguments": {"source": {"path": SCRIPT},
                    "breakpoints": [{"line": 2}]}})
bp = read_one()
check("setBreakpoints replies", bp is not None and '"breakpoints"' in bp)

send({"seq": 4, "type": "request", "command": "configurationDone"})
cd = read_one()
check("configurationDone replies", cd is not None and '"command":"configurationDone"' in cd)

send({"seq": 5, "type": "request", "command": "threads"})
th = read_one()
check("threads lists main", th is not None and '"main"' in th)

send({"seq": 6, "type": "request", "command": "stackTrace",
      "arguments": {"threadId": 1}})
st = read_one()
check("stackTrace has source", st is not None and '"stackFrames"' in st)

send({"seq": 7, "type": "request", "command": "scopes",
      "arguments": {"frameId": 1}})
sc = read_one()
check("scopes replies", sc is not None and '"scopes"' in sc)

send({"seq": 8, "type": "request", "command": "variables",
      "arguments": {"variablesReference": 100}})
va = read_one()
check("variables replies", va is not None and '"variables"' in va)

# step over -> next stopped
send({"seq": 9, "type": "request", "command": "next", "arguments": {"threadId": 1}})
nx = read_one()
check("next replies", nx is not None and '"command":"next"' in nx)
expect("stopped event after next", '"event":"stopped"', timeout=10)

send({"seq": 10, "type": "request", "command": "continue",
      "arguments": {"threadId": 1}})
ct = read_one()
check("continue replies", ct is not None and '"allThreadsContinued":true' in ct)

send({"seq": 11, "type": "request", "command": "disconnect",
      "arguments": {"terminateDebuggee": True}})
dc = read_one()
check("disconnect replies", dc is not None and '"command":"disconnect"' in dc)

try:
    p.wait(timeout=5)
    check("adapter exits cleanly", p.returncode == 0)
except subprocess.TimeoutExpired:
    p.kill()
    check("adapter exits cleanly", False)

if failures:
    print("DAP-SMOKE-FAIL (%d)" % len(failures))
    sys.exit(1)
print("DAP-SMOKE-OK")
