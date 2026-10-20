#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aardio VM instruction decoder for '=aardio' precompiled chunks.

The aardio VM is a 32-bit RISC.  One instruction = one u32 word.  The field
layout below is taken verbatim from the **official runtime interpreter**
FUN_004C4BC0 (aardio.exe image VA 0x004C4BC0, size 7263 bytes) -- the only
function in the binary that does `switch (word & 0x3F)` and whose cases cover
all 61 legal record kinds 0x00..0x3C:

    bits  0.. 5 : kind     (6 bit)   0x00..0x3C
    bits  6..13 : A        (8 bit)   destination local slot / branch condition
    bits 14..22 : C        (9 bit)   bit22 = global-scope flag, bits14..21 = index
    bits 23..31 : B        (9 bit)   bit31 = global-scope flag, bits23..30 = index

Branch records (0x22/0x23/0x24/0x25/0x26/0x27/0x28/0x2D/0x2E) use bits
14..31 as ONE signed 18 bit relative displacement (bias 0x1FFFE):

    IP = address_of_current_record + ((word >> 14) - 0x1FFFE)

The per-opcode names and effects live in tools/aardio_vm_runtime.py.

The 64 byte descriptor table below (image VA 0x7C80A0) is a **compile side**
artefact: it is referenced only by the compiler / chunk validator, never by the
runtime interpreter, so it must not be used to decode running code.

    bit 0..1 : operand format
               0 -> three operands  (A 8 bit, B 9 bit, C 9 bit)
               1 -> two operands    (A 8 bit, B 18 bit)
               2 -> two operands    (A 8 bit, B 18 bit, signed / PC relative)
               3 -> unused (reserved)
    bit 2..3 : reserved
    bit 4..5 : operand type
               0 -> constant pool index
               1 -> local / register index
               2 -> upvalue / outer index
               3 -> mixed (resolved by the format)
    bit 6    : instruction participates in control flow (branch target)
    bit 7    : operand C is negative (sign flag)
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aardio_vm_runtime import (  # noqa: E402
    FIELD_A,
    FIELD_B,
    FIELD_C,
    JUMP_KINDS,
    OPERAND_GLOBAL_BIT,
    REL18_BIAS,
    VM_KIND_COUNT,
    VM_KIND_MASK,
    check_opcode_table,
    decode_record,
    opcode_name,
)

# aardio.exe image VA 0x7C80A0, 64 bytes, one descriptor per opcode.
OPCODE_DESCRIPTORS = bytes((
    0x71, 0x54, 0x60, 0x50, 0x60, 0x71, 0x41, 0x71,
    0x60, 0x6C, 0x6C, 0x6C, 0x31, 0x31, 0x10, 0x20,
    0x30, 0x3C, 0x3C, 0x3C, 0x3C, 0x54, 0x38, 0x6C,
    0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x60, 0x60,
    0x60, 0x68, 0x22, 0xBC, 0xBC, 0xBC, 0xBC, 0xE4,
    0xE4, 0x28, 0x54, 0x54, 0x10, 0x62, 0x62, 0x84,
    0x00, 0x84, 0x14, 0x00, 0x51, 0x70, 0x7C, 0x7C,
    0x7C, 0x7C, 0x7C, 0x50,
))

FMT_THREE = 0
FMT_TWO = 1
FMT_TWO_REL = 2

OPTYPE_CONST = 0
OPTYPE_LOCAL = 1
OPTYPE_UPVALUE = 2
OPTYPE_MIXED = 3

OPTYPE_NAMES = {
    OPTYPE_CONST: "const",
    OPTYPE_LOCAL: "local",
    OPTYPE_UPVALUE: "upvalue",
    OPTYPE_MIXED: "mixed",
}


class Opcode:
    __slots__ = ("index", "descriptor")

    def __init__(self, index: int, descriptor: int):
        self.index = index
        self.descriptor = descriptor

    @property
    def fmt(self) -> int:
        return self.descriptor & 3

    @property
    def optype(self) -> int:
        return (self.descriptor >> 4) & 3

    @property
    def is_branch(self) -> bool:
        return bool(self.descriptor & 0x40)

    @property
    def negative(self) -> bool:
        return bool(self.descriptor & 0x80)


def opcode(index: int) -> Opcode:
    return Opcode(index, OPCODE_DESCRIPTORS[index])


def decode(word: int, pc: int = 0, next_word: int = 0):
    """Decode one 32-bit instruction word into a dict.

    The field extraction mirrors the official runtime interpreter
    FUN_004C4BC0 exactly.  ``next_word`` is the u32 at pc+1 — conditional
    branches take their displacement from there.  The compile-side
    descriptor is kept alongside as ``desc`` for tooling that wants it,
    but it is not authoritative.
    """
    out = decode_record(word, pc, next_word)
    out["op"] = out["kind"]
    desc = opcode(out["op"])
    out["desc"] = desc.descriptor
    out["fmt"] = desc.fmt
    out["optype"] = desc.optype
    out["branch"] = desc.is_branch
    return out


def format_instr(d) -> str:
    op = d["op"]
    parts = ["%-13s" % d["name"], "A=%-3d" % d["a"]]
    parts.append("B=%-3d%s" % (d["b"], "!" if d.get("b_global") else " "))
    parts.append("C=%-3d%s" % (d["c"], "!" if d.get("c_global") else " "))
    tags = []
    if d.get("b_global") or d.get("c_global"):
        tags.append("global-scope")
    if d["op"] in JUMP_KINDS:
        src = "next" if d.get("rel_from_next") else "own"
        tags.append("rel18=%d(%s)->%d" % (d["rel18"], src, d["target"]))
    return "%-13s %-26s ; %s" % (" ".join(parts), "", ",".join(tags) or "-")


def disasm(code):
    n = len(code)
    for pc, word in enumerate(code):
        nxt = code[pc + 1] if pc + 1 < n else 0
        yield pc, decode(word, pc, nxt)


def load_code(path: str):
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from aardio_bytecode import Reader, parse_chunk, HEADER_LEN, HEADER
    data = open(path, "rb").read()
    if not data.startswith(b"=aardio"):
        raise SystemExit("not an =aardio file")
    r = Reader(data, HEADER_LEN)
    return parse_chunk(r)


if __name__ == "__main__":
    check_opcode_table()
    root = load_code(sys.argv[1])
    print("root code: %d words" % len(root["code"]))
    for pc, d in list(disasm(root["code"]))[:60]:
        print("%5d  %08x  %s" % (pc, d["word"], format_instr(d)))
