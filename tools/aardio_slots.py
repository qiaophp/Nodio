#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aardio '=aardio' 预编译块：槽位 / 常量池寻址关系分析器。

目的（对应 docs/aardio-bytecode.md §7.8 的两个未决点）：

  1. **常量池寻址约定** —— 2173 个常量在记录流里是怎么被引用的？
     记录的操作数 C / B 段既可能是「槽下标」也可能是「常量下标」，
     需要用数据判断：常量到底是**混在同一片槽里**（常量槽在低位），
     还是**单独的一段**（由某条记录单独 load）。
  2. **槽位规模** —— attr1 / attr2 到底是什么（局部槽数？参数槽数？行号？）
     它们与记录里出现过的最大槽下标是什么关系。

用法:
    python aardio_slots.py <file> [<file> ...]
"""
from __future__ import annotations

import sys
from collections import Counter, defaultdict

import aardio_bytecode as bc

# 运行期位布局（docs/aardio-bytecode.md §7.2）
KIND_MASK = 0x3F
A_SHIFT = 6
A_MASK = 0xFF
C_SHIFT = 14
C_MASK = 0x1FF
B_SHIFT = 23
B_MASK = 0x1FF
GLOBAL_BIT = 0x100

# 跳转类：位移取自本条或下一条 u32
JUMP_OWN = {0x22, 0x2D, 0x2E}
JUMP_NEXT_1FFFE = {0x23, 0x24, 0x25, 0x26}
JUMP_NEXT_1FFFD = {0x27, 0x28}
JUMPS = JUMP_OWN | JUMP_NEXT_1FFFE | JUMP_NEXT_1FFFD


def decode(word: int):
    kind = word & KIND_MASK
    a = (word >> A_SHIFT) & A_MASK
    c = (word >> C_SHIFT) & C_MASK
    b = (word >> B_SHIFT) & B_MASK
    return kind, a, b, c


def const_text(c):
    t = c["type"]
    if t == bc.CONST_STRING:
        return bc.s_text(c["string"])
    if t == bc.CONST_NUMBER:
        return repr(c["value"])
    if t == bc.CONST_BOOL:
        return repr(bool(c["value"]))
    return "null"


def walk(node, depth=0, path="root", out=None):
    out.append((depth, path, node))
    for i, o in enumerate(node["objects"]):
        walk(o, depth + 1, "%s/%d" % (path, i), out)
    return out


def analyze_object(node):
    code = node["code"]
    consts = node["consts"]
    nconst = len(consts)

    kinds = Counter()
    slot_max = {"A": -1, "B": -1, "C": -1}
    slot_vals = defaultdict(Counter)
    operand_vals = defaultdict(Counter)   # (kind, role) -> Counter(value)
    globals_seen = Counter()

    for pc, word in enumerate(code):
        kind, a, b, c = decode(word)
        kinds[kind] += 1
        slot_vals["A"][a] += 1
        slot_vals["B"][b] += 1
        slot_vals["C"][c] += 1
        if kind in JUMPS:
            continue
        slot_max["A"] = max(slot_max["A"], a)
        slot_max["B"] = max(slot_max["B"], b & ~GLOBAL_BIT)
        slot_max["C"] = max(slot_max["C"], c & ~GLOBAL_BIT)
        if b & GLOBAL_BIT:
            globals_seen["B"] += 1
        if c & GLOBAL_BIT:
            globals_seen["C"] += 1
        operand_vals[(kind, "A")][a] += 1
        operand_vals[(kind, "B")][b] += 1
        operand_vals[(kind, "C")][c] += 1

    return {
        "code": len(code),
        "consts": nconst,
        "kindcount": len(kinds),
        "kinds": kinds,
        "slot_max": slot_max,
        "slot_vals": slot_vals,
        "operand_vals": operand_vals,
        "globals": globals_seen,
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        data = open(path, "rb").read()
        if not data.startswith(bc.MAGIC):
            print("%s: not an =aardio file" % path)
            continue
        r = bc.Reader(data, bc.HEADER_LEN)
        root = bc.parse_chunk(r)
        objs = walk(root, out=[])
        print("=" * 78)
        print("FILE %s  (%d bytes)" % (path, len(data)))
        print("=" * 78)
        print("%-14s %6s %6s %6s %6s %6s %6s  %s" % (
            "object", "code", "const", "tA", "tB", "tC", "maxA", "attr1/attr2/flags"))
        for depth, p, node in objs:
            st = analyze_object(node)
            print("%-14s %6d %6d %6d %6d %6d %6d  %d/%d/%s" % (
                p, st["code"], st["consts"], len(node["tableA"]), len(node["tableB"]),
                len(node["tableC"]), st["slot_max"]["A"],
                node["attr1"], node["attr2"],
                ",".join("%02x" % f for f in node["flags"])))

        # --- 假设检验 -------------------------------------------------------
        print()
        print("-- 槽位下标分布（根对象，按 role 汇总前 12 大） --")
        st = analyze_object(root)
        for role in ("A", "B", "C"):
            top = st["slot_vals"][role].most_common(12)
            print("  %s: %s" % (role, " ".join("%d×%d" % (v, n) for v, n in top)))
        print("  全局作用域标志命中: %s" % dict(st["globals"]))
        print("  出现过的种类: %d" % st["kindcount"])
        print("  种类直方图: %s" % ", ".join(
            "%02X:%d" % (k, st["kinds"][k]) for k in sorted(st["kinds"])))

        # H1: attr1/attr2 与最大槽下标的关系
        print()
        print("-- attr1/attr2 假设 --")
        for depth, p, node in objs:
            s = analyze_object(node)
            print("  %-12s attr1=%-4d attr2=%-4d consts=%-4d maxA=%-4d maxB=%-4d maxC=%-4d" % (
                p, node["attr1"], node["attr2"], s["consts"],
                s["slot_max"]["A"], s["slot_max"]["B"], s["slot_max"]["C"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
