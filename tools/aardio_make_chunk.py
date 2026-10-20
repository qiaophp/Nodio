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
        """0x2A CALL：被调值在槽 A，实参窗口 = 槽 A+1..A+B-1，返回槽 = A+B。

        B 是**返回槽的偏移**（官方 0x2A 装参块 0x004C5A44 写 vm->0x08 = &slot[A+B]）；
        实参个数 = B-1（其中第一个是 self / 接收者）。
        """
        return self.emit(rec(0x2A, a=a, c=c, b=b))

    def load_upvalue(self, a: int, b: int) -> int:
        """0x03 LOAD_UPVALUE：目标槽 A，上值槽 B。"""
        return self.emit(rec(0x03, a=a, b=b))

    def store_upvalue(self, a: int, b: int) -> int:
        """0x0E STORE_UPVALUE：源槽 A，上值槽 B（按引用写入）。"""
        return self.emit(rec(0x0E, a=a, b=b))

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
    c.call(0, 4, 2)               # 实参 = 槽1..槽3；返回槽 = 槽4
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
    c.call(0, 3, 2)               # 实参 = 槽1..槽2；返回槽 = 槽3
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
    c.call(1, 2, 1)               # 被调 = 槽1；实参 = 槽2；返回槽 = 槽3
    c.local(4, 0)                 # 槽4 = 帧槽 0（应已被闭包改成 99）
    c.ret(4)
    return c


def build_unsupported():
    """故意用一条尚未实现的记录（NEW_NATIVE 0x15）→ 记录 VM 必须明确报错

    0x15 是官方「内建函数序号」记录（依赖内建表 DAT_007C89B0），
    第十六轮尚未落地；这里用它验证「未实现的种类必须明确报错」。
    """
    c = Chunk(slots=4)
    c.load_int(0, 1)
    c.emit(rec(0x15, a=1))      # NEW_NATIVE：记录 VM 尚未实现内建函数序号
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
