#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""合成官方 "=aardio" 预编译块（precompiled chunk）—— 记录 VM 的测试夹具生成器。

官方格式的文法见 docs/aardio-bytecode.md §3；本工具按同一文法**写**出字节，
用来给 Nodio 的记录 VM（engine/aardio_vm.cpp）提供可执行的样本：
官方 aardio 自己不提供任何「执行一个预编译块」的入口，
而官方样本（lib/ide/aifix.aardio）依赖命名空间/闭包/原生函数，
无法用来单独验证算术与分支这类基础语义。

记录字布局（docs/aardio-bytecode.md §7.2 / §7.5.4）：
    bits  0.. 5 : kind
    bits  6..13 : A      （目标槽）
    bits 14..22 : C      （9 位操作数②；bit22=1 → 指常量表）
    bits 23..31 : B      （9 位操作数①；bit31=1 → 指常量表）
    描述符类别 1 的记录：bits 14..31 是**一个 18 位常量下标**。

**二元运算的左右顺序**：左操作数在 bits 23..31（B），右操作数在 bits 14..22（C）。
    实测依据（官方解释器 FUN_004C4BC0）：
      · 0x19 SUB  → 结果 = *B − *C
      · 0x1B DIV  → 结果 = *B / *C
      · 0x36/39  → FUN_004C49C0(vm, dest, *B, *C, kind)，其中 lhs=*B、rhs=*C
    并由官方样本 aifix.aardio 交叉验证（0x1B 的 B 是局部槽、C 是常量 2.0）。

比较跳转：
      0x23/0x24  r = (B == C)
      0x25       r = (C > B)
      0x26       r = (B <= C)
      0x27/0x28  A/C 里的立即数与真值比较

跳转：kind 0x22 的位移在本条（偏置 0x1FFFE）；
      条件跳转 0x23-0x26 / 0x27-0x28 的位移在**下一条 u32**（一条 0x22），
      偏置分别是 0x1FFFE / 0x1FFFD，落点 = pc + (raw - 偏置)。

用法:
    python aardio_make_chunk.py <输出目录>
"""
from __future__ import annotations

import os
import struct
import sys

MAGIC = b"=aardio"
HEADER = MAGIC + bytes((0x10, 0x00, 0x01, 0x04, 0x04, 0x04, 0x08, 0x00))

# 描述符类别 1（单 18 位操作数）的记录
WIDE_KINDS = {0x00, 0x05, 0x07, 0x0C, 0x0D, 0x34}
# 条件跳转：位移取自下一条 u32
COND_KINDS = {0x23: 0x1FFFE, 0x24: 0x1FFFE, 0x25: 0x1FFFE,
              0x26: 0x1FFFE, 0x27: 0x1FFFD, 0x28: 0x1FFFD}


def rec(kind: int, a: int = 0, c: int = 0, b: int = 0) -> int:
    """普通记录（三操作数 / 单 18 位）。"""
    if kind in WIDE_KINDS:
        # 18 位常量下标：bits 14..31
        return (kind & 0x3F) | ((a & 0xFF) << 6) | ((c & 0x3FFFF) << 14)
    return ((kind & 0x3F) | ((a & 0xFF) << 6) |
            ((c & 0x1FF) << 14) | ((b & 0x1FF) << 23))


def jump_word(target: int, pc: int) -> int:
    """0x22 JMP：本条带 18 位位移。"""
    rel = target - pc
    raw = (rel + 0x1FFFE) & 0x3FFFF
    return 0x22 | (raw << 14)


class Chunk(object):
    def __init__(self, slots: int = 8, upvalues: int = 0):
        self.code = []
        self.consts = []       # (type, payload)
        self.slots = slots
        self.upvalues = upvalues
        self.children = []     # 嵌套对象（闭包体）—— NEW_CLOSURE 的 K 指向它

    # ---- 发射 ----
    def emit(self, word: int) -> int:
        self.code.append(word & 0xFFFFFFFF)
        return len(self.code) - 1

    def load_int(self, a: int, v: int) -> int:
        if 0 <= v <= 0x1FF:
            return self.emit(rec(0x01, a=a, b=v))
        # 完整立即数：下一条 u32
        pc = self.emit(rec(0x01, a=a, c=1))
        self.emit(v & 0xFFFFFFFF)
        return pc

    def load_const(self, a: int, idx: int) -> int:
        """LOAD_OUTER：18 位常量下标。"""
        return self.emit(rec(0x00, a=a, c=idx))

    def add_const(self, text: str) -> int:
        payload = text.encode("utf-8") + b"\x00"
        self.consts.append((4, struct.pack("<I", len(payload) + 1) + payload + b"\x00"))
        return len(self.consts) - 1

    def add_number(self, v: float) -> int:
        self.consts.append((3, struct.pack("<d", v)))
        return len(self.consts) - 1

    def local(self, a: int, b: int) -> int:
        return self.emit(rec(0x04, a=a, b=b))

    def arith(self, kind: int, a: int, left: int, right: int) -> int:
        """二元运算：**左操作数在 B（bits23..31），右操作数在 C（bits14..22）**。

        一元记录（0x1E NEG / 0x1F TRUTHY / 0x20 LENGTH / 0x35 BITNOT）
        的操作数同样在 B，左参数即其操作数。
        """
        return self.emit(rec(kind, a=a, c=right, b=left))

    def ret(self, a: int) -> int:
        return self.emit(rec(0x2C, a=a))

    def jmp(self, target: int) -> int:
        """0x22 JMP：位移在本条（偏置 0x1FFFE），落点 = 本条 pc + 位移。"""
        pc = len(self.code)
        self.emit(jump_word(target, pc))
        return pc

    def cond(self, kind: int, a: int, left: int, right: int, target: int) -> int:
        """条件跳转 + 紧跟的 0x22（位移在下一条 u32）。

        语义按「left OP right」书写，按官方各自的比较方向落位：
          0x23/0x24 → (B == C)  对称
          0x25      → (C > B)   故 C = left
          0x26      → (B <= C)  故 B = left
        """
        if kind == 0x25:
            c, b = left, right
        else:
            c, b = right, left
        pc = self.emit(rec(kind, a=a, c=c, b=b))
        bias = COND_KINDS[kind]
        raw = ((target - pc) + bias) & 0x3FFFF
        self.emit(0x22 | (raw << 14))
        return pc

    # ---- 成员访问族 ----

    def get_member(self, a: int, const_idx: int) -> int:
        """0x05 GET_MEMBER：对象 = 模块命名空间，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x05, a=a, c=const_idx))

    def set_member(self, a: int, const_idx: int) -> int:
        """0x0C SET_MEMBER：值 = 槽 A，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x0C, a=a, c=const_idx))

    def get_member2(self, a: int, const_idx: int) -> int:
        """0x07 GET_MEMBER2：对象 = VM 暂存槽(vm+0x40)，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x07, a=a, c=const_idx))

    def set_member2(self, a: int, const_idx: int) -> int:
        """0x0D SET_MEMBER2：值 = 槽 A，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x0D, a=a, c=const_idx))

    def get_member3(self, a: int, obj: int, key_const: int) -> int:
        """0x09 GET_MEMBER3：目标 = 槽 A，对象 = 槽 B，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x09, a=a, c=key_const | 0x100, b=obj))

    def set_member3(self, a: int, key_const: int, value: int) -> int:
        """0x11 SET_MEMBER3：对象 = 槽 A，键 = 常量表[const_idx]，值 = 槽 C。"""
        return self.emit(rec(0x11, a=a, c=value, b=key_const | 0x100))

    def index(self, a: int, obj: int, key: int) -> int:
        """0x0B INDEX：目标 = 槽 A，对象 = 槽 B，下标 = 槽 C。"""
        return self.emit(rec(0x0B, a=a, c=key, b=obj))

    def method_prep(self, a: int, obj: int, key_const: int) -> int:
        """0x17 METHOD_PREP：结果 = 槽 A，A+1 = 槽 B 副本，键 = 常量表[const_idx]。"""
        return self.emit(rec(0x17, a=a, c=key_const | 0x100, b=obj))

    def load_self_ns(self, a: int) -> int:
        """0x06 LOAD_SELF_NS：槽 A ← 当前模块命名空间。"""
        return self.emit(rec(0x06, a=a))

    def build_array(self, a: int, count: int, cap: int = 1) -> int:
        """0x32 BUILD_ARRAY：目标表 = 槽 A，元素取自 A+1..A+count。"""
        return self.emit(rec(0x32, a=a, c=cap, b=count))

    # ---- 调用帧与闭包（第十六轮）----

    def new_closure(self, a: int, k: int) -> int:
        """0x34 NEW_CLOSURE：目标槽 A，K = 嵌套对象下标（18 位）。

        捕获条数 = 嵌套对象的 flags[0]（官方运行时 +0x48）；
        紧随其后的 N 条记录是捕获项（kind 3 = 上值 / kind 4 = 局部槽），
        会被**消费**（ip 前进 N 条）。
        """
        return self.emit(rec(0x34, a=a, c=k))

    def call(self, a: int, b: int, c: int = 2) -> int:
        """0x2A CALL：被调值在槽 A，实参窗口 = 槽 A+1..A+C-1，返回槽 = A+B。

        官方 0x2A 装参块 0x004C5A44：
          · 把 (C-1) 压栈作为**实参个数**（C = bits14..22）；
          · 写 vm->0x08 = &slot[A+B]（B = bits23..31）作为返回槽。
        官方校验器 FUN_004D4450 的槽位检查「nslots > A + C - 2」
        正好等价于「实参 = 槽 A+1 .. A+C-1」。
        """
        return self.emit(rec(0x2A, a=a, c=c, b=b))

    # ---- 动态调用族（第二十轮）----

    def call_multi(self, a: int, argc: int, target: int) -> int:
        """0x2F CALL_MULTI：被调值 = 槽 A，实参个数 = argc（官方只搬两个实参）。

        官方把 (A, A+1, A+2) 上移 3 位 → (A+3, A+4, A+5)，
        以 A+3（= 原 A）为被调值调用，返回值写回槽 A+6；
        调用后若槽 A+3 非空 → 槽 A-1 := 槽 A+3，并按**下一条 0x22** 的位移跳转。
        这里紧跟一条 0x22，落点 = target。
        """
        pc = self.emit(rec(0x2F, a=a, c=argc + 1))
        self.jmp(target)
        return pc

    def call_value(self, a: int) -> int:
        """0x30 CALL_VALUE：槽 A+1 是函数（类型 6）时以 0 个实参调用它。

        返回值写回槽 A+2；官方在类型不是 6 时什么都不做（顺序执行）。
        """
        return self.emit(rec(0x30, a=a))

    def call_ns(self, a: int, target: int) -> int:
        """0x31 CALL_NS：槽 A 是表 → 取第一个非空成员（键写回槽 A+1，值写进槽 A+4）。

        官方紧跟一条 0x22（官方校验器要求 0x31/0x2F 的后继必须是 0x22）。
        """
        pc = self.emit(rec(0x31, a=a, c=2))
        self.jmp(target)
        return pc

    def load_upvalue(self, a: int, b: int) -> int:
        """0x03 LOAD_UPVALUE：目标槽 A，上值槽 B。"""
        return self.emit(rec(0x03, a=a, b=b))

    def store_upvalue(self, a: int, b: int) -> int:
        """0x0E STORE_UPVALUE：源槽 A，上值槽 B（按引用写入）。"""
        return self.emit(rec(0x0E, a=a, b=b))

    # ---- 命名空间与导入（第十七轮）----

    def new_native(self, a: int, b: int = 0, c: int = 0) -> int:
        """0x15 NEW_NATIVE：目标槽 A ← 新建的命名空间表（父表 = 全局命名空间）。

        B / C 是官方打包的两个小整数位域（((x & 7) + 8) << (x >> 3 - 1)），
        官方只把它落到表标志位上，语义尚未查明（docs §7.8）。
        """
        return self.emit(rec(0x15, a=a, c=c, b=b))

    def import_member(self, dest: int, name_operand: int) -> int:
        """0x16 IMPORT_MEMBER：名字 = B 操作数（bit8=1 → 常量下标），目标 = 槽 C。

        官方描述符：0x16 的 C 段寻址类别 = 局部槽下标，B 段 = 常量或局部槽。
        """
        return self.emit(rec(0x16, a=0, c=dest, b=name_operand))

    # ---- 拼接与循环（第十八轮）----

    def concat(self, a: int, b: int, c: int) -> int:
        """0x21 CONCAT：把槽 B..C 的值拼成一个字符串，写回槽 B 再拷进槽 A。

        官方：count = C - B + 1，FUN_004C4410 按**升序**拼接槽 B..C。
        """
        return self.emit(rec(0x21, a=a, c=c, b=b))

    def addeq(self, a: int, target: int) -> int:
        """0x2D ADDEQ：for 循环推进（A = 计数器槽），位移取自本条（偏置 0x1FFFE）。"""
        return self.emit(self._jump_like(0x2D, a, target))

    def subeq(self, a: int, target: int) -> int:
        """0x2E SUBEQ：槽[A] -= 槽[A+2] 后无条件跳转（位移取自本条，偏置 0x1FFFE）。"""
        return self.emit(self._jump_like(0x2E, a, target))

    def _jump_like(self, kind: int, a: int, target: int) -> int:
        """跳转类记录（0x22/0x2D/0x2E）：位移取自本条 18 位，偏置 0x1FFFE。"""
        pc = len(self.code)
        raw = ((target - pc) + 0x1FFFE) & 0x3FFFF
        return (kind & 0x3F) | ((a & 0xFF) << 6) | (raw << 14)

    # ---- 序列化 ----

    def body(self) -> bytes:
        out = bytearray()
        out += struct.pack("<I", 0)          # name = null
        out += struct.pack("<i", 0)          # attr1 = 首行
        out += struct.pack("<i", 0)          # attr2 = 末行
        # flags[0] = 上值个数（运行时 +0x48）/ flags[3] = 局部槽数（+0x4B）
        out += bytes((self.upvalues & 0xFF, 0, 0, self.slots & 0xFF, 0))
        out += struct.pack("<I", len(self.code))
        for w in self.code:
            out += struct.pack("<I", w)
        out += struct.pack("<I", len(self.consts))
        for t, payload in self.consts:
            out += bytes((t,)) + payload
        out += struct.pack("<I", len(self.children))
        for ch in self.children:
            out += ch.body()
        out += struct.pack("<I", 0)          # tableA
        out += struct.pack("<I", 0)          # tableB
        out += struct.pack("<I", 0)          # tableC
        return bytes(out)

    def bytes(self) -> bytes:
        return bytes(HEADER) + self.body()


def build_add():
    """3 + 4 = 7"""
    c = Chunk(slots=4)
    c.load_int(0, 3)
    c.load_int(1, 4)
    c.arith(0x18, 2, 0, 1)      # ADD：槽2 = 槽0 + 槽1
    c.ret(2)
    return c


def build_loop():
    """1 + 2 + ... + 10 = 55"""
    c = Chunk(slots=6)
    c.load_int(0, 1)           # i
    c.load_int(1, 0)           # sum
    c.load_int(2, 10)          # limit
    # 3: sum += i
    c.arith(0x18, 1, 1, 0)
    # 4: i = i + 1
    c.load_int(3, 1)
    c.arith(0x18, 0, 0, 3)
    # 6: JLE（r = (B <= C)，即 i <= limit 时跳转）→ 回到 3
    c.cond(0x26, 1, 0, 2, 3)
    c.ret(1)
    return c


def build_bits():
    """(0xF0 & 0x3C) | 0x01 = 0x31 = 49"""
    c = Chunk(slots=6)
    c.load_int(0, 0xF0)
    c.load_int(1, 0x3C)
    c.arith(0x36, 2, 0, 1)      # BITAND
    c.load_int(3, 0x01)
    c.arith(0x37, 2, 2, 3)      # BITOR
    c.ret(2)
    return c


def build_str():
    """常量表 + LENGTH："hello" 的长度 = 5"""
    c = Chunk(slots=4)
    idx = c.add_const("hello")
    c.load_const(0, idx)        # LOAD_OUTER：槽0 ← 常量
    c.arith(0x20, 1, 0, 0)      # LENGTH：槽1 = len(槽0)
    c.ret(1)
    return c


def build_neg():
    """NEG + TRUTHY：-(7) = -7，非零为真 = 1"""
    c = Chunk(slots=4)
    c.load_int(0, 7)
    c.arith(0x1E, 1, 0, 0)      # NEG：槽1 = -槽0（操作数在 B）
    c.arith(0x1F, 2, 1, 0)      # TRUTHY：槽2 = 真值(槽1)（操作数在 B）
    c.ret(1)
    return c


def build_sub():
    """SUB 的左右顺序：官方是 B - C，故 10 - 3 = 7"""
    c = Chunk(slots=4)
    c.load_int(0, 10)
    c.load_int(1, 3)
    c.arith(0x19, 2, 0, 1)      # 槽2 = 槽0 - 槽1
    c.ret(2)
    return c


def build_div():
    """DIV 的左右顺序：官方是 B / C，故 9 / 2 = 4.5"""
    c = Chunk(slots=4)
    c.load_int(0, 9)
    c.load_int(1, 2)
    c.arith(0x1B, 2, 0, 1)      # 槽2 = 槽0 / 槽1
    c.ret(2)
    return c


def build_shl():
    """SHL 的左右顺序：官方是 B << C，故 1 << 4 = 16"""
    c = Chunk(slots=4)
    c.load_int(0, 1)
    c.load_int(1, 4)
    c.arith(0x39, 2, 0, 1)      # 槽2 = 槽0 << 槽1
    c.ret(2)
    return c


def build_member():
    """模块命名空间的成员：ns.answer = 42，再取回来"""
    c = Chunk(slots=4)
    key = c.add_const("answer")
    c.load_int(0, 42)
    c.set_member(0, key)        # 0x0C：ns.answer = 槽0
    c.get_member(1, key)        # 0x05：槽1 = ns.answer
    c.ret(1)
    return c


def build_member2():
    """VM 暂存槽(vm+0x40)的成员：scratch.k = 7，再取回来"""
    c = Chunk(slots=4)
    key = c.add_const("k")
    c.load_int(0, 7)
    c.set_member2(0, key)       # 0x0D：scratch.k = 槽0
    c.get_member2(1, key)       # 0x07：槽1 = scratch.k
    c.ret(1)
    return c


def build_member3():
    """对象在 A、键在 B、值在 C 的 set/get 变体（0x11 / 0x09）"""
    c = Chunk(slots=6)
    key = c.add_const("m")
    c.load_self_ns(0)           # 槽0 = 模块命名空间（一张表）
    c.load_int(1, 5)
    c.set_member3(0, key, 1)    # 0x11：ns.m = 槽1
    c.get_member3(2, 0, key)    # 0x09：槽2 = ns.m
    c.ret(2)
    return c


def build_index():
    """字符串按下标取字符（1 基）："hello" 的第 2 个字符 = 'e' = 101"""
    c = Chunk(slots=6)
    idx = c.add_const("hello")
    c.load_const(0, idx)        # 槽0 = "hello"
    c.load_int(1, 2)            # 槽1 = 2
    c.index(2, 0, 1)            # 0x0B：槽2 = 槽0[槽1]
    c.ret(2)
    return c


def build_method():
    """0x17 METHOD_PREP：把对象拷到 A+1，再取成员到 A"""
    c = Chunk(slots=6)
    key = c.add_const("m")
    c.load_self_ns(0)           # 槽0 = 模块命名空间
    c.load_int(1, 8)
    c.set_member3(0, key, 1)    # ns.m = 8
    c.method_prep(2, 0, key)    # 槽3 = 槽0 的副本；槽2 = ns.m
    c.ret(2)
    return c


def build_array():
    """0x32 BUILD_ARRAY：把 A+1..A+B 的元素追加进 A 槽的表（长度 = 2）"""
    c = Chunk(slots=6)
    c.load_self_ns(0)           # 槽0 = 模块命名空间（目标表）
    c.load_int(1, 10)
    c.load_int(2, 20)
    c.build_array(0, 2, 1)      # 槽0 的表追加 2 个元素
    c.arith(0x20, 3, 0, 0)      # 槽3 = len(槽0)
    c.ret(3)
    return c


def build_call():
    """0x34 NEW_CLOSURE + 0x2A CALL：闭包返回 (参数1 + 参数2) = 10

    官方调用约定：被调值在槽 A，实参窗口 = 槽 A+1..A+B-1（第一个是 self），
    返回值写入槽 A+B。
    被调帧的槽布局：**槽 0 = self，槽 1.. = 显式参数**，
    函数体的局部变量与参数共用同一片槽（官方编译器把局部排在参数之后），
    所以这里的局部从槽 2 开始。
    """
    body = Chunk(slots=4)
    body.arith(0x18, 2, 1, 2)     # 槽2 = 帧槽1 + 帧槽2 = 7 + 3
    body.ret(2)

    c = Chunk(slots=6)
    c.children.append(body)
    c.new_closure(0, 0)           # 槽0 = 闭包（体 = 子对象 0）
    c.load_int(1, 5)              # 槽1 = self
    c.load_int(2, 7)              # 槽2 = 参数1
    c.load_int(3, 3)              # 槽3 = 参数2
    c.call(0, 4, 4)               # 实参 = 槽1..槽3（3 个）；返回槽 = 槽4
    c.ret(4)
    return c


def build_params():
    """调用带参数的闭包：self + 参数1 = 3 + 4 = 7"""
    body = Chunk(slots=4)
    body.arith(0x18, 2, 0, 1)     # 槽2 = 帧槽0(self) + 帧槽1(参数1)
    body.ret(2)

    c = Chunk(slots=6)
    c.children.append(body)
    c.new_closure(0, 0)
    c.load_int(1, 3)              # 槽1 = self
    c.load_int(2, 4)              # 槽2 = 参数1
    c.call(0, 3, 3)               # 实参 = 槽1..槽2（2 个）；返回槽 = 槽3
    c.ret(3)
    return c


def build_upvalue():
    """闭包按引用改写外层局部：10 → 99

    0x34 之后的捕获项（kind 4 = LOAD_LOCAL）把外层帧槽 0 **装箱**，
    闭包里的 0x0E STORE_UPVALUE 写的就是同一个 cell（官方 FUN_004D5210）。
    """
    body = Chunk(slots=4, upvalues=1)
    body.load_upvalue(1, 0)       # 槽1 = 上值0
    body.load_int(2, 99)          # 槽2 = 99
    body.store_upvalue(2, 0)      # 上值0 ← 槽2（按引用写回外层帧槽 0）
    body.load_upvalue(3, 0)       # 槽3 = 上值0（此时 = 99）
    body.ret(3)

    c = Chunk(slots=6)
    c.children.append(body)
    c.load_int(0, 10)             # 槽0 = 10
    c.new_closure(1, 0)           # 槽1 = 闭包（1 个上值）
    c.local(0, 0)                 # 捕获项：装箱帧槽 0（kind 4 / LOAD_LOCAL）
    c.call(1, 2, 2)               # 被调 = 槽1；实参 = 槽2（1 个）；返回槽 = 槽3
    c.local(4, 0)                 # 槽4 = 帧槽 0（应已被闭包改成 99）
    c.ret(4)
    return c


def build_recurse():
    """递归闭包：f(n) = n * f(n-1)，f(5) = 120

    闭包先存进**模块命名空间**（0x0C SET_MEMBER），函数体再用 0x05 GET_MEMBER
    把「自己」取回来调用 —— 这条路径同时验证：
      闭包能进表 / 能从表取回 / 闭包的属主命名空间对帧可见 / 嵌套帧。
    """
    body = Chunk(slots=8)
    key = body.add_const("f")
    # 被调帧：槽 0 = self，槽 1 = 参数 n，**局部变量从槽 2 起**
    body.get_member(2, key)        # 槽2 = ns.f（自己）
    body.local(3, 1)               # 槽3 = 帧槽1（参数 n）
    body.load_int(4, 1)            # 槽4 = 1
    # 注意：cond() 会连发**两条**记录（条件记录 + 一条 0x22），所以后面的
    # 记录号要 +1：base = 13、end = 14。
    body.cond(0x26, 1, 3, 4, 13)   # if (n <= 1) → base
    body.local(4, 1)               # 槽4 = n
    body.load_int(5, 1)            # 槽5 = 1
    body.arith(0x19, 4, 4, 5)      # 槽4 = n - 1
    body.local(5, 1)               # 槽5 = n
    body.call(2, 5, 5)             # f(...)：实参 = 槽3..槽6（4 个）；返回槽 = 槽7
    body.local(6, 1)               # 槽6 = n
    body.arith(0x1A, 7, 7, 6)      # 槽7 = 槽7 * 槽6
    body.jmp(14)                   # → end
    body.load_int(7, 1)            # base：槽7 = 1
    body.ret(7)                    # end：返回槽7

    c = Chunk(slots=8)
    c.children.append(body)
    ckey = c.add_const("f")
    c.new_closure(0, 0)             # 槽0 = 闭包
    c.set_member(0, ckey)           # ns.f = 闭包
    c.get_member(2, ckey)           # 槽2 = ns.f
    c.load_int(3, 0)                # 槽3 = self
    c.load_int(4, 5)                # 槽4 = n = 5
    c.call(2, 4, 4)                 # f(5)：实参 = 槽3..槽5（3 个）；返回槽 = 槽6
    c.ret(6)
    return c


def build_new_native():
    """0x15 NEW_NATIVE：新建命名空间表 → 写成员 → 读回来 = 42

    官方 FUN_004C0810 建的是一张**空的 type 5 表**（父表 = 全局内建表），
    表里的成员由随后的 SET_MEMBER3 填 —— 这正是官方样本 aifix.aardio 的写法。
    """
    c = Chunk(slots=6)
    key = c.add_const("v")
    c.new_native(1)                # 槽1 = 新命名空间
    c.load_int(2, 42)              # 槽2 = 42
    c.set_member3(1, key, 2)       # 0x11：ns.v = 槽2
    c.get_member3(3, 1, key)       # 0x09：槽3 = ns.v
    c.ret(3)
    return c


def build_import():
    """0x16 IMPORT_MEMBER：全局.mod ← 命名空间；import "mod" 后读回成员 = 42

    0x0D SET_MEMBER2 写的对象是 VM 暂存槽（vm+0x40）= **全局命名空间**，
    所以这里先把模块挂到全局，再 import 它。
    """
    c = Chunk(slots=8)
    gkey = c.add_const("mod")
    mkey = c.add_const("v")
    c.new_native(1)                # 槽1 = 模块命名空间
    c.load_int(2, 42)
    c.set_member3(1, mkey, 2)      # 模块.v = 42
    c.set_member2(1, gkey)         # 0x0D：全局.mod = 模块
    c.import_member(3, gkey | 0x100)   # 名字 = 常量 "mod"；目标 = 槽3
    c.get_member(4, mkey)          # 0x05：模块命名空间已切到 mod → 槽4 = mod.v
    c.ret(4)
    return c


def build_import_new():
    """0x16 IMPORT_MEMBER：导入一个**全局里没有**的名字 = 7

    官方 FUN_004C3B80：查不到的段会就地建一张空命名空间并登记在当前表里，
    然后模块命名空间切到这张新表。
    """
    c = Chunk(slots=8)
    nkey = c.add_const("newmod")
    vkey = c.add_const("v")
    c.import_member(2, nkey | 0x100)   # 名字 = "newmod"（全局里没有）→ 目标 = 槽2
    c.get_member3(1, 2, nkey)          # 槽1 = 槽2["newmod"]（原模块命名空间里新建的那张表）
    c.load_int(3, 7)
    c.set_member3(1, vkey, 3)          # newmod.v = 7
    c.get_member(4, vkey)              # 模块命名空间（已切到 newmod）.v = 7
    c.ret(4)
    return c


def build_concat():
    """0x21 CONCAT：把槽 B..C 的值按升序拼起来 = "a12b"

    官方样本 aifix.aardio 的 /0 pc=401 就是
    「LOAD_OUTER 槽16 / LOAD_LOCAL 槽17 / LOAD_OUTER 槽18 → CONCAT A=16 B=16 C=18」。
    """
    c = Chunk(slots=6)
    k1 = c.add_const("a")
    k2 = c.add_const("b")
    c.load_const(1, k1)            # 槽1 = "a"
    c.load_int(2, 12)              # 槽2 = 12
    c.load_const(3, k2)            # 槽3 = "b"
    c.concat(1, 1, 3)              # 槽1 = "a" + "12" + "b"
    c.ret(1)
    return c


def build_forloop():
    """0x2E SUBEQ + 0x2D ADDEQ：for 循环 = 60

    照官方样本 aifix.aardio 的 /0 pc=262-277 的形状拼：
      SUBEQ A=1  → 槽1 = 槽1 - 槽3（序言），跳到 ADDEQ
      ADDEQ A=1  → 槽1 += 槽3；越界就落空，否则跳回循环体
    循环体：槽5 = 槽1 * 10，累加进槽0。
    计数器 1、2、3 → 10 + 20 + 30 = 60。
    """
    c = Chunk(slots=8)
    c.load_int(0, 0)               # 槽0 = 累加和
    c.load_int(1, 1)               # 槽1 = 计数器（= 1）
    c.load_int(2, 3)               # 槽2 = 上限（= 3）
    c.load_int(3, 1)               # 槽3 = 步长（= 1）
    c.subeq(1, 8)                  # 槽1 = 1 - 1 = 0；跳到 ADDEQ（记录 8）
    # 记录 5..7：循环体
    c.load_int(6, 10)              # 槽6 = 10
    c.arith(0x1A, 5, 1, 6)         # 槽5 = 槽1 * 10
    c.arith(0x18, 0, 0, 5)         # 槽0 = 槽0 + 槽5
    c.addeq(1, 5)                  # 槽1 += 1；越界则落空，否则跳回记录 5
    c.ret(0)
    return c


def build_callmulti():
    """0x2F CALL_MULTI：被调值在槽 A，实参 = 槽 A+1/A+2，返回值写回槽 A+6

    官方 0x004C5BA2 块：把 (A, A+1, A+2) 上移 3 位 → (A+3, A+4, A+5)，
    以 A+3（= 原 A）为被调值调用；调用后若槽 A+3 非空 → 槽 A-1 := 槽 A+3。
    这里被调闭包返回 (self + 参数1) = 5 + 7 = 12。
    """
    body = Chunk(slots=4)
    body.arith(0x18, 2, 0, 1)     # 帧槽2 = 帧槽0(self) + 帧槽1(参数1)
    body.ret(2)

    c = Chunk(slots=10)
    c.children.append(body)
    c.new_closure(2, 0)           # 槽2 = 闭包（体 = 子对象 0）
    c.load_int(3, 5)              # 槽3 = self
    c.load_int(4, 7)              # 槽4 = 参数1
    # 0x2F：被调 = 槽2；实参 = 槽3、槽4（2 个）；返回值 → 槽8
    #      搬运：槽5 = 槽2、槽6 = 槽3、槽7 = 槽4
    #      调用后：槽1 (= A-1) := 槽5；并按下一条 0x22 跳转
    c.call_multi(2, 2, 5)         # 0x2F + 紧跟的 0x22（落点 = 记录 5）
    c.ret(8)
    return c


def build_callvalue():
    """0x30 CALL_VALUE：槽 A+1 是函数 → 以 0 个实参调用，返回值写回槽 A+2 = 42

    官方 0x004C661C 块：先判槽 A+1 的类型标签是不是 6（函数），
    是则调用并把返回值写进槽 A+2。
    """
    body = Chunk(slots=2)
    body.load_int(0, 42)          # 帧槽0 = 42
    body.ret(0)

    c = Chunk(slots=6)
    c.children.append(body)
    c.new_closure(1, 0)           # 槽1 = 闭包
    c.call_value(0)               # 0x30：被调 = 槽1；返回值 → 槽2
    c.ret(2)
    return c


def build_callns():
    """0x31 CALL_NS：槽 A 是表 → 取第一个非空成员的键 = "v"

    官方 0x004C5B40 块：槽 A 是表时调用 FUN_004C00C0（先扫数组部分再扫哈希部分），
    第一个非空成员的键写进槽 A+3 再拷到槽 A+1，成员值写进槽 A+4；
    找到就按下一条 0x22 的位移跳转。
    """
    c = Chunk(slots=8)
    key = c.add_const("v")
    c.new_native(0)                # 槽0 = 新命名空间表
    c.load_int(1, 42)              # 槽1 = 42
    c.set_member3(0, key, 1)       # ns.v = 42
    c.call_ns(0, 5)                # 0x31 + 紧跟的 0x22（落点 = 记录 5）
    c.ret(1)                       # 槽1 = 第一个非空成员的键 = "v"
    return c


def build_unsupported():
    """故意用一条**非法**记录种类（0x3D，官方只有 0x00..0x3C）→ 必须明确报错

    第二十轮之后 61 种记录全部落地，「未实现的种类」这条路已经走不到；
    这里改用越界种类验证「非法种类必须明确报错」，而不是静默跳过。
    记录字的种类字段只有 6 位（0..0x3F），官方只定义了 0x00..0x3C 共 61 种。
    """
    c = Chunk(slots=4)
    c.load_int(0, 1)
    c.emit(rec(0x3D, a=1, c=0, b=0))   # 0x3D：超出官方 61 种记录的范围
    c.ret(1)
    return c


BUILDERS = {
    "add": build_add,
    "loop": build_loop,
    "bits": build_bits,
    "str": build_str,
    "neg": build_neg,
    "sub": build_sub,
    "div": build_div,
    "shl": build_shl,
    "member": build_member,
    "member2": build_member2,
    "member3": build_member3,
    "index": build_index,
    "method": build_method,
    "array": build_array,
    "call": build_call,
    "params": build_params,
    "upvalue": build_upvalue,
    "recurse": build_recurse,
    "new_native": build_new_native,
    "import": build_import,
    "import_new": build_import_new,
    "concat": build_concat,
    "callmulti": build_callmulti,
    "callvalue": build_callvalue,
    "callns": build_callns,
    "forloop": build_forloop,
    "unsupported": build_unsupported,
}


def main(argv):
    outdir = argv[1] if len(argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    for name, fn in BUILDERS.items():
        path = os.path.join(outdir, name + ".aardio")
        with open(path, "wb") as f:
            f.write(fn().bytes())
        print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
