#!/usr/bin/env python3
# tests/dap_eval_check.py — DAP 表达式求值（evaluate）的协议级回归锁。
#
# 为什么用 Python 写客户端：DAP 是 Content-Length 分帧的 JSON-RPC，
# PowerShell 里读写分帧二进制流很啰嗦（仓库里已有 ai_mock_server.py 的
# Python 先例）。
#
# 验的是「引擎能在**当前暂停帧的环境**里求值」这条链路：
#   IDE → nodio-dap.exe（evaluate 请求）
#        → 引擎命令管道 {"cmd":"eval","expr":...}
#        → 引擎 Lex/Parser/Eval(node, env)
#        → 事件 {"event":"eval","value":...,"type":...}
#        → DAP 响应 {result; type}
#
# 用法：
#   python tests/dap_eval_check.py <nodio.exe> [nodio-dap.exe]
import json
import os
import subprocess
import sys
import time

fail = 0


def chk(name, got, want):
    global fail
    if got == want:
        print("PASS", name)
    else:
        fail += 1
        print("FAIL", name, "got=", repr(got), "want=", repr(want))


def chk_true(name, got, detail=""):
    global fail
    if got:
        print("PASS", name)
    else:
        fail += 1
        print("FAIL", name, detail)


class Dap:
    def __init__(self, dap_exe, env):
        self.p = subprocess.Popen(
            [dap_exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env)
        self.seq = 0

    def send(self, command, body=None):
        self.seq += 1
        msg = {"seq": self.seq, "type": "request", "command": command}
        if body is not None:
            msg["body"] = body
        raw = json.dumps(msg).encode("utf-8")
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(raw) + raw)
        self.p.stdin.flush()
        return self.seq

    def read(self, timeout=10.0):
        deadline = time.time() + timeout
        hdr = b""
        while b"\r\n\r\n" not in hdr:
            if time.time() > deadline:
                return None
            c = self.p.stdout.read(1)
            if not c:
                return None
            hdr += c
        n = 0
        for line in hdr.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                n = int(line.split(b":")[1].strip())
        body = self.p.stdout.read(n)
        try:
            return json.loads(body.decode("utf-8"))
        except Exception as e:
            print("  !! 收到无法解析的 DAP 帧:", repr(body[:200]), e)
            return None

    def request(self, command, body=None, timeout=10.0):
        """发请求并等它的响应（跳过事件）"""
        seq = self.send(command, body)
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self.read(min(2.0, max(0.2, deadline - time.time())))
            if m is None:
                continue
            if m.get("type") == "response" and m.get("request_seq") == seq:
                return m
        return None

    def wait_event(self, name, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self.read(2.0)
            if m is None:
                continue
            if m.get("type") == "event" and m.get("event") == name:
                return m
        return None

    def close(self):
        try:
            self.p.kill()
        except Exception:
            pass


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else \
        r"D:\Nodio\build-x64\Release\nodio.exe"
    dap_exe = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(exe), "nodio-dap.exe")

    for p in (exe, dap_exe):
        if not os.path.exists(p):
            print("FAIL 缺少文件:", p)
            return 1

    script = os.path.join(os.environ["TEMP"], "nodio_dap_eval_fixture.aardio")
    with open(script, "w", encoding="utf-8") as f:
        f.write('var x = 42;\n')
        f.write('var s = "hello";\n')
        f.write('io.print("hit");\n')
        f.write('io.print("done");\n')

    env = dict(os.environ)
    env["NODIO_EXE"] = exe
    env.setdefault("NODIO_LIBDIR", r"D:\aardio\lib")
    env.setdefault("NODIO_OVERLAY_LIBDIR", r"D:\Nodio\lib")

    d = Dap(dap_exe, env)
    try:
        r = d.request("initialize", {"adapterID": "nodio", "linesStartAt1": True})
        chk_true("initialize", r is not None and r.get("success"), str(r))

        r = d.request("launch", {"program": script, "nodioExe": exe}, timeout=20)
        chk_true("launch", r is not None and r.get("success"), str(r))

        # 引擎在第一条语句就停（bStep=true on connect）
        ev = d.wait_event("stopped")
        chk_true("stopped-on-launch", ev is not None, "没等到 stopped 事件")

        # 设断点（第 3 行）→ 继续 → 停在断点行（此时 x/s 已赋值）
        r = d.request("setBreakpoints",
                      {"source": {"path": script},
                       "breakpoints": [{"line": 3}]})
        chk_true("setBreakpoints", r is not None and r.get("success"), str(r))
        d.request("configurationDone")
        d.request("continue")
        ev = d.wait_event("stopped")
        chk_true("stopped-at-bp", ev is not None, "没等到断点 stopped 事件")
        if ev is not None:
            chk("bp-line", ev.get("body", {}).get("line"), 2)

        # ① 算术
        r = d.request("evaluate", {"expression": "1+2", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-arith", b.get("result"), "3")
        chk("eval-arith-type", b.get("type"), "number")

        # ② **局部变量**（证明在当前帧的环境里求值）
        r = d.request("evaluate", {"expression": "x", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-local-number", b.get("result"), "42")

        # ③ 局部字符串
        r = d.request("evaluate", {"expression": "s", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-local-string", b.get("result"), "hello")

        # ④ 中文（走 \uXXXX 编码往返：适配器转义 → 引擎解码 → 回传）
        r = d.request("evaluate", {"expression": '"中文OK"', "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-chinese", b.get("result"), "中文OK")

        # ⑤ 表达式里带引号（考验适配器的 JSON 转义 + 引擎的反转义）
        #    ⚠ aardio 铁律：**双引号是原样串、单引号才是转义串** →
        #      想在字符串里放双引号必须用单引号串 'a"b'；
        #      写 "a\"b" 是**非法语法**（引擎报「字符串未终止」才对）。
        #    这里用单引号串，验证引号能原样穿过整条链路（适配器转义 → 引擎反转义）。
        r = d.request("evaluate", {"expression": "'a\"b'", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-quote-in-string", b.get("result"), 'a"b')

        # ⑥ 单引号串里的反斜杠（转义串语义：'a\\b' → a\b）
        r = d.request("evaluate", {"expression": "'a\\\\b'", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-backslash", b.get("result"), "a\\b")

        # ⑥b 双引号串里的反斜杠是**原样**的（aardio 铁律：双引号原样串）
        r = d.request("evaluate", {"expression": '"a\\\\b"', "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-raw-doublequote-backslash", b.get("result"), "a\\\\b")

        # ⑦ 语法错误 → type=error
        r = d.request("evaluate", {"expression": "1+", "context": "repl"},
                      timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-syntax-error-type", b.get("type"), "error")
        chk_true("eval-syntax-error-msg", bool(b.get("result")), str(b))

        # ⑧ 调用引擎内建（证明求值走的是真解释器，不是字符串计算）
        r = d.request("evaluate", {"expression": "tostring(123+1)",
                                    "context": "repl"}, timeout=15)
        b = (r or {}).get("body", {})
        chk("eval-builtin-call", b.get("result"), "124")

        # ⑨ 变量视图仍工作（回归：别把既有能力碰坏）
        r = d.request("variables", {"variablesReference": 100}, timeout=10)
        vs = (r or {}).get("body", {}).get("variables")
        chk_true("variables-still-work", vs and len(vs) > 0, str(vs))

        # ⑩ 继续跑完 → terminated
        d.request("continue")
        ev = d.wait_event("terminated", timeout=20)
        chk_true("terminated", ev is not None, "没等到 terminated 事件")
    finally:
        d.close()

    if fail:
        print("== DAP EVAL FAILED:", fail, "==")
        return 1
    print("== DAP EVAL OK ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
