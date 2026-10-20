#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在官方样本里定位指定记录种类，打印上下文（相邻记录 + 相关常量）。

用来人工核对「操作数角色」：看 B/C 指向的槽里装的是什么。

用法:
    python aardio_ctx.py <file> <kind1,kind2,...> [--all]
"""
from __future__ import annotations

import sys

import aardio_bytecode as bc
from aardio_objdump import NAMES, ctext, find


def walk(node, path, out):
    out.append((path, node))
    for i, o in enumerate(node["objects"]):
        walk(o, "%s/%d" % (path, i), out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    path = argv[1]
    kinds = {int(k, 0) for k in argv[2].split(",")}
    data = open(path, "rb").read()
    root = bc.parse_chunk(bc.Reader(data, bc.HEADER_LEN))
    out = []
    walk(root, "", out)
    for opath, node in out:
        code = node["code"]
        for pc, w in enumerate(code):
            kind = w & 0x3F
            if kind not in kinds:
                continue
            a = (w >> 6) & 0xFF
            c = (w >> 14) & 0x1FF
            b = (w >> 23) & 0x1FF
            print("=== %s  pc=%d  %s  A=%d B=%d%s C=%d%s" % (
                opath, pc, NAMES.get(kind, hex(kind)), a, b,
                "!" if b & 0x100 else "", c, "!" if c & 0x100 else ""))
            lo = max(0, pc - 6)
            hi = min(len(code), pc + 7)
            for q in range(lo, hi):
                w2 = code[q]
                k2 = w2 & 0x3F
                mark = ">>" if q == pc else "  "
                print("   %s %4d %-16s A=%-3d B=%-3d%s C=%-3d%s" % (
                    mark, q, NAMES.get(k2, hex(k2)), (w2 >> 6) & 0xFF,
                    (w2 >> 23) & 0x1FF, "!" if (w2 >> 23) & 0x100 else "",
                    (w2 >> 14) & 0x1FF, "!" if (w2 >> 14) & 0x100 else ""))
            # 关键常量
            for nm, idx in (("B", b), ("C", c)):
                if idx & 0x100:
                    k = idx & 0xFF
                    if k < len(node["consts"]):
                        print("     const[%s=%d] = %s" % (nm, k, ctext(node["consts"][k])))
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
