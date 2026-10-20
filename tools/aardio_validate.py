#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""官方 '=aardio' 预编译块校验器的等价实现（对照 aardio.exe FUN_004D4450）。

第十三轮定位到官方的三张关键表 / 规则：

  * **描述符表 DAT_007C80A0**（每 kind 1 字节）：
        bits 0..1  = 操作数类别  0=三操作数 1=单18位 2=单18位有符号
        bits 4..5  = 语义标记    0x10/0x20/0x30
        bit  6     = 可作跳转落点
        bit  7     = 负号位：该记录后必须紧跟一条 0x22
  * **9 位操作数**（C=bits14..22，B=bits23..31）的寻址由 FUN_004D4400 判定：
        bit8 == 0 → **局部槽下标**，必须 < 槽位数（chunk 帧 +0x4B）
        bit8 == 1 → **常量表下标**，必须 < 常量数（chunk 帧 +0x28）
  * **18 位操作数**（bits14..31）出现在描述符类别 1 的记录上：
        kind 0x00/0x05/0x07/0x0C/0x0D → **常量表下标**（0x05/0x07/0x0C/0x0D 还要求是字符串常量）
        kind 0x34 → **嵌套对象下标**

chunk 帧（0x50 字节）关键偏移：
        +0x08 常量表指针（16 字节/项）   +0x28 常量数
        +0x0C code 指针                 +0x2C code 条数
        +0x10 嵌套对象表指针            +0x34 对象数
        +0x48 upvalue 数上限            +0x4A / +0x4B / +0x4C 标志字节
        +0x4B 槽位数（局部槽表大小）

用法:
    python aardio_validate.py <file> [<file> ...]
"""
from __future__ import annotations

import struct
import sys

import aardio_bytecode as bc

# ---- 描述符表 DAT_007C80A0（自 aardio.exe 内存导出，64 字节）----
DESCRIPTOR = bytes((
    0x71, 0x54, 0x60, 0x50, 0x60, 0x71, 0x41, 0x71,
    0x60, 0x6C, 0x6C, 0x6C, 0x31, 0x31, 0x10, 0x20,
    0x30, 0x3C, 0x3C, 0x3C, 0x3C, 0x54, 0x38, 0x6C,
    0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x60, 0x60,
    0x60, 0x68, 0x22, 0xBC, 0xBC, 0xBC, 0xBC, 0xE4,
    0xE4, 0x28, 0x54, 0x54, 0x10, 0x62, 0x62, 0x84,
    0x00, 0x84, 0x14, 0x00, 0x51, 0x70, 0x7C, 0x7C,
    0x7C, 0x7C, 0x7C, 0x3B, 0x7C, 0x50, 0x00, 0x00,
))

# class-1（单 18 位操作数）里，操作数是常量下标的 kind
CONST_INDEX_KINDS = {0x00, 0x05, 0x07, 0x0C, 0x0D}
# 其中要求必须是字符串常量（type 4）的 kind
STRING_KEY_KINDS = {0x05, 0x07, 0x0C, 0x0D}
# class-1 里操作数是嵌套对象下标的 kind
OBJECT_INDEX_KINDS = {0x34}
# 跳转类（描述符类别 2 = 单 18 位有符号）
JUMP_KINDS = {0x22, 0x2D, 0x2E}
# 条件跳转：位移取自下一条 u32，且下一条必须是 0x22
COND_JUMP_KINDS = {0x23, 0x24, 0x25, 0x26, 0x27, 0x28}

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


class Ctx(object):
    """chunk 的运行期上下文（只保留校验器用得到的字段）。"""

    def __init__(self, node, path, parent=None):
        self.node = node
        self.path = path
        self.parent = parent
        self.code = node["code"]
        self.consts = node["consts"]
        self.objects = node["objects"]
        self.flags = node["flags"]
        self.nconst = len(self.consts)
        self.slots = self.flags[3]        # frame+0x4B 槽位数
        self.upvalue_limit = self.flags[0]  # frame+0x48 upvalue 数上限
        self.ncode = len(self.code)
        self.errors = []

    def err(self, pc, msg):
        self.errors.append((self.path, pc, msg))


def const_type(ctx, idx):
    if idx < 0 or idx >= ctx.nconst:
        return None
    return ctx.consts[idx]["type"]


def check_operand9(ctx, pc, kind, role, val, cat):
    """9 位操作数，按官方 FUN_004D4400 的类别语义检查。

    类别（来自描述符字节 bits2..3 / bits4..5）：
        0 → 操作数必须为 0
        1 → 不检查（立即数 / 内建序号 / 计数等）
        2 → 局部槽下标，必须 < 槽位数（chunk 帧 +0x4B）
        3 → bit8 ? 常量表下标(< 常量数) : 局部槽下标(< 槽位数)
    """
    if cat == 0:
        if val != 0:
            ctx.err(pc, "%s: 操作数%s 必须为 0，实为 %d"
                    % (NAMES.get(kind, "0x%02X" % kind), role, val))
        return
    if cat == 1:
        return
    if cat == 2:
        if val >= ctx.slots:
            ctx.err(pc, "%s: 操作数%s 局部槽下标 %d 越界 (槽位数 %d)"
                    % (NAMES.get(kind, "0x%02X" % kind), role, val, ctx.slots))
        return
    # cat == 3
    if val & 0x100:
        idx = val & 0xFF
        if idx >= ctx.nconst:
            ctx.err(pc, "%s: 操作数%s 常量下标 %d 越界 (常量数 %d)"
                    % (NAMES.get(kind, "0x%02X" % kind), role, idx, ctx.nconst))
    else:
        if val >= ctx.slots:
            ctx.err(pc, "%s: 操作数%s 局部槽下标 %d 越界 (槽位数 %d)"
                    % (NAMES.get(kind, "0x%02X" % kind), role, val, ctx.slots))


def check_operand18(ctx, pc, kind, val, cat):
    """18 位操作数（描述符类别 1 的记录）。"""
    if cat == 0:
        if val != 0:
            ctx.err(pc, "%s: 18 位操作数必须为 0，实为 %d"
                    % (NAMES.get(kind, "0x%02X" % kind), val))
    elif cat == 2:
        if val >= ctx.slots:
            ctx.err(pc, "%s: 18 位操作数 %d 越界 (槽位数 %d)"
                    % (NAMES.get(kind, "0x%02X" % kind), val, ctx.slots))
    elif cat == 3:
        if val >= ctx.nconst:
            ctx.err(pc, "%s: 常量下标 %d 越界 (常量数 %d)"
                    % (NAMES.get(kind,), val, ctx.nconst))
        elif kind in STRING_KEY_KINDS and const_type(ctx, val) != 4:
            ctx.err(pc, "%s: 常量下标 %d 不是字符串常量 (type=%s)"
                    % (NAMES.get(kind,), val, const_type(ctx, val)))


def check_object(ctx, pc, kind):
    errors_before = len(ctx.errors)
    code = ctx.code
    n = len(code)
    for pc2, word in enumerate(code):
        k = word & 0x3F
        if k > 0x3C:
            ctx.err(pc2, "非法记录种类 0x%02X" % k)
            continue
        desc = DESCRIPTOR[k]
        cls = desc & 3
        A = (word >> 6) & 0xFF
        if ctx.slots <= A:
            ctx.err(pc2, "%s: 目标槽 A=%d 越界 (槽位数 %d)"
                    % (NAMES.get(k, "0x%02X" % k), A, ctx.slots))
        if cls == 0:
            C = (word >> 14) & 0x1FF
            B = (word >> 23) & 0x1FF
            check_operand9(ctx, pc2, k, "C", C, (desc >> 2) & 3)
            check_operand9(ctx, pc2, k, "B", B, (desc >> 4) & 3)
        elif cls == 1:
            op = word >> 14          # 18 位
            check_operand18(ctx, pc2, k, op, (desc >> 4) & 3)
            if k in OBJECT_INDEX_KINDS:
                if op >= len(ctx.objects):
                    ctx.err(pc2, "%s: 嵌套对象下标 %d 越界 (对象数 %d)"
                            % (NAMES.get(k,), op, len(ctx.objects)))
                else:
                    # 官方：NEW_CLOSURE 后面紧跟 N 条 kind 3/4 的上值记录
                    up = ctx.objects[op]["flags"][0]
                    for i in range(1, up + 1):
                        if pc2 + i >= n:
                            ctx.err(pc2, "%s: 上值记录越界" % NAMES.get(k,))
                            break
                        nk = code[pc2 + i] & 0x3F
                        if nk not in (0x03, 0x04):
                            ctx.err(pc2 + i, "%s: 上值记录应为 LOAD_UPVALUE/LOAD_LOCAL，实为 %s"
                                    % (NAMES.get(k,), NAMES.get(nk, "0x%02X" % nk)))
                            break
        elif cls == 2:
            rel = (word >> 14) - 0x1FFFF      # 有符号 18 位
            target = pc2 + rel + 1
            if target < 0 or target >= n:
                ctx.err(pc2, "%s: 跳转落点 %d 越界 (记录数 %d)"
                        % (NAMES.get(k,), target, n))
        # bit6：可作断点落点（不影响结构）
        # bit7（符号位）：该记录后必须紧跟一条 0x22
        if desc & 0x80:
            if pc2 + 2 > n or (code[pc2 + 1] & 0x3F) != 0x22:
                ctx.err(pc2, "%s: 后面必须紧跟一条 JMP(0x22)" % NAMES.get(k,))
        # 条件跳转：位移取自下一条 u32
        if k in COND_JUMP_KINDS:
            if pc2 + 1 >= n:
                ctx.err(pc2, "%s: 缺少后继的 JMP(0x22)" % NAMES.get(k,))
            elif (code[pc2 + 1] & 0x3F) != 0x22:
                ctx.err(pc2, "%s: 后继不是 JMP(0x22)" % NAMES.get(k,))
            else:
                nxt = code[pc2 + 1]
                rel = (nxt >> 14) - 0x1FFFE
                target = pc2 + rel
                if target < 0 or target >= n:
                    ctx.err(pc2, "%s: 跳转落点 %d 越界" % (NAMES.get(k,), target))
    return len(ctx.errors) - errors_before


def walk(node, path, out):
    ctx = Ctx(node, path)
    check_object(ctx, 0, 0)
    out.extend(ctx.errors)
    for i, o in enumerate(node["objects"]):
        walk(o, "%s/%d" % (path, i), out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rc = 0
    for path in argv[1:]:
        data = open(path, "rb").read()
        if not data.startswith(bc.MAGIC):
            print("%s: not an =aardio file" % path)
            continue
        root = bc.parse_chunk(bc.Reader(data, bc.HEADER_LEN))
        errs = []
        walk(root, "root", errs)
        print("=" * 70)
        print("%s  -> %s" % (path, "OK" if not errs else "%d 处违规" % len(errs)))
        for p, pc, msg in errs[:40]:
            print("   %-12s pc=%-6d %s" % (p, pc, msg))
        if len(errs) > 40:
            print("   ... 还有 %d 处" % (len(errs) - 40))
        if errs:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
