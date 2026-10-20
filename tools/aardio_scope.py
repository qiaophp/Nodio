#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统计每个记录种类里「局部 / 外层」作用域标志的用法分布。

目的：确定「全局(外层)作用域」到底指向什么——
  (a) 父 chunk 的帧槽表，还是
  (b) 本 chunk 的常量池。

判定依据：
  * 若某个 chunk 的「外层下标」始终 ≤ 自己的常量数-1 → 支持 (b)；
  * 同时看 LOAD_LOCAL 是否会带外层标志（= 「从常量池装载到局部槽」）。

用法:
    python aardio_scope.py <file> [对象路径]
"""
from __future__ import annotations

import sys
from collections import Counter, defaultdict

import aardio_bytecode as bc

NAMES = {
    0x00: "LOAD_OUTER", 0x01: "LOAD_INT", 0x02: "CLEAR_LOCALS", 0x03: "LOAD_UPVALUE",
    0x04: "LOAD_LOCAL", 0x05: "GET_MEMBER", 0x07: "GET_MEMBER2", 0x08: "GET_GLOBAL",
    0x09: "GET_MEMBER3", 0x0A: "GET_MEMBER4", 0x0B: "INDEX", 0x0C: "SET_MEMBER",
    0x0D: "SET_MEMBER2", 0x0E: "STORE_UPVALUE", 0x0F: "LOAD_IF_SET", 0x10: "SET_METATABLE",
    0x11: "SET_MEMBER3", 0x12: "CONCAT_MEMBER", 0x13: "RAWSET", 0x14: "SET_MEMBER4",
    0x15: "NEW_NATIVE", 0x16: "IMPORT_MEMBER", 0x17: "METHOD_PREP", 0x18: "ADD",
    0x20: "LENGTH", 0x21: "CONCAT", 0x22: "JMP", 0x23: "JZ", 0x24: "JE", 0x25: "JG",
    0x26: "JLE", 0x27: "JTRUE", 0x28: "JTRUE2", 0x29: "TAILCALL", 0x2A: "CALL",
    0x2B: "CALL_NATIVE", 0x2C: "RET", 0x2F: "CALL_MULTI", 0x30: "CALL_VALUE",
    0x31: "CALL_NS", 0x32: "BUILD_ARRAY", 0x33: "GC", 0x34: "NEW_CLOSURE",
}


def find(root, path):
    node = root
    for part in path.split("/"):
        if part in ("root", ""):
            continue
        node = node["objects"][int(part)]
    return node


def analyze(node, label):
    code = node["code"]
    scope_by_kind = defaultdict(Counter)
    outer_vals = defaultdict(list)
    local_vals = defaultdict(list)
    for w in code:
        kind = w & 0x3F
        a = (w >> 6) & 0xFF
        c = (w >> 14) & 0x1FF
        b = (w >> 23) & 0x1FF
        for role, v in (("A", a), ("B", b), ("C", c)):
            if role == "A":
                continue
            scope_by_kind[(kind, role)]["outer" if v & 0x100 else "local"] += 1
            (outer_vals if v & 0x100 else local_vals)[(kind, role)].append(v & 0xFF)
    print("=== %s  code=%d consts=%d ===" % (label, len(code), len(node["consts"])))
    print("%-16s %-4s %8s %8s  %s" % ("kind", "role", "local", "outer", "值域(local/outer)"))
    for (kind, role) in sorted(scope_by_kind):
        cnt = scope_by_kind[(kind, role)]
        lv = local_vals[(kind, role)]
        ov = outer_vals[(kind, role)]
        lspan = "%d..%d" % (min(lv), max(lv)) if lv else "-"
        ospan = "%d..%d" % (min(ov), max(ov)) if ov else "-"
        print("%-16s %-4s %8d %8d  L[%s] O[%s]" % (
            NAMES.get(kind, "0x%02X" % kind), role, cnt["local"], cnt["outer"], lspan, ospan))
    allov = sorted({v for (k, r), lst in outer_vals.items() for v in lst})
    alllv = sorted({v for (k, r), lst in local_vals.items() for v in lst})
    print("  外层下标 distinct=%d  范围 %s" % (
        len(allov), ("%d..%d" % (allov[0], allov[-1])) if allov else "-"))
    print("  局部下标 distinct=%d  范围 %s" % (
        len(alllv), ("%d..%d" % (alllv[0], alllv[-1])) if alllv else "-"))
    print("  常量数 = %d" % len(node["consts"]))
    return allov, alllv


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    root = bc.parse_chunk(bc.Reader(data, bc.HEADER_LEN))
    if len(argv) > 2:
        analyze(find(root, argv[2]), argv[2])
        return 0
    for p in ("root", "root/0", "root/0/13", "root/0/38/0/0"):
        try:
            analyze(find(root, p), p)
        except (IndexError, ValueError):
            pass
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
