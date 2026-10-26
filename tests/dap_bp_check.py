#!/usr/bin/env python3
# tests/dap_bp_check.py — 条件/命中次数断点的协议级回归锁（第五十九轮）。
#
# 链路：DAP setBreakpoints(condition/hitCondition) → 适配器 bp2 命令
#       → 引擎 bpMeta（BpShouldStop：条件求值 + 命中计数）→ stopped 事件。
# 同场加锁三件既有行为：
#   ① 空断点表必须真的清除（旧实现 lines 为空就不发 → 消不掉最后的断点）；
#   ② stopped 事件的 reason 透传（launch 首停 = "step"，断点 = "breakpoint"）；
#   ③ setBreakpoints 响应带回 verified 列表。
#
# 用法：python tests/dap_bp_check.py <nodio.exe> [nodio-dap.exe]
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
    """与 dap_eval_check.py 同款的最小 DAP 客户端（保持自包含）。"""

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
        except Exception:
            return None

    def request(self, command, body=None, timeout=10.0):
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


FIXTURE = (
    'var x = 0;\n'        # 1
    'while(x < 6){\n'     # 2  — while 是一条语句，断点要打在循环体行
    '    x = x + 1;\n'    # 3  — 每圈执行一次，到达时 x = 0,1,2,3,4,5
    '}\n'                 # 4
    'io.print("done");\n' # 5
)

BP_LINE = 3  # 1-based；循环体语句行


def eval_x(d):
    r = d.request("evaluate", {"expression": "x", "context": "repl"},
                  timeout=15)
    return (r or {}).get("body", {}).get("result")


def launch(d, exe, script):
    r = d.request("initialize", {"adapterID": "nodio",
                                 "linesStartAt1": True})
    chk_true("initialize", r is not None and r.get("success"), str(r))
    r = d.request("launch", {"program": script, "nodioExe": exe}, timeout=20)
    chk_true("launch", r is not None and r.get("success"), str(r))
    return d.wait_event("stopped")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else \
        r"D:\Nodio\build-x64\Release\nodio.exe"
    dap_exe = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(exe), "nodio-dap.exe")
    for p in (exe, dap_exe):
        if not os.path.exists(p):
            print("FAIL 缺少文件:", p)
            return 1

    script = os.path.join(os.environ["TEMP"], "nodio_dap_bp_fixture.aardio")
    with open(script, "w", encoding="utf-8") as f:
        f.write(FIXTURE)

    env = dict(os.environ)
    env["NODIO_EXE"] = exe
    env.setdefault("NODIO_LIBDIR", r"D:\aardio\lib")
    env.setdefault("NODIO_OVERLAY_LIBDIR", r"D:\Nodio\lib")

    # ---------- 场景 A：条件断点（x >= 3 才停） ----------
    d = Dap(dap_exe, env)
    try:
        ev = launch(d, exe, script)
        chk_true("cond: stopped-on-launch", ev is not None, "没等到首停")
        chk("cond: launch-stop-reason-is-step",
            (ev or {}).get("body", {}).get("reason"), "step")

        r = d.request("setBreakpoints",
                      {"source": {"path": script},
                       "breakpoints": [{"line": BP_LINE,
                                        "condition": "x >= 3"}]})
        chk_true("cond: setBreakpoints-ok",
                 r is not None and r.get("success"), str(r))
        vb = (r or {}).get("body", {}).get("breakpoints")
        chk_true("cond: reply-has-verified", bool(vb) and vb[0].get("verified"),
                 str(vb))
        d.request("configurationDone")
        d.request("continue")
        ev = d.wait_event("stopped")
        chk_true("cond: stopped-at-condition", ev is not None,
                 "条件断点没有停")
        chk("cond: stopped-line", (ev or {}).get("body", {}).get("line"),
            BP_LINE - 1)
        chk("cond: stop-reason",
            (ev or {}).get("body", {}).get("reason"), "breakpoint")
        chk("cond: x-is-3-at-first-stop", eval_x(d), "3")

        # 清空断点表 → 必须真的清掉（旧实现清不掉），一路跑到 terminated
        r = d.request("setBreakpoints",
                      {"source": {"path": script}, "breakpoints": []})
        chk_true("clear: setBreakpoints-ok",
                 r is not None and r.get("success"), str(r))
        d.request("continue")
        ev = d.wait_event("terminated", timeout=20)
        chk_true("clear: no-more-stops", ev is not None,
                 "断点没被清掉（又停了/没结束）")
    finally:
        d.close()

    # ---------- 场景 B：命中次数断点（每第 2 次到达才停） ----------
    d = Dap(dap_exe, env)
    try:
        launch(d, exe, script)
        r = d.request("setBreakpoints",
                      {"source": {"path": script},
                       "breakpoints": [{"line": BP_LINE,
                                        "hitCondition": "2"}]})
        chk_true("hit: setBreakpoints-ok",
                 r is not None and r.get("success"), str(r))
        d.request("configurationDone")
        d.request("continue")
        ev = d.wait_event("stopped")
        chk_true("hit: stopped-on-2nd-arrival", ev is not None,
                 "命中次数断点没有停")
        # 到达第 2 次时 x 已经自增过 1 次 → x == 1
        chk("hit: x-is-1-at-2nd-arrival", eval_x(d), "1")
        r = d.request("setBreakpoints",
                      {"source": {"path": script}, "breakpoints": []})
        d.request("continue")
        ev = d.wait_event("terminated", timeout=20)
        chk_true("hit: runs-to-end", ev is not None, "没跑到 terminated")
    finally:
        d.close()

    # ---------- 场景 C：条件为假必须不停（直接跑完） ----------
    d = Dap(dap_exe, env)
    try:
        launch(d, exe, script)
        r = d.request("setBreakpoints",
                      {"source": {"path": script},
                       "breakpoints": [{"line": BP_LINE,
                                        "condition": "x > 100"}]})
        chk_true("never: setBreakpoints-ok",
                 r is not None and r.get("success"), str(r))
        d.request("configurationDone")
        d.request("continue")
        ev = d.wait_event("terminated", timeout=20)
        chk_true("never: condition-false-never-stops", ev is not None,
                 "恒假条件竟然停了")
    finally:
        d.close()

    if fail:
        print("== DAP BP FAILED:", fail, "==")
        return 1
    print("== DAP BP OK ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
