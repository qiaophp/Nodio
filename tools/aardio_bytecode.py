#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aardio '=aardio' precompiled-chunk reader / writer / verifier.

Format (reverse engineered from aardio.exe via REA + Ghidra, see
docs/aardio-bytecode.md for the evidence trail):

  chunk      := header15
                name        str
                attr1       i32
                attr2       i32
                flags       u8 x 5
                code        codeBlock
                consts      constBlock
                objects     objectBlock
                tableA      u32Block
                tableB      recordBlock
                tableC      strBlock

  header15   := "=aardio" 10 00 01 04 04 04 08 00
  str        := u32 len ; (len-1) bytes ; u8 flags
                len == 0 -> null string
                payload = text (len-2 bytes) + NUL ; flags & 0x10 -> wide text
  codeBlock  := u32 count ; count * u32 instruction
  constBlock := u32 count ; count * constant
  constant   := u8 type ; type-specific payload
                0 null      : (no payload)
                1 bool      : u8 value
                3 number    : 8 bytes (IEEE-754 double)
                4 string    : str
                (2 is rejected by the official loader: "bad constant")
  objectBlock:= u32 count ; count * chunk
  u32Block   := u32 count ; count * u32
  recordBlock:= u32 count ; count * (str, i32, i32)
  strBlock   := u32 count ; count * str

Instruction words (32-bit RISC) are laid out as

    bits  0.. 5 : opcode   (0..0x3c)
    bits  6..13 : operand A (8 bit)
    bits 14..22 : operand B (9 bit)
    bits 23..31 : operand C (9 bit)

The per-opcode operand format is taken from a 64-byte descriptor table in
aardio.exe (0x7C80A0); this reader keeps the raw word, the descriptor
interpreter lives in aardio_opcodes.py.

Usage:
    python aardio_bytecode.py <file>            # structural dump
    python aardio_bytecode.py <file> --verify   # re-serialize and compare
"""
from __future__ import annotations

import struct
import sys

MAGIC = b"=aardio"
HEADER = MAGIC + bytes((0x10, 0x00, 0x01, 0x04, 0x04, 0x04, 0x08, 0x00))
HEADER_LEN = len(HEADER)  # 15

CONST_NULL = 0
CONST_BOOL = 1
CONST_NUMBER = 3
CONST_STRING = 4


class AardioError(Exception):
    pass


class Reader:
    __slots__ = ("data", "pos")

    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def eof(self) -> bool:
        return self.pos >= len(self.data)

    def u8(self) -> int:
        if self.pos + 1 > len(self.data):
            raise AardioError("u8 read past EOF at %d" % self.pos)
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u32(self) -> int:
        if self.pos + 4 > len(self.data):
            raise AardioError("u32 read past EOF at %d" % self.pos)
        v = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def i32(self) -> int:
        v = self.u32()
        return v - (1 << 32) if v >= (1 << 31) else v

    def raw(self, n: int) -> bytes:
        if n < 0 or self.pos + n > len(self.data):
            raise AardioError("raw(%d) past EOF at %d" % (n, self.pos))
        v = self.data[self.pos:self.pos + n]
        self.pos += n
        return v

    def string(self):
        """str := u32 len ; (len-1) bytes ; u8 flags"""
        start = self.pos
        ln = self.u32()
        if ln == 0:
            return {"off": start, "len": 0, "text": None, "flags": None, "wide": False}
        if ln < 2:
            raise AardioError("bad string length %d at %d" % (ln, start))
        body = self.raw(ln - 1)
        flags = self.u8()
        text = body[:-1] if body.endswith(b"\x00") else body
        return {
            "off": start,
            "len": ln,
            "text": text,
            "flags": flags,
            "wide": bool(flags & 0x10),
            "raw": self.data[start:self.pos],
        }


def parse_chunk(r: Reader) -> dict:
    node = {"off": r.pos}
    node["name"] = r.string()
    node["attr1"] = r.i32()
    node["attr2"] = r.i32()
    node["flags"] = [r.u8() for _ in range(5)]

    # code block
    n = r.u32()
    node["code_off"] = r.pos
    node["code"] = [r.u32() for _ in range(n)]

    # constant block
    n = r.u32()
    node["consts"] = [parse_const(r) for _ in range(n)]

    # nested objects
    n = r.u32()
    node["objects"] = [parse_chunk(r) for _ in range(n)]

    # table A : u32 array
    n = r.u32()
    node["tableA"] = [r.u32() for _ in range(n)]

    # table B : (str, i32, i32) records
    n = r.u32()
    tableB = []
    for _ in range(n):
        s = r.string()
        a = r.i32()
        b = r.i32()
        tableB.append((s, a, b))
    node["tableB"] = tableB

    # table C : string array
    n = r.u32()
    node["tableC"] = [r.string() for _ in range(n)]

    node["end"] = r.pos
    return node


def parse_const(r: Reader) -> dict:
    start = r.pos
    t = r.u8()
    c = {"off": start, "type": t}
    if t == CONST_NULL:
        pass
    elif t == CONST_BOOL:
        c["value"] = r.u8()
    elif t == CONST_NUMBER:
        c["value"] = struct.unpack("<d", r.raw(8))[0]
    elif t == CONST_STRING:
        c["string"] = r.string()
    else:
        raise AardioError("bad constant type %d at %d" % (t, start))
    c["end"] = r.pos
    return c


# ---------------------------------------------------------------- re-emit ----

def emit_string(s) -> bytes:
    if s["len"] == 0:
        return struct.pack("<I", 0)
    return s["raw"]


def emit_chunk(node) -> bytes:
    out = bytearray()
    out += emit_string(node["name"])
    out += struct.pack("<i", node["attr1"])
    out += struct.pack("<i", node["attr2"])
    out += bytes(node["flags"])
    out += struct.pack("<I", len(node["code"]))
    out += struct.pack("<%dI" % len(node["code"]), *node["code"])
    out += struct.pack("<I", len(node["consts"]))
    for c in node["consts"]:
        out += bytes((c["type"],))
        if c["type"] == CONST_BOOL:
            out += bytes((c["value"],))
        elif c["type"] == CONST_NUMBER:
            out += struct.pack("<d", c["value"])
        elif c["type"] == CONST_STRING:
            out += emit_string(c["string"])
    out += struct.pack("<I", len(node["objects"]))
    for o in node["objects"]:
        out += emit_chunk(o)
    out += struct.pack("<I", len(node["tableA"]))
    out += struct.pack("<%dI" % len(node["tableA"]), *node["tableA"])
    out += struct.pack("<I", len(node["tableB"]))
    for s, a, b in node["tableB"]:
        out += emit_string(s)
        out += struct.pack("<i", a)
        out += struct.pack("<i", b)
    out += struct.pack("<I", len(node["tableC"]))
    for s in node["tableC"]:
        out += emit_string(s)
    return bytes(out)


# ------------------------------------------------------------------ dump -----

def s_text(s) -> str:
    if s["len"] == 0:
        return "<null>"
    t = s["text"]
    if t is None:
        return "<null>"
    try:
        return t.decode("utf-8")
    except UnicodeDecodeError:
        return repr(t)


def dump(node, depth=0, out=sys.stdout):
    pad = "  " * depth
    out.write("%sobject @%d name=%r attr1=%d attr2=%d flags=%s\n" % (
        pad, node["off"], s_text(node["name"]), node["attr1"], node["attr2"],
        ",".join("%02x" % f for f in node["flags"])))
    out.write("%s  code: %d words\n" % (pad, len(node["code"])))
    out.write("%s  consts: %d\n" % (pad, len(node["consts"])))
    for i, c in enumerate(node["consts"][:12]):
        if c["type"] == CONST_STRING:
            v = repr(s_text(c["string"]))
        elif c["type"] == CONST_NUMBER:
            v = repr(c["value"])
        elif c["type"] == CONST_BOOL:
            v = repr(bool(c["value"]))
        else:
            v = "null"
        out.write("%s    [%d] type=%d %s\n" % (pad, i, c["type"], v))
    if len(node["consts"]) > 12:
        out.write("%s    ... %d more\n" % (pad, len(node["consts"]) - 12))
    out.write("%s  objects: %d\n" % (pad, len(node["objects"])))
    for o in node["objects"]:
        dump(o, depth + 1, out)
    out.write("%s  tableA(u32): %d  tableB(rec): %d  tableC(str): %d\n" % (
        pad, len(node["tableA"]), len(node["tableB"]), len(node["tableC"])))
    for s, a, b in node["tableB"][:8]:
        out.write("%s    B: %r %d %d\n" % (pad, s_text(s), a, b))
    for s in node["tableC"][:8]:
        out.write("%s    C: %r\n" % (pad, s_text(s)))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    data = open(path, "rb").read()
    if not data.startswith(MAGIC):
        print("not an =aardio file: %s" % path)
        return 1
    if data[:HEADER_LEN] != HEADER:
        print("header mismatch:\n  file  %s\n  spec  %s" % (data[:HEADER_LEN].hex(), HEADER.hex()))
        return 1
    print("header OK: %s" % data[:HEADER_LEN].hex())
    r = Reader(data, HEADER_LEN)
    try:
        root = parse_chunk(r)
    except AardioError as e:
        print("parse error: %s" % e)
        return 1
    print("parsed root object @%d..%d (file size %d)" % (root["off"], root["end"], len(data)))
    print("trailing bytes: %d" % (len(data) - root["end"]))
    if "--verify" in argv:
        re = HEADER + emit_chunk(root)
        same = re == data
        print("re-serialize: %d bytes, identical=%s" % (len(re), same))
        if not same:
            for i in range(min(len(re), len(data))):
                if re[i] != data[i]:
                    print("  first difference at %d: got %02x want %02x" % (i, re[i], data[i]))
                    break
            return 1
    if "--dump" in argv:
        dump(root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
