#!/usr/bin/env python3
"""simulate_load.py -- relocate two WarpOS executables as the 68k stub would, and compare.

    simulate_load.py A B

Places every hunk of both files at the same made-up addresses, applies each file's
relocation table exactly as stub.s (and Elf2Exe2's stub) does -- including the
packed HA/LO pairs elf2warpos.py never writes -- and compares the loaded images of
every hunk but the stub and the table. Equal images mean the two files run the
same program: how elf2warpos.py's output is checked against Elf2Exe2's.
"""
import struct
import sys


def hunks_of(path):
    d = open(path, "rb").read()
    p = 0
    def u():
        nonlocal p
        v, = struct.unpack_from(">I", d, p)
        p += 4
        return v
    u()
    while u():
        pass
    u(); first = u(); last = u()
    [u() for _ in range(last - first + 1)]
    hunks = []
    while p < len(d):
        t = u() & 0x3FFFFFFF
        if t in (0x3E9, 0x3EA):
            n = u(); hunks.append(bytearray(d[p:p + n * 4])); p += n * 4
        elif t == 0x3EB:
            hunks.append(bytearray(u() * 4))
    return hunks


def load(path):
    hunks = hunks_of(path)
    base = [0x10000000 + i * 0x01000000 for i in range(len(hunks))]
    tab = bytes(hunks[-1])
    q = 0
    def g():
        nonlocal q
        v, = struct.unpack_from(">I", tab, q)
        q += 4
        return v
    while True:
        patch = g()
        if patch == 0:
            break
        h = hunks[patch]
        while True:
            cnt = g()
            if cnt == 0:
                break
            b = base[g()]
            for _ in range(cnt):
                e = g()
                if not e & 0x80000000:                      # ADDR32
                    v, = struct.unpack_from(">I", h, e)
                    struct.pack_into(">I", h, e, (v + b) & 0xFFFFFFFF)
                elif not e & 0x40000000:                    # HA/LO pair
                    ha = (e & 0x3FFFFF) << 2
                    lo = ha + (((e >> 22) & 0xFF) << 2)
                    v = (struct.unpack_from(">H", h, ha + 2)[0] << 16) | struct.unpack_from(">H", h, lo + 2)[0]
                    v = (v + b) & 0xFFFFFFFF
                    struct.pack_into(">H", h, lo + 2, v & 0xFFFF)
                    if v & 0x8000:
                        v = (v + 0x10000) & 0xFFFFFFFF
                    struct.pack_into(">H", h, ha + 2, v >> 16)
                elif e & 0x20000000:                        # REL24
                    off = e & 0x0FFFFFFF
                    insn, = struct.unpack_from(">I", h, off)
                    disp = ((insn & 0x03FFFFFC) + b - (base[patch] + off)) & 0x03FFFFFC
                    struct.pack_into(">I", h, off, (insn & 0xFC000003) | disp)
                elif e & 0x10000000:                        # LO
                    off = e & 0x0FFFFFFF
                    v, = struct.unpack_from(">H", h, off)
                    struct.pack_into(">H", h, off, (v + b) & 0xFFFF)
                else:                                       # HA + value
                    off = e & 0x0FFFFFFF
                    v = (g() + b) & 0xFFFFFFFF
                    if v & 0x8000:
                        v = (v + 0x10000) & 0xFFFFFFFF
                    struct.pack_into(">H", h, off, v >> 16)
    return hunks


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(a) != len(b):
        print("different hunk counts: %d vs %d" % (len(a), len(b)))
        return 1
    bad = 0
    for i in range(1, len(a) - 1):
        if a[i] != b[i]:
            n = min(len(a[i]), len(b[i]))
            diffs = [o for o in range(0, n, 4) if a[i][o:o + 4] != b[i][o:o + 4]]
            print("hunk %d differs: %d longwords (first at %#x), sizes %d/%d" %
                  (i, len(diffs), diffs[0] if diffs else -1, len(a[i]), len(b[i])))
            bad += 1
    print("loaded images: %s (%d hunks compared)" % ("IDENTICAL" if not bad else "DIFFERENT", len(a) - 2))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
