#!/usr/bin/env python3
"""Dump a PE's MENU / DIALOG / STRINGTABLE / ACCELERATOR resources.

Used to build the official-IDE capability checklist for the Nodio gap audit.
Plain PE parsing (no third-party dependency). Not REA evidence: this is an
independent read of the same file REA also inspected.
"""
import struct
import sys

RT_CURSOR, RT_BITMAP, RT_ICON, RT_MENU, RT_DIALOG = 1, 2, 3, 4, 5
RT_STRING, RT_ACCELERATOR, RT_GROUP_CURSOR, RT_GROUP_ICON = 6, 9, 12, 14
RT_VERSION, RT_MANIFEST = 16, 24

TYPE_NAMES = {RT_CURSOR: "CURSOR", RT_BITMAP: "BITMAP", RT_ICON: "ICON",
              RT_MENU: "MENU", RT_DIALOG: "DIALOG", RT_STRING: "STRING",
              RT_ACCELERATOR: "ACCEL", RT_GROUP_CURSOR: "GRP_CURSOR",
              RT_GROUP_ICON: "GRP_ICON", RT_VERSION: "VERSION",
              RT_MANIFEST: "MANIFEST"}


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def load_resources(path):
    data = open(path, "rb").read()
    pe = u32(data, 0x3C)
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE"
    coff = pe + 4
    nsec = u16(data, coff + 2)
    optsz = u16(data, coff + 16)
    opt = coff + 20
    magic = u16(data, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, rsrc_size = u32(data, dd + 2 * 8), u32(data, dd + 2 * 8 + 4)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        s = so + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rsize, raddr = u32(data, s + 8), u32(data, s + 12), u32(data, s + 16), u32(data, s + 20)
        secs.append((name, vaddr, vsize, raddr, rsize))

    def rva2off(rva):
        for _n, va, _vs, ra, _rs in secs:
            if va <= rva < va + max(_vs, _rs):
                return ra + (rva - va)
        return None

    root = rva2off(rsrc_rva)
    out = []

    def walk(off, level, path):
        nnamed, nid = u16(data, off + 12), u16(data, off + 14)
        n = nnamed + nid
        for i in range(n):
            e = off + 16 + i * 8
            nameid = u32(data, e)
            off2 = u32(data, e + 4)
            is_dir = bool(off2 & 0x80000000)
            off2 &= 0x7FFFFFFF
            if nameid & 0x80000000:
                no = root + (nameid & 0x7FFFFFFF)
                nlen = u16(data, no + 0)
                ename = data[no + 2:no + 2 + nlen * 2].decode("utf-16le")
            else:
                ename = nameid
            if level == 0:
                t = ename
            elif level == 1:
                t = ename
            else:
                t = ename
            if is_dir:
                walk(root + off2, level + 1, path + [t])
            else:
                leaf = root + off2
                drva, dsize = u32(data, leaf), u32(data, leaf + 4)
                out.append((path + [t], rva2off(drva), dsize))

    walk(root, 0, [])
    return data, out


def decode_menu(data, off, size, depth=0, acc=None):
    """Walk a Win32 MENU resource, returning (text, flags) leaves."""
    items = []
    o = off
    end = off + size
    while o + 8 <= end:
        flags = u16(data, o)
        # MfPopup = 0x10
        if flags & 0x10:
            sub_off = u32(data, o + 4)
            sub = decode_menu(data, sub_off, size, depth + 1)
            items.append((f"[POPUP {sub[0]}]", sub[1:]))
            o += 8
        else:
            txt = ""
            p = o + 8
            while p < end and data[p] != 0:
                if data[p] == 0xFF:
                    break
                p += 1
            raw = data[o + 8:p]
            try:
                txt = raw.decode("utf-16le")
            except Exception:
                txt = repr(raw)
            items.append((txt, flags))
            o = p + 2
            if flags & 0x80:  # MF_POPUP -> next word is the popup id
                o += 2
    return items


def flatten(items, prefix=""):
    out = []
    for it in items:
        if isinstance(it, tuple) and len(it) == 2 and isinstance(it[1], list):
            out.extend(flatten(it[1], prefix + it[0] + " > "))
        else:
            out.append((prefix + it[0], it[1]))
    return out


def main():
    path = sys.argv[1]
    data, res = load_resources(path)
    want = {int(x) for x in sys.argv[2:]} or {RT_MENU, RT_STRING, RT_ACCELERATOR}
    for path_parts, off, size in res:
        rtype = path_parts[0]
        if not isinstance(rtype, int) or rtype not in want:
            continue
        label = "/".join(str(p) for p in path_parts[1:])
        if rtype == RT_MENU:
            try:
                items = decode_menu(data, off, size)
            except Exception as e:
                print(f"== MENU {label}: decode error {e}")
                continue
            print(f"== MENU {label} ==")
            for txt, flags in flatten(items):
                if txt.strip():
                    print("   ", txt)
        elif rtype == RT_STRING:
            print(f"== STRINGTABLE {label} ==")
            o = off
            end = off + size
            base = (path_parts[1] - 1) * 16 if isinstance(path_parts[1], int) else 0
            while o + 2 <= end:
                slen = u16(data, o)
                o += 2
                s = data[o:o + slen * 2].decode("utf-16le", "replace")
                o += slen * 2
                if s.strip():
                    print(f"   [{base}] {s}")
                base += 1


if __name__ == "__main__":
    main()
