#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐对象对照：记录流 ↔ 常量池。

对每个 chunk 对象打印：
    * 记录流（按运行期位布局解码）
    * 常量池（按池序）
用来肉眼确认「常量是怎么被记录引用的」。

用法:
    python aardio_objdump.py <file> [对象路径]
    例：python aardio_objdump.py aifix.aardio root/38/0/0
"""
from __future__ import annotations

import sys

import aardio_bytecode as bc

NAMES = {
    0x00: "LOAD_OUTER", 0x01: "LOAD_INT", 0x02: "CLEAR_LOCALS", 0x03: "LOAD_UPVALUE",
    0x04: "LOAD_LOCAL", 0x05: "GET_MEMBER", 0x06: "LOAD_SELF_NS", 0x07: "GET_MEMBER2",
    0x08: "GET_GLOBAL", 0x09: "GET_MEMBER3", 0x0A: "GET_MEMBER4", 0x0B: "INDEX",
    0x0C: "SET_MEMBER", 0x0D: "SET_MEMBER2", 0x0E: "STORE_UPVALUE", 0x0F: "LOAD_IF_SET",
    0x10: "SET_METATABLE", 0x11: "SET_MEMBER3", 0x12: "CONCAT_MEMBER", 0x13: "RAWSET",
    0x14: "SET_MEMBER4", 0x15: "NEW_NATIVE", 0x16: "IMPORT_MEMBER", 0x17: "METHOD_PREP",
    0x18: "ADD", 0x19: "SUB", 0x1A: "MUL", 0x1B: "DIV", 0x1C: "MOD", 0x1D: "POW",
    0x1E: "NEG", 0x1F: "TRUTHY", 0x20: "LENGTH", 0x21: "CONCAT", 0x22: "JMP",
    0x23: "JZ", 0x24: "JE", 0x25: "JG", 0x26: "JLE", 0x27: "JTRUE", 0x28: "JTRUE2",
    0x29: "TAILCALL", 0x2A: "CALL", 0x2B: "CALL_NATIVE", 0x2C: "RET", 0x2D: "ADDEQ",
    0x2E: "SUBEQ", 0x2F: "CALL_MULTI", 0x30: "CALL_VALUE", 0x31: "CALL_NS",
    0x32: "BUILD_ARRAY", 0x33: "GC", 0x34: "NEW_CLOSURE", 0x35: "BITNOT",
    0x36: "BITAND", 0x37: "BITOR", 0x38: "BITXOR", 0x39: "SHL", 0x3A: "SHR",
    0x3B: "USHR", 0x3C: "CALL_N",
}
JUMP_OWN = {0x22, 0x2D, 0x2E}
JUMP_NEXT = {0x23, 0x24, 0x25, 0x26, 0x27, 0x28}


def ctext(c):
    t = c["type"]
    if t == bc.CONST_STRING:
        return bc.s_text(c["string"])
    if t == bc.CONST_NUMBER:
        return repr(c["value"])
    if t == bc.CONST_BOOL:
        return repr(bool(c["value"]))
    return "null"


def find(root, path):
    node = root
    if path in ("root", ""):
        return node
    for part in path.split("/"):
        if part == "root":
            continue
        node = node["objects"][int(part)]
    return node


def dump(node, path=""):
    code = node["code"]
    print("--- object %s  code=%d consts=%d attr1=%d attr2=%d flags=%s" % (
        path or "root", len(code), len(node["consts"]), node["attr1"], node["attr2"],
        ",".join("%02x" % f for f in node["flags"])))
    for pc, w in enumerate(code):
        kind = w & 0x3F
        a = (w >> 6) & 0xFF
        c = (w >> 14) & 0x1FF
        b = (w >> 23) & 0x1FF
        cg = "!" if c & 0x100 else ""
        bg = "!" if b & 0x100 else ""
        extra = ""
        if kind in JUMP_OWN:
            rel = (w >> 14) & 0x3FFFF
            extra = " rel18=%d -> %d" % (rel - 0x1FFFE, pc + (rel - 0x1FFFE))
        elif kind in JUMP_NEXT and pc + 1 < len(code):
            nxt = code[pc + 1]
            rel = (nxt >> 14) & 0x3FFFF
            extra = " rel18(next)=%d -> %d" % (rel - 0x1FFFE, pc + (rel - 0x1FFFE))
        print("  %4d  %08X  %-16s A=%-3d B=%-3d%s C=%-3d%s%s" % (
            pc, w, NAMES.get(kind, "?%02X" % kind), a, b, bg, c, cg, extra))
    print("  常量池:")
    for i, c in enumerate(node["consts"]):
        print("    [%3d] type=%d %s" % (i, c["type"], ctext(c)))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    data = open(path, "rb").read()
    root = bc.parse_chunk(bc.Reader(data, bc.HEADER_LEN))
    if len(argv) > 2:
        dump(find(root, argv[2]), argv[2])
        return 0
    # 默认：列出所有对象摘要
    def walk(n, p):
        print("%-14s code=%-6d consts=%-5d attr1=%-5d attr2=%-5d" % (
            p or "root", len(n["code"]), len(n["consts"]), n["attr1"], n["attr2"]))
        for i, o in enumerate(n["objects"]):
            walk(o, "%s/%d" % (p, i))
    walk(root, "")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
