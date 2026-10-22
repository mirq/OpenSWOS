#!/usr/bin/env python3
"""elf2warpos.py -- a WarpOS executable from a relocatable PowerPC ELF, on Linux.

    elf2warpos.py [--stack BYTES] IN.warpelf OUT   convert (stack: at least this, default 1 MB)
    elf2warpos.py --dump EXE               the hunks and relocation table of a WarpOS exe

The input is what Makefile.warpos links: an ET_REL ELF (ppc-amigaos-gcc + MOS2WOS,
`ld -r`, startupwos.o first so _start is at .text+0) with only the relocations
R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HA and R_PPC_REL24. The output is the
hunk file Elf2Exe2 makes from the same input -- same layout, same relocation table
format, same PPCArgs to startupwos.o -- so that conversion no longer needs the Amiga:

    hunk 0       CODE  our 68k stub (stub.s): opens powerpc.library, relocates, RunPPC
    hunk 1       BSS   empty: ELF section 0
    hunk 2..n-2  DATA/BSS: every allocated ELF section, in section order (.text first)
    hunk n-1     DATA  the relocation table the stub applies (format in stub.s)

Why: Elf2Exe2 runs only on the Amiga (it needs ixemul) and crashed with a PowerPC
exception, every time, on one build of the game it had converted the build before
(2026-10-22). Branches within one section are resolved here, as Elf2Exe2 does; every
other relocation goes in the table. A relocation this does not know is an error, not
a guess.
"""
import struct
import sys

# stub.s assembled (its header says how). 68020, position-independent, no relocations.
STUB = bytes.fromhex(
    "48e77ffe4bfa039a2c7800042b8e0170000000002b880170000000042b8001700000000842b5"
    "01700000001093c94eaefeda26404aab00ac661841eb005c4eaefe8041eb005c4eaefe8c2b80"
    "01700000001043fa037c700e4eaefdd82b8001700000001466067e14600002ac41faff8e7001"
    "22106708e5892041528060f42b8001700000000ce5889fc0284f203501700000000c5380224f"
    "41faff6622c82228fffce5895881204151c8fff2203501700000000ce588223c000100012c75"
    "0170000000004eaefd542b8001700000001c670002687c01203501700000000c5380bc80644a"
    "24746c002a2afff851856f3a224a4eaefdea028000002000662c2005223c000120014eaefd54"
    "4a806700022c207501700000001c21806c0029806c00204a224020054eaefd90528660a82035"
    "01700000000c538020740c002018670000d224740c002e1867f2201826340c0024186a0000b0"
    "0802001e6700006c0802001d6600003c0802001c6700001602820fffffff38322800d8433584"
    "28006000008802820fffffff2818d8834a446a060684000100004844358428006000006a0282"
    "0fffffff47f228002813028403fffffcd883988b028403fffffc0293fc000003899360000042"
    "28020284003fffffe58c3a32480248452c024846e88e0286000003fcdc843a326802da834a45"
    "6a060685000100003585680248453585480260000006d7b2280053876600ff446000ff3642b5"
    "0170000000182c7501700000000093c94eaefeda2640202b003e90ab003ab0b501700000002c"
    "6436203501700000002c72004eaefd542b80017000000018671e41f50170000000202080d0b5"
    "01700000002c21400004214000084eaefd244fefff702eac000842af000442af000842af000c"
    "42af00102f7501700000000400142f7501700000001400182f7501700000001000342f750170"
    "000000080038204f2c750170000000144eaeffe22e2f00144fef009020350170000000186720"
    "2c7501700000000041f50170000000204eaefd2422750170000000184eaefd4e6100006e2035"
    "01700000000c4ff70c0022750170000000142c750170000000004eaefe622c75017000000000"
    "2435017000000010670a4eaeff7c22424eaefe8620074cdf7ffe4e7561000026203501700000"
    "000c4ff70c0022750170000000142c750170000000004eaefe627e1460b4203501700000001c"
    "67322c7501700000000024402c3501700000000c53866b0e20326c0067f622404eaefd4e60ee"
    "224a4eaefd4e42b501700000001c4e7500000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000053544b21706f77657270632e6c696272"
    "61727900")

STACK_MARK = b"STK!"         # v_stack in stub.s, which --stack replaces
DEFAULT_STACK = 1024 * 1024

HUNK_CODE, HUNK_DATA, HUNK_BSS, HUNK_END, HUNK_HEADER = 0x3E9, 0x3EA, 0x3EB, 0x3F2, 0x3F3
SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 1, 2, 8, 4
SHF_ALLOC = 0x2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HA, R_PPC_REL24 = 1, 4, 6, 10
RELOC_NAMES = {R_PPC_ADDR32: "R_PPC_ADDR32", R_PPC_ADDR16_LO: "R_PPC_ADDR16_LO",
               R_PPC_ADDR16_HA: "R_PPC_ADDR16_HA", R_PPC_REL24: "R_PPC_REL24"}
OFF_MAX = 0x0FFFFFFF        # an entry's offset field (28 bits)


class ConvertError(Exception):
    pass


def read_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise ConvertError("not a 32-bit big-endian ELF")
    e_type, e_machine = struct.unpack_from(">HH", data, 16)
    if e_type != 1 or e_machine != 20:
        raise ConvertError("not a relocatable PowerPC ELF (ET_REL, EM_PPC)")
    e_shoff, = struct.unpack_from(">I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", data, 0x2E)
    secs = []
    for i in range(e_shnum):
        (name, typ, flags, addr, off, size, link, info, align, entsize) = \
            struct.unpack_from(">IIIIIIIIII", data, e_shoff + i * e_shentsize)
        secs.append(dict(idx=i, name_off=name, type=typ, flags=flags, off=off, size=size,
                         link=link, info=info, align=align, entsize=entsize))
    strtab = secs[e_shstrndx]
    for s in secs:
        o = strtab["off"] + s["name_off"]
        s["name"] = data[o:data.index(b"\0", o)].decode()
    return secs


def read_symbols(data, secs):
    symtab = [s for s in secs if s["type"] == SHT_SYMTAB]
    if len(symtab) != 1:
        raise ConvertError("expected one symbol table, found %d" % len(symtab))
    st = symtab[0]
    strs = secs[st["link"]]
    syms = []
    for i in range(st["size"] // 16):
        name, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", data, st["off"] + i * 16)
        o = strs["off"] + name
        syms.append(dict(name=data[o:data.index(b"\0", o)].decode(), value=value, shndx=shndx))
    return syms


def stub_with_stack(stack):
    i = STUB.find(STACK_MARK)
    if i < 0 or STUB.find(STACK_MARK, i + 1) >= 0 or i % 2:
        raise ConvertError("the stub has no (or more than one) stack slot")
    return STUB[:i] + struct.pack(">I", stack) + STUB[i + 4:]


def convert(data, stack=DEFAULT_STACK):
    secs = read_elf(data)
    syms = read_symbols(data, secs)

    # Hunks: 0 the stub, 1 empty (ELF section 0), then every allocated section.
    alloc = [s for s in secs if s["flags"] & SHF_ALLOC and s["type"] in (SHT_PROGBITS, SHT_NOBITS)]
    other_alloc = [s for s in secs if s["flags"] & SHF_ALLOC and s not in alloc]
    if other_alloc:
        raise ConvertError("allocated sections of a kind this does not place: " +
                           ", ".join(s["name"] for s in other_alloc))
    if not alloc or alloc[0]["name"] != ".text":
        raise ConvertError(".text must be the first allocated section (_start at hunk 2 +0)")
    hunk_of = {s["idx"]: 2 + i for i, s in enumerate(alloc)}
    image = {s["idx"]: bytearray(data[s["off"]:s["off"] + s["size"]]) if s["type"] == SHT_PROGBITS
             else None for s in alloc}

    # table[patch_hunk][symbol_hunk] = [longwords]
    table = {}
    counts = dict.fromkeys(("static", "ADDR32", "HA", "LO", "REL24"), 0)

    for rs in secs:
        if rs["type"] != SHT_RELA:
            continue
        target = rs["info"]
        if target not in hunk_of:
            if secs[target]["flags"] & SHF_ALLOC:
                raise ConvertError("relocations for %s, which is not placed" % secs[target]["name"])
            continue                    # .rela.debug_* and the like: nothing to load
        tsec = secs[target]
        if tsec["type"] == SHT_NOBITS:
            raise ConvertError("relocations into %s, which has no contents" % tsec["name"])
        img = image[target]
        patch = hunk_of[target]
        for i in range(rs["size"] // 12):
            r_offset, r_info, r_addend = struct.unpack_from(">IIi", data, rs["off"] + i * 12)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            sym = syms[rsym]
            where = "%s+%#x (%s against %s)" % (tsec["name"], r_offset,
                                               RELOC_NAMES.get(rtype, "type %d" % rtype),
                                               sym["name"] or "section %d" % sym["shndx"])
            if sym["shndx"] in (SHN_UNDEF, SHN_COMMON):
                raise ConvertError("undefined or COMMON symbol at " + where)
            value = (sym["value"] + r_addend) & 0xFFFFFFFF
            absolute = sym["shndx"] == SHN_ABS
            if not absolute and sym["shndx"] not in hunk_of:
                raise ConvertError("symbol in a section that is not loaded at " + where)
            shunk = None if absolute else hunk_of[sym["shndx"]]

            def entry(word, extra=None):
                if r_offset > OFF_MAX:
                    raise ConvertError("offset too large for the table at " + where)
                lst = table.setdefault(patch, {}).setdefault(shunk, [])
                lst.append(word | r_offset)
                if extra is not None:
                    lst.append(extra)

            if rtype == R_PPC_ADDR32:
                struct.pack_into(">I", img, r_offset, value)
                if absolute:
                    counts["static"] += 1
                else:
                    if r_offset & 0x80000000:
                        raise ConvertError("offset too large at " + where)
                    lst = table.setdefault(patch, {}).setdefault(shunk, [])
                    lst.append(r_offset)
                    counts["ADDR32"] += 1
            elif rtype == R_PPC_ADDR16_LO:
                struct.pack_into(">H", img, r_offset, value & 0xFFFF)
                if absolute:
                    counts["static"] += 1
                else:
                    entry(0xD0000000)
                    counts["LO"] += 1
            elif rtype == R_PPC_ADDR16_HA:
                ha = ((value + 0x8000) >> 16) & 0xFFFF
                struct.pack_into(">H", img, r_offset, ha if absolute else (value >> 16))
                if absolute:
                    counts["static"] += 1
                else:
                    entry(0xC0000000, value)
                    counts["HA"] += 1
            elif rtype == R_PPC_REL24:
                insn, = struct.unpack_from(">I", img, r_offset)
                if not absolute and shunk == patch:
                    disp = (value - r_offset) & 0xFFFFFFFF
                    sdisp = disp - (1 << 32) if disp & 0x80000000 else disp
                    if sdisp < -0x2000000 or sdisp >= 0x2000000 or sdisp & 3:
                        raise ConvertError("branch out of range at " + where)
                    struct.pack_into(">I", img, r_offset, (insn & 0xFC000003) | (disp & 0x03FFFFFC))
                    counts["static"] += 1
                elif absolute:
                    raise ConvertError("a branch to an absolute address at " + where)
                else:
                    struct.pack_into(">I", img, r_offset, (insn & 0xFC000003) | (value & 0x03FFFFFC))
                    entry(0xE0000000)
                    counts["REL24"] += 1
            else:
                raise ConvertError("a relocation this converter does not know at " + where)

    reloc = bytearray()
    for patch in sorted(table):
        reloc += struct.pack(">I", patch)
        for shunk in sorted(table[patch]):
            words = table[patch][shunk]
            n = sum(1 for w in iter_entries(words))
            reloc += struct.pack(">II", n, shunk)
            reloc += b"".join(struct.pack(">I", w) for w in words)
        reloc += struct.pack(">I", 0)
    reloc += struct.pack(">I", 0)

    hunks = [(HUNK_CODE, stub_with_stack(stack)), (HUNK_BSS, 0)]
    for s in alloc:
        if s["type"] == SHT_NOBITS:
            hunks.append((HUNK_BSS, s["size"]))
        else:
            hunks.append((HUNK_DATA, bytes(image[s["idx"]])))
    hunks.append((HUNK_DATA, bytes(reloc)))
    return write_hunks(hunks), counts, len(alloc)


def iter_entries(words):
    """The entries of one group's longwords: an HA entry carries one more."""
    i = 0
    while i < len(words):
        w = words[i]
        yield w
        i += 2 if (w & 0xF0000000) == 0xC0000000 else 1


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def write_hunks(hunks):
    out = bytearray(struct.pack(">IIIII", HUNK_HEADER, 0, len(hunks), 0, len(hunks) - 1))
    for kind, body in hunks:
        size = body if kind == HUNK_BSS else len(pad4(body))
        out += struct.pack(">I", (size + 3) // 4)
    for kind, body in hunks:
        if kind == HUNK_BSS:
            out += struct.pack(">II", HUNK_BSS, (body + 3) // 4)
        else:
            b = pad4(body)
            out += struct.pack(">II", kind, len(b) // 4) + b
        out += struct.pack(">I", HUNK_END)
    return bytes(out)


def dump(data):
    p = 0
    def u():
        nonlocal p
        v, = struct.unpack_from(">I", data, p)
        p += 4
        return v
    if u() != HUNK_HEADER:
        raise ConvertError("not a hunk file")
    while u():
        pass
    u(); first = u(); last = u()
    [u() for _ in range(last - first + 1)]
    hunks = []
    while p < len(data):
        t = u() & 0x3FFFFFFF
        if t in (HUNK_CODE, HUNK_DATA):
            n = u(); hunks.append((t, data[p:p + n * 4])); p += n * 4
        elif t == HUNK_BSS:
            hunks.append((t, u() * 4))
        elif t != HUNK_END:
            raise ConvertError("hunk type %#x this does not read" % t)
    for i, (t, b) in enumerate(hunks):
        kind = {HUNK_CODE: "CODE", HUNK_DATA: "DATA", HUNK_BSS: "BSS"}[t]
        print("hunk %2d %-4s %8d bytes" % (i, kind, b if t == HUNK_BSS else len(b)))
    tab = hunks[-1][1]
    q = 0
    kinds = dict.fromkeys(("ADDR32", "HA/LO pair", "HA", "LO", "REL24"), 0)
    def g():
        nonlocal q
        v, = struct.unpack_from(">I", tab, q)
        q += 4
        return v
    while True:
        patch = g()
        if patch == 0:
            break
        while True:
            cnt = g()
            if cnt == 0:
                break
            g()
            for _ in range(cnt):
                e = g()
                if not e & 0x80000000: kinds["ADDR32"] += 1
                elif not e & 0x40000000: kinds["HA/LO pair"] += 1
                elif e & 0x20000000: kinds["REL24"] += 1
                elif e & 0x10000000: kinds["LO"] += 1
                else: kinds["HA"] += 1; g()
    print("relocation table: %d of %d bytes read; %s" % (q, len(tab), kinds))


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "--dump":
            dump(open(argv[2], "rb").read())
            return 0
        stack = DEFAULT_STACK
        if len(argv) >= 3 and argv[1] == "--stack":
            stack = int(argv[2], 0)
            argv = argv[:1] + argv[3:]
        if len(argv) != 3:
            print(__doc__.split("\n\n")[1], file=sys.stderr)
            return 2
        out, counts, nsec = convert(open(argv[1], "rb").read(), stack)
        open(argv[2], "wb").write(out)
        print("elf2warpos: %s -> %s, %d bytes: %d sections; resolved here %d, in the table "
              "%d ADDR32, %d HA, %d LO, %d REL24" %
              (argv[1], argv[2], len(out), nsec, counts["static"], counts["ADDR32"],
               counts["HA"], counts["LO"], counts["REL24"]))
        return 0
    except ConvertError as e:
        print("elf2warpos: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
