# tests/dsg_parse_probe.py — 探测引擎对「命名实参 / DSG 块」语法的解析能力。
#
# aardio 的表单文件几乎都是这个形状：
#   winform.add(
#       ed={cls="richedit";left=240;...}
#   )
# 引擎如果解析不了，IDE 的问题列表会给**每个表单文件**报一条假语法错误。
import json
import subprocess
import sys
import threading

EXE = sys.argv[1] if len(sys.argv) > 1 else r"D:\Nodio\build-x64\Release\nodio-lsp.exe"
p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE)


def send(m):
    b = json.dumps(m, ensure_ascii=False).encode("utf-8")
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    p.stdin.flush()


def read(t=10):
    r = {}

    def pump():
        h = {}
        line = p.stdout.readline().decode("ascii", "replace").strip()
        while line:
            if ":" in line:
                k, v = line.split(":", 1)
                h[k.strip()] = v.strip()
            line = p.stdout.readline().decode("ascii", "replace").strip()
        n = int(h.get("Content-Length", 0))
        r["b"] = p.stdout.read(n).decode("utf-8", "replace") if n else ""

    th = threading.Thread(target=pump, daemon=True)
    th.start()
    th.join(t)
    return r.get("b")


CASES = {
    "A_add_named": 'winform.add(\n\ted={cls="richedit";left=1}\n)\n',
    "B_add_brace": 'winform.add(\n\t{cls="richedit";left=1}\n)\n',
    "C_two_named": 'winform.add(\n\ta={cls="x"};\n\tb={cls="y"}\n)\n',
    "D_table_var": 'var t = {cls="richedit";left=1}\n',
    "E_dsg_block": '/*DSG{{*/\nwinform.add(\n\ted={cls="richedit";left=1}\n)\n/*}}*/\n',
    # 逐字照抄 ide/nodio/app/shell.nodio 的 DSG 块
    "F_shell_dsg": ('/*DSG{{*/\n'
                    'winform = win.form(text="Nodio IDE";right=1280;bottom=820)\n'
                    'winform.add(\n'
                    '\ted={cls="richedit";left=240;top=58;right=980;bottom=560;'
                    'multiline=true;vscroll=true;hscroll=true;z=6}\n'
                    ')\n'
                    '/*}}*/\n'),
    "G_form_only": 'winform = win.form(text="Nodio IDE";right=1280;bottom=820)\n',
    "H_ctrl_multi": ('winform.add(\n'
                     '\ted={cls="richedit";left=240;top=58;right=980;bottom=560;'
                     'multiline=true;vscroll=true;hscroll=true;z=6}\n'
                     '\tbtn={cls="button";left=1;top=1;right=2;bottom=2}\n'
                     ')\n'),
    # 字符串里带控制字符：JSON 转义必须原样还原
    "I_escapes": 'var s = "a\\tb\\nc";\nvar t = "q\\\\z";\n',
}

send({"jsonrpc": "2.0", "method": "initialize", "params": {"capabilities": {}}, "id": 1})
read()

fails = []


def check(uri, text, label):
    """Send one document and require an empty diagnostics array."""
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "nodio",
                         "version": 1, "text": text}}})
    d = read() or ""
    ok = '"diagnostics":[]' in d
    print(("ok  " if ok else "FAIL"), label, "" if ok else d[:240])
    if not ok:
        fails.append(label)


# 真实文件：把 IDE 工程里的 shell.nodio 整份丢进去，看报什么错。
# 必须同时测 LF 与 CRLF：IDE 的编辑器缓冲是 CRLF，磁盘是 LF，
# 两者都要零诊断。
try:
    raw = open(r"D:\Nodio\ide\nodio\app\shell.nodio", "rb").read()
    print("REAL bytes CRLF count:", raw.count(b"\r\n"), "LF count:", raw.count(b"\n"))
    lf = raw.replace(b"\r\n", b"\n")
    crlf = lf.replace(b"\n", b"\r\n")
    check("file:///d:/real_lf.nodio", lf.decode("utf-8"), "REAL lf")
    check("file:///d:/real_crlf.nodio", crlf.decode("utf-8"), "REAL crlf")
except Exception as e:
    print("REAL: skipped (%s)" % e)

for name, src in CASES.items():
    check("file:///d:/" + name + ".nodio", src, name)
    # 同一份代码换成 CRLF：JSON 转义 "\r\n" 必须被还原成真正的 CRLF，
    # 否则词法器会看到一个多余的 'r' 标识符并报出假语法错误。
    check("file:///d:/" + name + "_crlf.nodio", src.replace("\n", "\r\n"),
          name + " (crlf)")

send({"jsonrpc": "2.0", "method": "exit"})
if fails:
    print("DSG-PARSE-PROBE-FAIL (%d): %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
print("DSG-PARSE-PROBE-OK")

