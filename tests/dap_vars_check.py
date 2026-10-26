#!/usr/bin/env python3
# tests/dap_vars_check.py — 变量结构化展开 + setVariable 的协议级回归锁
# （第五十九轮续）。
#
# 链路：DAP variables(ref) → 适配器 varsRef 转发 → 引擎 varRefs 表子级
#       → DAP variables(ref>100) 条目；DAP setVariable → 引擎 setVar
#       （表达式在当前帧求值后写入）→ evaluate 复读验证。
# 同场加锁：
#   ① vars 是结构化数组：条目带 name/type/value/ref，数字/表分型；
#   ② 表条目 ref>100（0/100 留给帧作用域），可经 ref 展开子级；
#   ③ setVariable 改帧变量 / 表成员 / 数组元素三路都真实生效；
#   ④ 改不存在的变量必须失败（不凭空造变量）。
#
# 用法：python tests/dap_vars_check.py <nodio.exe> [nodio-dap.exe]
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
    'var x = 7;\n'          # 1
    'var t = {a = 1};\n'    # 2
    'var arr = [10, 20];\n' # 3
    'x = x + 1;\n'          # 4  — 断点行（停时语句未执行，x 仍是 7）
    'io.print("done");\n'   # 5
)

BP_LINE = 4  # 1-based


def eval_expr(d, expr):
    r = d.request("evaluate", {"expression": expr, "context": "repl"},
                  timeout=15)
    return (r or {}).get("body", {}).get("result")


def get_vars(d, ref=100):
    r = d.request("variables", {"variablesReference": ref}, timeout=15)
    return ((r or {}).get("body", {}) or {}).get("variables") or []


def by_name(vs, name):
    for v in vs:
        if v.get("name") == name:
            return v
    return None


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else \
        r"D:\Nodio\build-x64\Release\nodio.exe"
    dap_exe = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(exe), "nodio-dap.exe")
    for p in (exe, dap_exe):
        if not os.path.exists(p):
            print("FAIL 缺少文件:", p)
            return 1

    script = os.path.join(os.environ["TEMP"], "nodio_dap_vars_fixture.aardio")
    with open(script, "w", encoding="utf-8") as f:
        f.write(FIXTURE)

    env = dict(os.environ)
    env["NODIO_EXE"] = exe
    env.setdefault("NODIO_LIBDIR", r"D:\aardio\lib")
    env.setdefault("NODIO_OVERLAY_LIBDIR", r"D:\Nodio\lib")

    d = Dap(dap_exe, env)
    try:
        # launch → 首停（step）
        r = d.request("initialize", {"adapterID": "nodio",
                                     "linesStartAt1": True})
        chk_true("initialize", r is not None and r.get("success"), str(r))
        # 能力声明必须带 supportsSetVariable（IDE/客户端按它开编辑入口）
        caps = (r or {}).get("body", {})
        chk_true("initialize: supportsSetVariable",
                 caps.get("supportsSetVariable") is True, str(caps))
        r = d.request("launch", {"program": script, "nodioExe": exe},
                      timeout=20)
        chk_true("launch", r is not None and r.get("success"), str(r))
        ev = d.wait_event("stopped")
        chk_true("stopped-on-launch", ev is not None, "没等到首停")

        # 断点打在行 4 → 停下时 t/arr 已建好；停点快照是**语句执行前**
        # （与 bp_check 的 x==3 同一语义），所以 x 仍是 7
        r = d.request("setBreakpoints",
                      {"source": {"path": script},
                       "breakpoints": [{"line": BP_LINE}]})
        chk_true("setBreakpoints-ok",
                 r is not None and r.get("success"), str(r))
        d.request("configurationDone")
        d.request("continue")
        ev = d.wait_event("stopped")
        chk_true("stopped-at-bp", ev is not None, "没停在断点行")
        chk("stopped-line", (ev or {}).get("body", {}).get("line"),
            BP_LINE - 1)

        # ---------- ① 结构化 vars：分型 + ref 分配 ----------
        vs = get_vars(d, 100)
        chk_true("vars: nonempty", len(vs) >= 3, str(vs))
        vx = by_name(vs, "x")
        chk_true("vars: x-present", vx is not None, str(vs))
        chk("vars: x-value", vx and vx.get("value"), "7")
        chk("vars: x-type", vx and vx.get("type"), "number")
        chk("vars: x-no-ref", vx and vx.get("variablesReference"), 0)
        vt = by_name(vs, "t")
        chk_true("vars: t-present", vt is not None, str(vs))
        chk("vars: t-type", vt and vt.get("type"), "table")
        tref = vt and vt.get("variablesReference")
        chk_true("vars: t-ref-alloc", isinstance(tref, int) and tref > 100,
                 str(tref))
        va = by_name(vs, "arr")
        chk_true("vars: arr-present", va is not None, str(vs))
        aref = va and va.get("variablesReference")
        chk_true("vars: arr-ref-alloc", isinstance(aref, int) and aref > 100,
                 str(aref))
        chk_true("vars: distinct-refs", tref != aref, str((tref, aref)))

        # ---------- ② 表子级展开 ----------
        kids = get_vars(d, tref)
        ka = by_name(kids, "a")
        chk_true("expand: t.a-present", ka is not None, str(kids))
        chk("expand: t.a-value", ka and ka.get("value"), "1")
        chk("expand: t.a-type", ka and ka.get("type"), "number")

        # 数组子级：引擎把 [10,20] 存进 arr 槽，名字形如 "[1]"/"[2]"
        akids = get_vars(d, aref)
        a1 = by_name(akids, "[1]")
        chk_true("expand: arr[1]-present", a1 is not None, str(akids))
        chk("expand: arr[1]-value", a1 and a1.get("value"), "10")
        a2 = by_name(akids, "[2]")
        chk("expand: arr[2]-value", a2 and a2.get("value"), "20")

        # ---------- ③ setVariable 三路真实生效 ----------
        r = d.request("setVariable",
                      {"variablesReference": 100, "name": "x",
                       "value": "42"}, timeout=15)
        chk_true("set: frame-var-ok",
                 r is not None and (r.get("body") or {}).get("success"),
                 str(r))
        chk("set: frame-var-reply-value",
            r and (r.get("body") or {}).get("value"), "42")
        chk("set: frame-var-effective", eval_expr(d, "x"), "42")

        r = d.request("setVariable",
                      {"variablesReference": tref, "name": "a",
                       "value": "99"}, timeout=15)
        chk_true("set: table-member-ok",
                 r is not None and (r.get("body") or {}).get("success"),
                 str(r))
        chk("set: table-member-effective", eval_expr(d, "t.a"), "99")

        r = d.request("setVariable",
                      {"variablesReference": aref, "name": "[1]",
                       "value": "55"}, timeout=15)
        chk_true("set: array-elem-ok",
                 r is not None and (r.get("body") or {}).get("success"),
                 str(r))
        chk("set: array-elem-effective", eval_expr(d, "arr[1]"), "55")

        # ---------- ④ 不存在的变量必须失败 ----------
        r = d.request("setVariable",
                      {"variablesReference": 100, "name": "nosuch",
                       "value": "1"}, timeout=15)
        chk_true("set: missing-var-fails",
                 r is not None and
                 (r.get("body") or {}).get("success") is False, str(r))

        # 失效引用必须报错不崩溃（ref 远超已分配数量）
        r = d.request("variables", {"variablesReference": 9999}, timeout=15)
        chk_true("stale-ref: no-crash", r is not None, str(r))

        # 清断点跑完
        d.request("setBreakpoints", {"source": {"path": script},
                                     "breakpoints": []})
        d.request("continue")
        ev = d.wait_event("terminated", timeout=20)
        chk_true("runs-to-end", ev is not None, "没跑到 terminated")
    finally:
        d.close()

    if fail:
        print("== DAP VARS FAILED:", fail, "==")
        return 1
    print("== DAP VARS OK ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
