#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Summary statistics of an =aardio chunk tree."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aardio_bytecode import Reader, parse_chunk, HEADER_LEN, CONST_STRING, CONST_NUMBER, CONST_BOOL


def walk(node, depth=0, acc=None):
    acc = acc if acc is not None else {"objects": 0, "code": 0, "consts": 0, "strings": 0,
                                       "numbers": 0, "bools": 0, "nulls": 0, "maxdepth": 0}
    acc["objects"] += 1
    acc["code"] += len(node["code"])
    acc["consts"] += len(node["consts"])
    acc["maxdepth"] = max(acc["maxdepth"], depth)
    for c in node["consts"]:
        t = c["type"]
        if t == CONST_STRING:
            acc["strings"] += 1
        elif t == CONST_NUMBER:
            acc["numbers"] += 1
        elif t == CONST_BOOL:
            acc["bools"] += 1
        else:
            acc["nulls"] += 1
    for o in node["objects"]:
        walk(o, depth + 1, acc)
    return acc


def main(path):
    data = open(path, "rb").read()
    r = Reader(data, HEADER_LEN)
    root = parse_chunk(r)
    acc = walk(root)
    print("file            : %s" % path)
    print("size            : %d bytes" % len(data))
    print("header          : %s" % data[:HEADER_LEN].hex())
    print("objects (total) : %d" % acc["objects"])
    print("max depth       : %d" % acc["maxdepth"])
    print("code words      : %d" % acc["code"])
    print("constants       : %d  (strings %d, numbers %d, bools %d, null %d)" % (
        acc["consts"], acc["strings"], acc["numbers"], acc["bools"], acc["nulls"]))
    print("root code words : %d" % len(root["code"]))
    print("root consts     : %d" % len(root["consts"]))
    print("root objects    : %d" % len(root["objects"]))
    for i, o in enumerate(root["objects"]):
        print("  obj[%d] @%d code=%d consts=%d objects=%d" % (
            i, o["off"], len(o["code"]), len(o["consts"]), len(o["objects"])))


if __name__ == "__main__":
    main(sys.argv[1])
