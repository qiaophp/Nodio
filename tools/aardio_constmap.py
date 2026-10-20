#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证假设：**常量池被加载进「外层/全局槽表」，下标与常量池下标一一对应**。

依据（docs/aardio-bytecode.md §7.2）：
    9 位操作数 = 低 8 位下标 + 最高位「外层作用域」标志。
    标志位为 1 时基址换成外层槽数组。

观测（aifix.aardio 反汇编 40..70）：
    SET_MEMBER3  A=3  B=15! C=16!     ; 两个操作数都带外层标志
    SET_MEMBER3  A=3  B=17! C=16!     ; 下标单调递增、连续

于是「键槽 / 对象槽」在外层作用域里取到的，很可能不是普通局部变量，
而是**常量池里的字符串**（aardio 的成员名就是字符串常量）。

本脚本对每个 chunk 对象统计：
    * 局部作用域下标的最大值（应远小于常量数）
    * 外层作用域下标的最大值（若 ≈ 常量数-1，假设成立）
    * 外层下标是否覆盖 0..N-1 的连续区间（常量池被完整寻址的迹象）

用法:
    python aardio_constmap.py <file> [<file> ...]
"""
from __future__ import annotations

import sys
from collections import defaultdict

import aardio_bytecode as bc

KIND_MASK = 0x3F
C_SHIFT, B_SHIFT = 14, 23
GLOBAL_BIT = 0x100

JUMPS = {0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E}


def walk(node, path="root", out=None):
    out.append((path, node))
    for i, o in enumerate(node["objects"]):
        walk(o, "%s/%d" % (path, i), out)
    return out


def scan(code):
    local = set()
    outer = set()
    per_kind_outer = defaultdict(set)
    for word in code:
        kind = word & KIND_MASK
        if kind in JUMPS:
            continue
        c = (word >> C_SHIFT) & 0x1FF
        b = (word >> B_SHIFT) & 0x1FF
        for v in (c, b):
            if v & GLOBAL_BIT:
                outer.add(v & 0xFF)
            else:
                local.add(v & 0xFF)
        for v in (c, b):
            if v & GLOBAL_BIT:
                per_kind_outer[kind].add(v & 0xFF)
    return local, outer, per_kind_outer


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        data = open(path, "rb").read()
        if not data.startswith(bc.MAGIC):
            print("%s: not an =aardio file" % path)
            continue
        root = bc.parse_chunk(bc.Reader(data, bc.HEADER_LEN))
        objs = walk(root, out=[])
        print("=" * 78)
        print("FILE %s" % path)
        print("=" * 78)
        print("%-14s %6s %6s %8s %8s %8s %8s %7s %7s" % (
            "object", "code", "const", "maxLoc", "maxOut", "outCnt", "consts", "out/const", "dense"))
        rows = []
        for p, node in objs:
            local, outer, _ = scan(node["code"])
            nc = len(node["consts"])
            ml = max(local) if local else -1
            mo = max(outer) if outer else -1
            oc = len(outer)
            ratio = (oc / nc) if nc else 0.0
            dense = ""
            if outer and nc:
                covered = len([i for i in range(nc) if i in outer])
                dense = "%.0f%%" % (100.0 * covered / nc)
            print("%-14s %6d %6d %8d %8d %8d %8d %7.2f %7s" % (
                p, len(node["code"]), nc, ml, mo, oc, nc, ratio, dense))
            rows.append((p, nc, mo, oc, ml))

        print()
        print("-- 汇总 --")
        good = [r for r in rows if r[1] > 0 and r[2] >= 0]
        if good:
            fit = sum(1 for r in good if r[2] >= r[1] * 0.5)
            print("  有常量的对象数: %d" % len(good))
            print("  其中「最大外层下标 ≥ 常量数/2」的: %d  (%.0f%%)" % (
                fit, 100.0 * fit / len(good)))
            print("  最大外层下标 vs 常量数 (前 12 个对象):")
            for p, nc, mo, oc, ml in good[:12]:
                print("    %-14s consts=%-5d maxOuter=%-5d maxLocal=%-5d outerCnt=%d" % (
                    p, nc, mo, ml, oc))
        # 全局：外层下标是否跨对象共享同一张表
        allouter = set()
        for p, node in objs:
            _, outer, _ = scan(node["code"])
            allouter |= outer
        print()
        print("  全部对象合起来用到的外层下标: %d 个 (max=%d)" % (
            len(allouter), max(allouter) if allouter else -1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
