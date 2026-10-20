#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""官方 aardio 运行期记录解释器（precompiled chunk VM）的实测事实表。

第十一轮从 aardio.exe（image base 0x00400000）里定位到了**运行期记录解释器本体**：

    FUN_004C4BC0  @ 0x004C4BC0  size = 7263 bytes

它是整个二进制里唯一一处对 32 位记录做 `switch (word & 0x3F)` 的函数，
并且 case 覆盖 0x00..0x3C 全部 61 种合法记录种类，与 chunk 校验器
`FUN_004D4450` 的 `if (0x3C < kind) return 0;` 完全吻合。

本模块只记录**实测**事实：字段偏移、取值方式、helper 归属。
凡是靠字符串常量/函数命名推断出来的，一律标 "推断"；尚未定位的标 "未知"。

证据来源（Ghidra 12.1.4 headless，产物在 D:\\ghidra_out）：
    decomp_FUN_004c4bc0.c   解释器主循环（61 个 case 全量）
    decomp_FUN_004c30a0.c   取表成员（含 "__get" 元方法链）
    decomp_FUN_004c3380.c   取全局/命名空间
    decomp_FUN_004c33e0.c   设置元表
    decomp_FUN_004c3650.c   rawset 表
    decomp_FUN_004c3700.c   写表成员（含 "__set" 元方法链）
    decomp_FUN_004c3a30.c   表成员串拼接
    decomp_FUN_004c3b80.c   按点号名导入命名空间成员
    decomp_FUN_004c3db0.c   元方法回退
    decomp_FUN_004c41f0.c   相等比较
    decomp_FUN_004c40f0.c   大于比较
    decomp_FUN_004c4160.c   小于等于比较
    decomp_FUN_004c4410.c   变参串拼接
    decomp_FUN_004c4740.c   二元算术（参数 = opcode - 0x12）
    decomp_FUN_004c49c0.c   位运算（参数 = opcode）
    decomp_FUN_004c4bc0.c   主循环
    decomp_FUN_004d3040.c   调用（建栈帧）
    decomp_FUN_004d33c0.c   调用入口
    decomp_FUN_004d3450.c   调用入口
    decomp_FUN_004d50a0.c   建立闭包对象

文档：docs/aardio-bytecode.md §7
"""
from __future__ import annotations

import os
import sys

# --------------------------------------------------------------- 定位信息 ----

VM_INTERPRETER = 0x004C4BC0
VM_INTERPRETER_SIZE = 7263
VM_ENTER_FUNCTIONS = (0x004D33C0, 0x004D3450)

# 解释器按记录种类分派时用的掩码：kind = word & 0x3F
VM_KIND_MASK = 0x3F
VM_KIND_COUNT = 61          # 0x00..0x3C，case 全部存在（实测）

# --------------------------------------------------------- 记录字位布局 ----
#
# 全部取自 FUN_004C4BC0 自身的位移表达式，不是推断：
#
#   kind = word & 0x3F                    bits  0..5
#   A    = (word >> 6) & 0xFF             bits  6..13   目标槽 / 条件值
#   C    = (word >> 14) & 0x1FF           bits 14..22
#          bit22 = 全局作用域标志；bits 14..21 = 9 位索引
#   B    = (word >> 23) & 0x1FF           bits 23..31
#          bit31 = 全局作用域标志；bits 23..30 = 9 位索引
#
# 跳转类记录用 bits 14..31 当成**一个 18 位有符号相对位移**（bias 0x1FFFE）：
#   IP = 当前指令地址 + ((word >> 14) - 0x1FFFE)
#
FIELD_KIND = (0, 6)
FIELD_A = (6, 8)
FIELD_C = (14, 9)
FIELD_B = (23, 9)

# 9 位操作数字段：低 8 位是索引，最高位是「取外层/全局作用域」标志
OPERAND_GLOBAL_BIT = 0x100

# 18 位有符号相对位移的偏置
REL18_BIAS = 0x1FFFE

# ------------------------------------------------------------ 运行时槽位 ----
#
# 槽位固定 16 字节。实测取法（例：case 0x18 的 "*(int *)(pdVar13 + 1)"）：
#
#   +0x00  值（type 1/3 时是 8 字节值；type 3 按 double 解释）
#   +0x08  类型标签
#   +0x0C  附加（type 4 时是字符串长度；type 6/7 时是对象指针）
#   +0x10  附加 2（type 4 时是字符串数据指针）
#
SLOT_SIZE = 16
SLOT_VALUE = 0x00
SLOT_TYPE = 0x08
SLOT_EXTRA = 0x0C
SLOT_EXTRA2 = 0x10

# 类型标签（实测：FUN_004C30A0 / FUN_004C3700 里按 tag 分派，
# 全局基本类型槽位在 heap+0x98+tag*4）
VALUE_NULL = 0
VALUE_BOOL = 1
VALUE_INT = 2        # 推断：标签 2 走 heap+0x98 的基本类型槽
VALUE_NUMBER = 3     # double
VALUE_STRING = 4     # 长度在 +0x0C，数据指针在 +0x10
VALUE_NAMESPACE = 5  # 命名空间/类表（有自己的哈希表）
VALUE_FUNCTION = 6   # 闭包 / 原生函数
VALUE_TABLE = 7      # 表

VALUE_TYPE_NAMES = {
    VALUE_NULL: "null",
    VALUE_BOOL: "bool",
    VALUE_INT: "int",
    VALUE_NUMBER: "number",
    VALUE_STRING: "string",
    VALUE_NAMESPACE: "namespace",
    VALUE_FUNCTION: "function",
    VALUE_TABLE: "table",
}

# --------------------------------------------------------- VM 状态对象 ----
#
# 解释器的 this 指针（in_EAX）。字段偏移全部实测自 FUN_004C4BC0 / FUN_004D3040。
#
VM_STATE_SIZE = 0x40
VM_STATE_FIELDS = {
    0x08: "值栈顶指针（指向当前槽位）",
    0x0C: "当前帧局部槽表基址",
    0x10: "运行时上下文（堆）指针",
    0x14: "调用帧栈指针（每帧 0x18 字节）",
    0x18: "指令指针 IP（指向记录流中的当前 u32）",
    0x1C: "值栈上界",
    0x20: "值栈基址",
    0x24: "调用帧栈上界",
    0x28: "调用帧栈基址",
    0x2C: "调用帧计数",
    0x34: "调用深度（ushort，上限 200）",
    0x36: "标志字节（bit0=跟踪, bit1=断点）",
    0x40: "父帧 / 原生宿主",
}

# 调用帧（0x18 字节）
VM_FRAME_FIELDS = {
    0x00: "新帧局部槽表基址",
    0x04: "实参区基址",
    0x08: "局部槽表上界",
    0x0C: "记录流（code）基址",
    0x10: "self",
    0x14: "帧计数",
}

# ------------------------------------------------------ 61 种记录语义表 ----
#
# (kind, 名称, 操作数, 效果, 证据强度)
#
# 证据强度：
#   "实测"  = 直接从 FUN_004C4BC0 / 其 helper 的反编译里读出来的行为
#   "推断"  = 由 helper 里的字符串常量或函数名推断
#   "未知"  = 分派已定位但语义尚未还原
#
# 操作数记法：
#   A      目标局部槽（bits 6..13）
#   B      9 位索引槽（bits 23..30，bit31 = 全局作用域标志）
#   C      9 位索引槽（bits 14..21，bit22 = 全局作用域标志）
#   B!     B 取全局作用域
#   C!     C 取全局作用域
#   R18    18 位有符号相对位移（bits 14..31）
#   n      字面量 / 立即数
#
OPCODES = [
    (0x00, "LOAD_OUTER",     "A, C!",  "A ← 外层/全局槽 C（拷贝 12 字节）", "实测"),
    (0x01, "LOAD_INT",      "A, n",   "A ← 整数立即数；C 段非 0 时下一条 u32 是完整立即数", "实测"),
    (0x02, "CLEAR_LOCALS",  "A..B",   "把局部槽 A..B 的类型标签清成 null", "实测"),
    (0x03, "LOAD_UPVALUE",  "A, B",   "A ← 闭包上值槽 B", "实测"),
    (0x04, "LOAD_LOCAL",    "A, B",   "A ← 局部槽 B（拷贝 12 字节）", "实测"),
    (0x05, "GET_MEMBER",    "A, C",   "取表成员（走 __get 元方法链）", "实测"),
    (0x06, "LOAD_SELF_NS",  "A",      "A ← 当前模块命名空间（type 5）", "实测"),
    (0x07, "GET_MEMBER2",   "A, C",   "取表成员变体", "实测"),
    (0x08, "GET_GLOBAL",    "A",      "按类型标签取全局/命名空间表", "实测"),
    (0x09, "GET_MEMBER3",   "A, C",   "取表成员变体", "实测"),
    (0x0A, "GET_MEMBER4",   "A, C",   "取表成员变体", "实测"),
    (0x0B, "INDEX",         "A, C",   "按下标取元素（字符串按字符、数组按元素）", "实测"),
    (0x0C, "SET_MEMBER",    "A, C",   "写表成员（走 __set 元方法链）", "实测"),
    (0x0D, "SET_MEMBER2",   "A, C",   "写表成员变体", "实测"),
    (0x0E, "STORE_UPVALUE", "A, B",   "上值槽 B ← 局部槽 A", "实测"),
    (0x0F, "LOAD_IF_SET",   "A, B",   "当 B 槽非 null 时才复制到 A", "实测"),
    (0x10, "SET_METATABLE", "A, C",   "设置元表", "实测"),
    (0x11, "SET_MEMBER3",   "A, C",   "写表成员变体", "实测"),
    (0x12, "CONCAT_MEMBER", "A, C",   "表成员串拼接（以 ';' 连接）后写回", "实测"),
    (0x13, "RAWSET",        "A, C",   "rawset 表成员（绕过 __set）", "实测"),
    (0x14, "SET_MEMBER4",   "A, C",   "写表成员变体", "实测"),
    (0x15, "NEW_NATIVE",    "A",      "新建原生函数对象", "实测"),
    (0x16, "IMPORT_MEMBER", "A, C",   "按点号名导入命名空间成员", "实测"),
    (0x17, "METHOD_PREP",   "A, B",   "取方法：复制对象到 A 再取成员", "实测"),
    (0x18, "ADD",           "A, B, C", "A ← B + C", "实测"),
    (0x19, "SUB",           "A, B, C", "A ← B - C", "实测"),
    (0x1A, "MUL",           "A, B, C", "A ← B * C", "实测"),
    (0x1B, "DIV",           "A, B, C", "A ← B / C", "实测"),
    (0x1C, "MOD",           "A, B, C", "A ← B % C（浮点取模，整数语义）", "实测"),
    (0x1D, "POW",           "A, B, C", "A ← B ^ C（幂）", "实测"),
    (0x1E, "NEG",           "A, B",   "A ← -B（一元负号）", "实测"),
    (0x1F, "TRUTHY",        "A, B",   "A ← (B 是否为真)，结果类型 1", "实测"),
    (0x20, "LENGTH",        "A, B",   "A ← 长度（字符串长度 / 表元素数）", "实测"),
    (0x21, "CONCAT",        "A, B",   "变参串拼接，结果写回槽", "实测"),
    (0x22, "JMP",           "R18",    "无条件跳转：IP ← 本条 + R18（偏置 0x1FFFE）", "实测"),
    (0x23, "JZ",            "A, C",   "相等比较为假则跳过；真则跳到下一条 u32 的 R18", "实测"),
    (0x24, "JE",            "A, B, C", "比较 B 与 C，相等则跳到下一条 u32 的 R18", "实测"),
    (0x25, "JG",            "A, B, C", "B > C 则跳到下一条 u32 的 R18", "实测"),
    (0x26, "JLE",           "A, B, C", "B <= C 则跳到下一条 u32 的 R18", "实测"),
    (0x27, "JTRUE",         "A, C",   "真值 != C 则跳到下一条 u32 的 R18（偏置 0x1FFFD）", "实测"),
    (0x28, "JTRUE2",        "A, B, C", "真值 != C 则跳转并带上槽 B 的值（偏置 0x1FFFD）", "实测"),
    (0x29, "TAILCALL",      "A, C",   "尾调用", "实测"),
    (0x2A, "CALL",          "A, C",   "调用（建立新帧）", "实测"),
    (0x2B, "CALL_NATIVE",   "A, C",   "调用并搬运实参到新帧", "实测"),
    (0x2C, "RET",           "A, C",   "返回", "实测"),
    (0x2D, "ADDEQ",         "A, R18", "A ← A + delta（复合赋值）", "实测"),
    (0x2E, "SUBEQ",         "A, R18", "A ← A - delta（复合赋值）", "实测"),
    (0x2F, "CALL_MULTI",    "A, C",   "多值调用：搬运 4 个槽后调用", "实测"),
    (0x30, "CALL_VALUE",    "A, C",   "调用函数值（槽类型 6）", "实测"),
    (0x31, "CALL_NS",       "A, C",   "调用（槽类型 5/6）", "实测"),
    (0x32, "BUILD_ARRAY",   "A, C",   "构造数组/参数表；C==0 时下一条 u32 是元素个数", "实测"),
    (0x33, "GC",            "-",      "垃圾回收", "实测"),
    (0x34, "NEW_CLOSURE",   "A, C",   "建立闭包：收集后续 kind 3/4 记录里的上值", "实测"),
    (0x35, "BITNOT",        "A, B",   "A ← ~B", "实测"),
    (0x36, "BITAND",        "A, B, C", "A ← B & C", "实测"),
    (0x37, "BITOR",         "A, B, C", "A ← B | C", "实测"),
    (0x38, "BITXOR",        "A, B, C", "A ← B ^ C", "实测"),
    (0x39, "SHL",           "A, B, C", "A ← B << C", "实测"),
    (0x3A, "SHR",           "A, B, C", "A ← B >> C（算术右移）", "实测"),
    (0x3B, "USHR",          "A, B, C", "A ← B >>> C（逻辑右移）", "实测"),
    (0x3C, "CALL_N",        "A, C",   "带显式实参个数的调用（个数 = B - 1）", "实测"),
]

# 已知但语义未完全还原的记录种类（分派已定位，行为仍需进一步反查）
OPCODE_OPEN_QUESTIONS = {
    0x00: "0x00 与 0x04 都是 12 字节拷贝，差别只在源表（外层 vs 局部）",
    0x01: "「C 段非 0 则下一条 u32 是完整立即数」这条只看到一次，需再确认是否所有立即数都这样编码",
    0x0B: "字符串按字符取值、数组按元素取值走的是同一条记录，具体下标语义待确认",
    0x17: "「复制对象再取成员」与 0x05/0x07 的差别（方法调用 vs 普通取成员）待确认",
    0x20: "type 5（命名空间）的长度走 FUN_004C0E40 的二分查找，语义待确认",
    0x23: "0x23/0x27 跳到「下一条 u32 的 R18」；该 u32 需确认是否恒为 kind 0x22",
}

# 解释器里出现的 helper（名字 -> 作用），全部来自 decomp 观察
VM_HELPERS = {
    0x004C0810: "新建原生函数对象（type 5 / DAT_007C89B0 内建表）",
    0x004C0B40: "表按键查找（返回槽指针，命中 DAT_007C86A0 表示空）",
    0x004C0E40: "有序数组二分查找（type 5 命名空间计数用）",
    0x004C30A0: "取表成员（含 __get 元方法链，错误串 \"_get table \"）",
    0x004C3380: "按类型标签取全局/命名空间",
    0x004C3650: "rawset 表（错误串 \" rawset table \"）",
    0x004C3700: "写表成员（含 __set 元方法链，错误串 \"_set table \"）",
    0x004C3A30: "表成员串拼接（以 ';' 连接）",
    0x004C3B80: "按点号名导入命名空间成员",
    0x004C3DB0: "元方法回退（__add/__sub 等）",
    0x004C40F0: "大于比较",
    0x004C4160: "小于等于比较",
    0x004C41F0: "相等比较",
    0x004C4410: "变参串拼接",
    0x004C4740: "二元算术（参数 = opcode - 0x12：6=+ 7=- 8=* 9=/ 10=% 11=^ 12=一元-）",
    0x004C49C0: "位运算（参数 = opcode：35=~ 36=& 37=| 38=^ 39=<< 3A=>> 3B=>>>）",
    0x004D3040: "调用：建立 0x18 字节调用帧，搬运实参",
    0x004D33C0: "调用入口（解释器 VM 的两个入口之一）",
    0x004D3450: "调用入口（解释器 VM 的两个入口之一）",
    0x004D50A0: "建立闭包对象",
    0x004D5E30: "字符串转数字（支持 0x 前缀与 #radix）",
}


# 跳转类记录：把 bits 14..31 当成一个 18 位有符号相对位移
JUMP_KINDS = frozenset((0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E))

# 每个跳转种类的位移来源与偏置（实测自 FUN_004C4BC0 的取位表达式）：
#
#   kind            位移取自        偏置       目标 = base + (raw - bias)
#   0x22 / 0x2D / 0x2E   本条记录   0x1FFFE   base = pc
#   0x23..0x26          下一条记录 0x1FFFE   base = pc
#   0x27 / 0x28         下一条记录 0x1FFFD   base = pc
#
# 「下一条记录」= pc+1 处的 u32（编译器在条件跳转后紧跟一条 0x22）。
# 0x27/0x28 的偏置比 0x23..0x26 小 1，因此它们的落点与紧跟的 0x22 落点相同。
JUMP_SOURCE = {
    0x22: ("own", 0x1FFFE),
    0x23: ("next", 0x1FFFE),
    0x24: ("next", 0x1FFFE),
    0x25: ("next", 0x1FFFE),
    0x26: ("next", 0x1FFFE),
    0x27: ("next", 0x1FFFD),
    0x28: ("next", 0x1FFFD),
    0x2D: ("own", 0x1FFFE),
    0x2E: ("own", 0x1FFFE),
}


def branch_target(kind: int, pc: int, word: int, next_word: int = 0):
    """跳转类记录的落点。next_word 是 pc+1 处的 u32（非跳转类忽略）。"""
    src, bias = JUMP_SOURCE[kind]
    raw = (word if src == "own" else next_word) >> 14
    return pc + (raw - bias)


def check_jump_consistency(code) -> dict:
    """自检：条件跳转与其紧跟的 0x22 必须落在同一个目标。

    官方编译器在条件跳转后紧跟一条 0x22；解释器里 0x23..0x26 的偏置是
    0x1FFFE、0x27/0x28 是 0x1FFFD，而 0x22 用自己的记录 + 0x1FFFE，
    因此两者必须算出同一个落点。返回 {'pairs': n, 'mismatch': [...]}。
    """
    pairs = 0
    mismatch = []
    n = len(code)
    for pc in range(n - 1):
        kind = code[pc] & VM_KIND_MASK
        if kind not in (0x27, 0x28):
            continue
        if (code[pc + 1] & VM_KIND_MASK) != 0x22:
            continue
        pairs += 1
        t_cond = branch_target(kind, pc, code[pc], code[pc + 1])
        t_jmp = branch_target(0x22, pc + 1, code[pc + 1], 0)
        if t_cond != t_jmp:
            mismatch.append((pc, t_cond, t_jmp))
    return {"pairs": pairs, "mismatch": mismatch}


def opcode_table() -> list:
    """返回 61 条记录语义，顺序按 kind 升序。"""
    return sorted(OPCODES, key=lambda row: row[0])


def check_opcode_table() -> list:
    """自检：必须正好 61 条、kind 连续 0x00..0x3C、无重复。"""
    kinds = [row[0] for row in OPCODES]
    if len(kinds) != VM_KIND_COUNT:
        raise AssertionError("opcode count = %d, expected %d"
                             % (len(kinds), VM_KIND_COUNT))
    if len(set(kinds)) != len(kinds):
        dup = sorted(k for k in set(kinds) if kinds.count(k) > 1)
        raise AssertionError("duplicate opcode kinds: %s" % dup)
    expect = list(range(VM_KIND_COUNT))
    if sorted(kinds) != expect:
        missing = sorted(set(expect) - set(kinds))
        raise AssertionError("missing opcode kinds: %s" % missing)
    return sorted(kinds)


def decode_record(word: int, pc: int = 0, next_word: int = 0) -> dict:
    """按解释器自身的取位方式解码一条 32 位记录。"""
    kind = word & VM_KIND_MASK
    if kind >= VM_KIND_COUNT:
        raise ValueError("illegal record kind 0x%02X" % kind)
    a = (word >> FIELD_A[0]) & 0xFF
    c = (word >> FIELD_C[0]) & 0x1FF
    b = (word >> FIELD_B[0]) & 0x1FF
    rel18 = (word >> 14) - REL18_BIAS
    out = {
        "kind": kind,
        "name": opcode_name(kind),
        "a": a,
        "b": b & 0xFF,
        "b_global": bool(b & OPERAND_GLOBAL_BIT),
        "c": c & 0xFF,
        "c_global": bool(c & OPERAND_GLOBAL_BIT),
        "rel18": rel18,
        "word": word,
    }
    if kind in JUMP_SOURCE:
        src, bias = JUMP_SOURCE[kind]
        raw = (word if src == "own" else next_word) >> 14
        out["rel18"] = raw - bias
        out["target"] = pc + out["rel18"]
        out["rel_from_next"] = (src == "next")
    else:
        out["target"] = None
        out["rel_from_next"] = False
    return out


def opcode_name(kind: int) -> str:
    for k, name, _ops, _eff, _ev in OPCODES:
        if k == kind:
            return name
    return "OP%02X" % kind


if __name__ == "__main__":
    print("=== 官方运行期解释器 ===")
    print("  FUN_%08X  size=%d  分派: switch(word & 0x3F)" % (
        VM_INTERPRETER, VM_INTERPRETER_SIZE))
    print("  合法记录种类: %d (0x00..0x%02X)" % (VM_KIND_COUNT, VM_KIND_COUNT - 1))
    print("  入口: %s" % ", ".join("FUN_%08X" % a for a in VM_ENTER_FUNCTIONS))
    print()

    print("=== 记录字位布局（实测自 FUN_004C4BC0）===")
    print("  bits  0.. 5 : kind   (6 bit)")
    print("  bits  6..13 : A      (8 bit)  目标局部槽")
    print("  bits 14..22 : C      (9 bit)  bit22=全局标志, bits14..21=索引")
    print("  bits 23..31 : B      (9 bit)  bit31=全局标志, bits23..30=索引")
    print("  跳转类用 bits 14..31 作 18 位有符号相对位移 (bias 0x%05X)" % REL18_BIAS)
    print()

    print("=== 槽位布局（16 字节）===")
    for off, what in ((SLOT_VALUE, "值"), (SLOT_TYPE, "类型标签"),
                      (SLOT_EXTRA, "附加（字符串长度/对象指针）"),
                      (SLOT_EXTRA2, "附加 2（字符串数据指针）")):
        print("  +0x%02X  %s" % (off, what))
    print("  类型标签: %s" % ", ".join(
        "%d=%s" % (k, v) for k, v in sorted(VALUE_TYPE_NAMES.items())))
    print()

    print("=== VM 状态对象 ===")
    for off, what in sorted(VM_STATE_FIELDS.items()):
        print("  +0x%02X  %s" % (off, what))
    print()

    print("=== 61 种记录语义 ===")
    check_opcode_table()
    for kind, name, ops, eff, ev in opcode_table():
        print("  0x%02X %-14s %-10s %-38s [%s]" % (kind, name, ops, eff, ev))
    print()

    if OPCODE_OPEN_QUESTIONS:
        print("=== 未决点 ===")
        for kind, q in sorted(OPCODE_OPEN_QUESTIONS.items()):
            print("  0x%02X: %s" % (kind, q))
        print()

    print("=== 解释器 helper 地图 ===")
    for addr, what in sorted(VM_HELPERS.items()):
        print("  FUN_%08X  %s" % (addr, what))

    if len(sys.argv) > 1:
        print()
        print("=== 官方样本跳转模型自检 ===")
        import os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from aardio_bytecode import Reader, parse_chunk, HEADER_LEN
        data = open(sys.argv[1], "rb").read()
        root = parse_chunk(Reader(data, HEADER_LEN))

        def all_code(node, acc):
            acc.append(node["code"])
            for child in node.get("objects", []):
                all_code(child, acc)
            return acc

        total = 0
        bad = 0
        for code in all_code(root, []):
            r = check_jump_consistency(code)
            total += r["pairs"]
            bad += len(r["mismatch"])
        print("  条件跳转/0x22 配对: %d 对，落点不一致: %d" % (total, bad))
        if bad:
            raise SystemExit("jump model mismatch")
