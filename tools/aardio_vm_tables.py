#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aardio.exe internal VM tables recovered by REA + Ghidra 12.1.4 headless.

Every constant in this file is a byte-exact dump of aardio.exe (image base
0x00400000) or a fact taken from a decompiled function of the same binary.
See docs/aardio-bytecode.md for the evidence trail and the reproduction steps
(tools/ghidra/*.java).

Three tables live here:

1. NODE_PRIORITY — the expression-node priority table at image VA 0x007C80E0.
   26 entries of 2 bytes: (priority, child-priority).

2. BUILTIN_TABLE — the official builtin dispatch table at image VA 0x007C8118.
   Alternating (name pointer, handler) pairs; the handler is a native function
   inside aardio.exe.

3. OPCODE_SEMANTICS / OPCODE_CONSTRAINTS — what the official code says about
   individual opcodes (the debugger's variable resolver FUN_004D4940 and the
   chunk validator FUN_004D4450).

Usage:
    python aardio_vm_tables.py            # print every table
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aardio_vm_runtime as rt  # noqa: E402

# ---------------------------------------------------------------- tables ----

# Image VA 0x007C80E0, 52 bytes = 26 entries x 2 bytes.
NODE_PRIORITY_BYTES = bytes((
    0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x16, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x19, 0x18,
    0x13, 0x12, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C,
    0x0B, 0x0B, 0x00, 0x00,
))

# (priority, child-priority) per node kind 0..25.
# kind 0x19 (25) is the end marker: FUN_004EE510 stops when the node kind
# equals 0x19, so its table slot is unused (0).
NODE_PRIORITY = tuple(
    (NODE_PRIORITY_BYTES[i * 2], NODE_PRIORITY_BYTES[i * 2 + 1]) for i in range(26)
)

NODE_KIND_COUNT = 26
NODE_END_MARKER = 0x19

# Image VA 0x007C8118 — official builtin dispatch table (name -> handler).
BUILTIN_TABLE = {
    "lshift":     0x004E6260,
    "rshift":     0x004E62E0,
    "lessThan":   0x004E69B0,
    "greaterThan": 0x004E6AC0,
    "split32":    0x004E63D0,
    "_tonumber":  0x004E6360,
    "_tostring":  0x004E6480,
    "_serialize": 0x004E6960,
    "reset":      0x004E67D0,
    "format":     0x004E68D0,
    "size64":     0x004E6BD0,
    "floor":      0x004E4EC0,
    "round":      0x004E4F10,
    "frexp":      0x004E5210,
    "ldexp":      0x004E5270,
    "log10":      0x004E5110,
    "clamp":      0x004E52C0,
    "random":     0x004E5960,
    "randomize":  0x004E5AA0,
    "validateRange": 0x004E6C00,
    "isArrayLike": 0x004E4AA0,
    "isArray":    0x004E4A40,
    "tostring":   0x004E44D0,
    "concat":     0x004E32B0,
    "append":     0x004E3020,
    "insert":     0x004E1350,
    "remove":     0x004E1D80,
    "splice":     0x004E15E0,
    "unique":     0x004E2AF0,
    "reverse":    0x004E3670,
    "range":      0x004E0D00,
    "unpack":     0x004E2F50,
    "right":      0x004E2FF0,
    "clone":      0x004E4B10,
    "slice":      0x004E4600,
    "getAttr":    0x004E48C0,
    "setAttr":    0x004E4980,
    "isStruct":   0x004DE720,
    "stamp":      0x004DE840,
}

# 由 tools/ghidra/EmitSites.java 从 aardio.exe 实测导出：
# 每个 kind 对应的「记录发射点」及其所属函数（83 个调用点，其中 10 个 kind 为常量）。
RECORD_EMIT_SITES = {
    0x06: {
        "FUN_004f45e0",
    },
    0x08: {
        "FUN_004ea360",
        "FUN_004eb640",
        "FUN_004ec550",
        "FUN_004ec700",
        "FUN_004ecdf0",
        "FUN_004eeb00",
        "FUN_004eece0",
        "FUN_004eeda0",
        "FUN_004ef1e0",
        "FUN_004ef390",
        "FUN_004efb20",
        "FUN_004f00f0",
        "FUN_004f09b0",
        "FUN_004f0d60",
        "FUN_004f1370",
        "FUN_004f1850",
        "FUN_004f1c70",
        "FUN_004f21f0",
        "FUN_004f2680",
        "FUN_004f3cc0",
        "FUN_004f3da0",
        "FUN_004f3dd0",
        "FUN_004f4730",
        "FUN_004f4770",
        "FUN_004f4900",
        "FUN_004f4ca0",
        "FUN_004f4d30",
        "FUN_004f5000",
        "FUN_004f5250",
        "FUN_004f5560",
        "FUN_004f5bd0",
        "FUN_004f5c00",
        "FUN_004f5c30",
    },
    0x0C: {
        "FUN_004f4d30",
    },
    0x20: {
        "FUN_004ebe80",
        "FUN_004ec700",
    },
    0x22: {
        "FUN_004efb20",
        "FUN_004f1000",
        "FUN_004f21f0",
        "FUN_004f3d60",
        "FUN_004f3dd0",
        "FUN_004f4900",
        "FUN_004f5000",
        "FUN_004f5660",
    },
    0x24: {
        "FUN_004ef390",
    },
    0x2C: {
        "FUN_004ea660",
        "FUN_004ecdf0",
    },
    0x34: {
        "FUN_004ef1e0",
    },
    0x38: {
        "FUN_004f0f50",
    },
    0x3C: {
        "FUN_004edeb0",
    },
}

# 每个 kind 的发射点数量（含动态 kind 的调用点）
RECORD_EMIT_COUNTS = {
    0x06: 1,
    0x08: 50,
    0x0C: 1,
    0x20: 2,
    0x22: 9,
    0x24: 1,
    0x2C: 2,
    0x34: 1,
    0x38: 1,
    0x3C: 1,
}

# Image VA 0x007C80A0 — 记录种类的「操作数格式描述符」表。
# 每个记录种类 1 字节；官方 chunk 校验器 FUN_004D4450 用它决定怎么取操作数：
#   b & 3      == 0 -> 双操作数：A = bits14..22（9 位）、B = bits23..31（9 位）
#   b & 3      == 1 -> 单操作数：bits14..31（18 位，无符号）
#   b & 3      == 2 -> 单操作数：bits14..31（18 位）再减 0x1FFFF（有符号）
#   b & 0x30   == 0x30 -> 该操作数是**槽位索引**，必须 < chunk 帧槽位上限
#                          （校验器读 param_1+0x28）
#   b & 0x30   == 0x20 -> 该操作数是**相对位移**（相对当前记录位置）
#   b & 0x40   != 0    -> 该记录可以作为控制流跳转目标
#   b & 0x80   != 0    -> 该记录**必须**紧跟一条 0x22（跳转）记录
# 校验器 `if (0x3c < uVar6) return 0;` —— 合法种类只有 0x00..0x3C（61 种），
# 0x3D..0x3F 的槽位是 0 且被直接拒绝。
OPERAND_DESC_BYTES = bytes((
    0x71, 0x54, 0x60, 0x50, 0x60, 0x71, 0x41, 0x71,
    0x60, 0x6C, 0x6C, 0x6C, 0x31, 0x31, 0x10, 0x20,
    0x30, 0x3C, 0x3C, 0x3C, 0x3C, 0x54, 0x38, 0x6C,
    0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x60, 0x60,
    0x60, 0x68, 0x22, 0xBC, 0xBC, 0xBC, 0xBC, 0xE4,
    0xE4, 0x28, 0x54, 0x54, 0x10, 0x62, 0x62, 0x84,
    0x00, 0x84, 0x14, 0x00, 0x51, 0x70, 0x7C, 0x7C,
    0x7C, 0x7C, 0x7C, 0x7C, 0x50, 0x00, 0x00, 0x00,
))

RECORD_KIND_COUNT = 0x3D          # 合法记录种类 0x00..0x3C（61 种）
RECORD_KIND_FIELD_BITS = 6        # 记录字 bits0..5 = 种类

_OPERAND_FORMATS = {
    0: "双操作数 A=bits14-22 / B=bits23-31",
    1: "单操作数 bits14-31（无符号）",
    2: "单操作数 bits14-31-0x1FFFF（有符号）",
}


def decode_operand_desc(kind: int) -> dict:
    """Decode one entry of the official operand-format descriptor table."""
    if kind < 0 or kind >= len(OPERAND_DESC_BYTES):
        raise ValueError("record kind out of range: %d" % kind)
    b = OPERAND_DESC_BYTES[kind]
    out = {
        "kind": kind,
        "byte": b,
        "valid": kind < RECORD_KIND_COUNT,
        "format": _OPERAND_FORMATS.get(b & 3, "未定义(b&3==3)"),
        "slot_bound": (b & 0x30) == 0x30,
        "relative": (b & 0x30) == 0x20,
        "branch_target": (b & 0x40) != 0,
        "must_follow_jump": (b & 0x80) != 0,
    }
    return out


def record_kind_flags(kind: int) -> str:
    """Human readable flag summary for one record kind."""
    d = decode_operand_desc(kind)
    if not d["valid"]:
        return "非法种类（校验器直接拒绝）"
    parts = [d["format"]]
    if d["slot_bound"]:
        parts.append("槽位索引")
    if d["relative"]:
        parts.append("相对位移")
    if d["branch_target"]:
        parts.append("可作跳转目标")
    if d["must_follow_jump"]:
        parts.append("必须紧跟 0x22")
    return " / ".join(parts)


# 官方预编译 chunk 的**运行时帧布局**（第十轮定位）。
# 官方加载器 FUN_004e93a0 分配一个 0x50 字节的帧对象，随后依次：
#   FUN_004e8880(frame) -> 读 code 数组     （+0x0c 指针 / +0x2c 条数）
#   FUN_004e8960(frame) -> 读常量池 + 嵌套对象
#   FUN_004e8df0(frame) -> 读三个并行数据数组
#   FUN_004d4450(frame, frame[0x0b], 0xff) -> 校验记录流
# 偏移与语义都能与官方校验器 FUN_004d4450 对上（它直接读 +0x0c/+0x2c/+0x28/
# +0x08/+0x48/+0x4a/+0x4b）。
CHUNK_FRAME_SIZE = 0x50

CHUNK_FRAME_LAYOUT = {
    0x08: "命名空间成员表指针（校验器按 +8+uVar9*0x10 取表项，表项类型必须为 4）",
    0x0C: "code 数组指针（记录流；校验器按此逐条取记录）",
    0x10: "模块/堆上下文（分配器用）",
    0x14: "tableA 指针（u32Block，字面量数组）",
    0x18: "tableB 指针（recordBlock，每项 12 字节 = name + i32 + i32）",
    0x1C: "tableC 指针（strBlock，字符串常量数组）",
    0x24: "tableC 条数",
    0x28: "帧槽位上限（校验器用它约束槽位索引操作数）",
    0x2C: "code 条数（= frame[0x0b]，校验器的 param_2）",
    0x30: "tableA 条数",
    0x34: "嵌套深度计数（ushort，>200 报 chunk has too many syntax levels）",
    0x38: "tableB 条数",
    0x48: "标志字节 0（校验器：操作数相对该值的越界判定）",
    0x49: "标志字节 1",
    0x4A: "标志字节 2（校验器：0x3C 记录要求 &2 置位、&4 清零）",
    0x4B: "标志字节 3（校验器：槽位总数 / 命名空间表大小上界）",
}

# 官方 chunk 加载器的四个步骤（第十轮反编译确认）
CHUNK_LOADER_STEPS = {
    0x004E8880: "读 code 数组：u32 条数 + count×u32（记录流）",
    0x004E8960: "读常量池（u32 条数 + 逐个 {type,载荷}）与嵌套对象块（递归 chunk）",
    0x004E8DF0: "读三个并行数据数组：tableA(u32) / tableB(12 字节记录) / tableC(字符串)",
    0x004D4450: "校验记录流：按 DAT_007C80A0 描述符表逐条取操作数并检查约束",
}

# Opcode semantics recovered so far.
#   source: FUN_004D4940 (official debugger variable resolver) unless noted.
OPCODE_SEMANTICS = {
    0x03: "读取外层变量 (upvalue)",
    0x04: "读取局部变量 (local)",
    0x05: "取 self / 命名空间成员（按操作数 B 索引）",
    0x06: "取 self / 命名空间（固定表项）",
    0x07: "取全局命名空间成员（按操作数 B 索引）",
    0x09: "取表字段 (field)",
    0x17: "取表方法 (method)",
    0x22: "跳转记录（唯一的有符号相对位移形态；描述符 0x22 = 格式类 2 + 相对位移位）",
    0x28: "条件跳转（描述符 0xE4：双操作数 + 相对位移位 + 可作跳转目标 + 必须紧跟 0x22）",
    0x32: "指令流标记（校验器按 C==0 特殊处理）",
}

# 按描述符字节归并的「记录形态」分组：同一形态 = 同样的操作数布局 + 同样的
# 标志组合（可作跳转目标 / 相对位移 / 槽位索引 / 必须紧跟 0x22）。
# 61 个有效种类共归并为 21 种形态。
RECORD_SHAPE_GROUPS = {
    0x00: [0x30, 0x33],
    0x10: [0x0E, 0x2C],
    0x14: [0x32],
    0x20: [0x0F],
    0x22: [0x22],
    0x28: [0x29],
    0x30: [0x10],
    0x31: [0x0C, 0x0D],
    0x38: [0x16],
    0x3C: [0x11, 0x12, 0x13, 0x14],
    0x41: [0x06],
    0x50: [0x03, 0x3C],
    0x51: [0x34],
    0x54: [0x01, 0x15, 0x2A, 0x2B],
    0x60: [0x02, 0x04, 0x08, 0x1E, 0x1F, 0x20],
    0x62: [0x2D, 0x2E],
    0x68: [0x21],
    0x6C: [0x09, 0x0A, 0x0B, 0x17],
    0x70: [0x35],
    0x71: [0x00, 0x05, 0x07],
    0x7C: [0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
           0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B],
    0x84: [0x2F, 0x31],
    0xBC: [0x23, 0x24, 0x25, 0x26],
    0xE4: [0x27, 0x28],
}


def check_shape_groups() -> list:
    """Self-check: the shape grouping must cover every valid kind exactly once."""
    seen = []
    for shape, kinds in RECORD_SHAPE_GROUPS.items():
        for k in kinds:
            if OPERAND_DESC_BYTES[k] != shape:
                raise AssertionError(
                    "kind 0x%02X listed under shape 0x%02X but descriptor is 0x%02X"
                    % (k, shape, OPERAND_DESC_BYTES[k]))
            seen.append(k)
    expect = list(range(RECORD_KIND_COUNT))
    if sorted(seen) != expect:
        missing = sorted(set(expect) - set(seen))
        dup = sorted(k for k in set(seen) if seen.count(k) > 1)
        raise AssertionError("shape coverage broken: missing=%s dup=%s"
                             % (missing, dup))
    return sorted(seen)

# Structural constraints the official chunk validator FUN_004D4450 enforces
# per opcode.  "kind" is a short label of what the constraint means.
OPCODE_CONSTRAINTS = {
    0x01: ("kind", "B==1 时后一条指令不能是 opcode 0x32 且 C==0"),
    0x02: ("flow", "A<=param3<=C 时该指令被视为控制流目标"),
    0x03: ("range", "操作数 C 必须落在局部变量区间内"),
    0x05: ("table", "操作数 B 索引的表项类型必须为 4"),
    0x07: ("table", "操作数 B 索引的表项类型必须为 4"),
    0x0C: ("table", "操作数 B 索引的表项类型必须为 4"),
    0x0D: ("table", "操作数 B 索引的表项类型必须为 4"),
    0x0E: ("range", "操作数 C 必须落在局部变量区间内"),
    0x17: ("flow", "A+1 必须是合法局部变量编号；param3==A+1 时为控制流目标"),
    0x21: ("range", "操作数 C 必须落在 B..C 区间内"),
    0x22: ("flow", "跳转目标计算：target = pc + 1 + C"),
    0x2A: ("range", "局部变量编号 + 操作数 C 不得越界"),
    0x2B: ("range", "局部变量编号 + 操作数 C 不得越界"),
    0x2C: ("range", "操作数 C > 1 时额外校验"),
    0x2D: ("flow", "A+3 必须是合法局部变量编号；跳到 pc+1+C"),
    0x2E: ("flow", "A+3 必须是合法局部变量编号；跳到 pc+1+C"),
    0x2F: ("flow", "B 必须非 0；跳到 pc+1+B"),
    0x31: ("flow", "B 必须非 0；跳到 pc+1+B"),
    0x32: ("flow", "B==0 时跳到下一条指令"),
    0x34: ("count", "操作数 C 指向的计数决定后续连续的 opcode 3/4 条数"),
    0x3C: ("flags", "受 chunk 标志位约束（flags & 2 必须置位，flags & 4 必须清零）"),
}


def node_priority(kind: int) -> int:
    """Priority of an expression node kind (FUN_004EE510 uses byte[kind*2])."""
    if kind < 0 or kind >= NODE_KIND_COUNT:
        raise ValueError("node kind out of range: %d" % kind)
    return NODE_PRIORITY[kind][0]


def node_child_priority(kind: int) -> int:
    """Child priority of an expression node kind (byte[kind*2+1])."""
    if kind < 0 or kind >= NODE_KIND_COUNT:
        raise ValueError("node kind out of range: %d" % kind)
    return NODE_PRIORITY[kind][1]


# ---------------------------------------------------------------------------
# AST 节点类型 -> 运算符种类（官方 FUN_004EE2E0 实测映射）
#
# 官方 VM 遍历的是 AST 节点树；每个节点类型先映射成 26 个「运算符种类」之一，
# 再用 NODE_PRIORITY 里的优先级做优先级爬升求值。
# ---------------------------------------------------------------------------
AST_NODE_TO_OPERATOR = {
    0x02B: 0x00, 0x02D: 0x01, 0x02A: 0x02, 0x02F: 0x03,
    0x025: 0x04, 0x026: 0x05, 0x07C: 0x06, 0x05E: 0x07,
    0x129: 0x08, 0x12A: 0x09, 0x12B: 0x0A, 0x11F: 0x0B,
    0x120: 0x0C, 0x126: 0x0D, 0x03D: 0x0E, 0x123: 0x0E,
    0x127: 0x10, 0x125: 0x12, 0x03C: 0x11, 0x124: 0x14,
    0x112: 0x15, 0x113: 0x16, 0x03E: 0x13, 0x03F: 0x17,
    0x03A: 0x18,
}

# 记录种类 -> 变量类别（官方调试器变量解析器 FUN_004D4940 的返回文案）
VARIABLE_RECORD_KINDS = {
    0x03: "variable(upvalue)",
    0x04: "variable(local)",
    0x05: "self(namespace)",
    0x06: "self(namespace)",
    0x07: "global(namespace)",
    0x09: "field(table)",
    0x17: "method(table)",
}

# 官方 VM 核心函数地图（本轮 Ghidra 反编译定位）
VM_KEY_FUNCTIONS = {
    0x004EC700: "VM 主分派：遍历 AST 节点树（节点类型在 +0x14，子节点在 +0x24/0x28/0x2c）",
    0x004EE510: "表达式求值：按运算符种类分派，优先级表 DAT_007C80E0",
    0x004EE2E0: "AST 节点类型 -> 运算符种类（26 类）映射表",
    0x004F5B20: "记录发射器：把 32 位记录写进 code 数组与并行数据数组",
    0x004F4030: "编译器侧分支重定位（记录 B 段写相对位移）",
    0x004D4450: "chunk 指令流校验/解码器（DAT_007C80A0 描述符表）",
    0x004D4940: "官方调试器变量解析器（记录种类 -> 变量类别）",
    0x004E9780: "预编译块入口（跳过 '=' 前缀、校验 15 字节头、解析根 chunk）",
    0x004E93A0: "chunk 解析器（递归）",
    0x004D36D0: "模块加载器：首字节 0x3d 走预编译块，否则走源码",
    0x004F45E0: "求值栈压值",
    0x004F5000: "二元运算符应用入口（参数 0/1 区分左右）",
    0x004F5120: "左值求值（kind 0x15/0x17）",
    0x004F51C0: "右值求值（kind 0x16/0x18）",
}

if __name__ == "__main__":
    print("=== node priority table (VA 0x007C80E0) ===")
    for i, (prio, child) in enumerate(NODE_PRIORITY):
        print("  kind 0x%02x (%2d): priority=0x%02x child=0x%02x%s" % (
            i, i, prio, child, "   <- end marker" if i == NODE_END_MARKER else ""))

    print()
    print("=== builtin dispatch table (VA 0x007C8118) ===")
    for name in sorted(BUILTIN_TABLE):
        print("  %-14s -> FUN_%08X" % (name, BUILTIN_TABLE[name]))

    print()
    print("=== AST node type -> operator kind (FUN_004EE2E0) ===")
    for node in sorted(AST_NODE_TO_OPERATOR):
        k = AST_NODE_TO_OPERATOR[node]
        print("  node 0x%03X -> operator kind 0x%02X (priority 0x%02x)" % (
            node, k, NODE_PRIORITY[k][0]))

    print()
    print("=== record emit sites (FUN_004F5B20) ===")
    for kind in sorted(RECORD_EMIT_SITES):
        print("  kind 0x%02X : %d site(s), %d func(s)" % (
            kind, RECORD_EMIT_COUNTS.get(kind, 0), len(RECORD_EMIT_SITES[kind])))

    print()
    print("=== record kind operand-format descriptor table (VA 0x007C80A0) ===")
    print("    %d valid kinds (0x00..0x%02X); 0x3D..0x3F are rejected" % (
        RECORD_KIND_COUNT, RECORD_KIND_COUNT - 1))
    for k in range(len(OPERAND_DESC_BYTES)):
        d = decode_operand_desc(k)
        if not d["valid"]:
            print("  kind 0x%02X : <invalid>" % k)
            continue
        label = OPCODE_SEMANTICS.get(k, "")
        print("  kind 0x%02X [%02x] : %s%s" % (
            k, d["byte"], record_kind_flags(k),
            ("   <- " + label) if label else ""))
    print()

    print("=== record shapes (kinds grouped by descriptor byte) ===")
    check_shape_groups()
    for shape in sorted(RECORD_SHAPE_GROUPS):
        kinds = RECORD_SHAPE_GROUPS[shape]
        print("  shape 0x%02X : %2d kind(s) %s" % (
            shape, len(kinds),
            "[" + ", ".join("0x%02X" % k for k in kinds) + "]"))
        print("             : %s" % record_kind_flags(kinds[0]))

    print()
    print("=== precompiled chunk runtime frame layout (FUN_004e93a0) ===")
    print("    frame size = 0x%X bytes" % CHUNK_FRAME_SIZE)
    for off in sorted(CHUNK_FRAME_LAYOUT):
        print("  +0x%02X : %s" % (off, CHUNK_FRAME_LAYOUT[off]))
    print("    loader steps:")
    for addr in sorted(CHUNK_LOADER_STEPS):
        print("      FUN_%08X : %s" % (addr, CHUNK_LOADER_STEPS[addr]))

    print("=== opcode semantics recovered ===")
    for op in sorted(OPCODE_SEMANTICS):
        print("  opcode 0x%02X : %s" % (op, OPCODE_SEMANTICS[op]))

    print()
    print("=== opcode constraints (official validator FUN_004D4450) ===")
    for op in sorted(OPCODE_CONSTRAINTS):
        kind, text = OPCODE_CONSTRAINTS[op]
        print("  opcode 0x%02X [%-5s] %s" % (op, kind, text))

    print()
    print("=== runtime record interpreter (FUN_004C4BC0) ===")
    print("    entry  = FUN_%08X (size %d bytes)" % (
        rt.VM_INTERPRETER, rt.VM_INTERPRETER_SIZE))
    print("    dispatch = switch(word & 0x3F), %d legal kinds (0x00..0x%02X)" % (
        rt.VM_KIND_COUNT, rt.VM_KIND_COUNT - 1))
    print("    entered from: %s" % ", ".join(
        "FUN_%08X" % a for a in rt.VM_ENTER_FUNCTIONS))
    print("    record word layout:")
    print("      bits  0.. 5 : kind")
    print("      bits  6..13 : A  (destination local slot)")
    print("      bits 14..22 : C  (bit22 = global flag, bits14..21 = index)")
    print("      bits 23..31 : B  (bit31 = global flag, bits23..30 = index)")
    print("      branch records use bits 14..31 as a signed 18 bit displacement")
    print("    slot layout (%d bytes): value@0x00 type@0x08 extra@0x0C extra2@0x10" % (
        rt.SLOT_SIZE))
    print("    value type tags: %s" % ", ".join(
        "%d=%s" % (k, v) for k, v in sorted(rt.VALUE_TYPE_NAMES.items())))
    print("    61 record semantics:")
    rt.check_opcode_table()
    for kind, name, ops, eff, ev in rt.opcode_table():
        print("      0x%02X %-13s %-10s %-40s [%s]" % (kind, name, ops, eff, ev))
