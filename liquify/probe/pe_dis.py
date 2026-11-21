#!/usr/bin/env python3
"""Offline static helper for sai2.exe (the SAI Ver.2 canvas engine).

The probe work in docs/20-canvas-capture.md needs three things that are all
pure static analysis: correct function boundaries from .pdata, callers of a
given function, and the instructions that touch a struct field.  Doing it by
eye in objdump output is what produced the wrong boundary last round, so this
script builds the model once and answers questions about it.

Usage:
    python3 pe_dis.py funcs                 # list functions around a range
    python3 pe_dis.py containing <addr>...
    python3 pe_dis.py callers <addr|name>   # direct call sites of a function
    python3 pe_dis.py dis <start> <end>     # disassemble (hex addrs)
    python3 pe_dis.py xref-imm <addr>       # who references an absolute addr
    python3 pe_dis.py xref-field <disp>     # who uses [reg+disp] (e.g. 0x18)
"""
import struct
import sys

import capstone
import pefile

BASE = 0x140000000
PATH = __file__.rsplit("/", 1)[0] + "/../notes/bin/sai2.exe"


class Image:
    def __init__(self, path=PATH):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = open(path, "rb").read()
        self.sections = []
        for s in self.pe.sections:
            self.sections.append(
                (s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
                 s.SizeOfRawData, s.Name.rstrip(b"\0").decode("ascii", "replace")))
        self.funcs = self._load_pdata()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    # -- address translation -------------------------------------------------
    def off(self, va):
        rva = va - self.base
        for sva, vsz, praw, rsz, _ in self.sections:
            if sva <= rva < sva + max(vsz, rsz):
                return praw + (rva - sva)
        raise KeyError(hex(va))

    def read(self, va, n):
        o = self.off(va)
        return self.data[o:o + n]

    # -- function table ------------------------------------------------------
    def _load_pdata(self):
        """Real function table, coalescing x64 chained unwind entries.

        MSVC splits one function into several RUNTIME_FUNCTION records whose
        UNWIND_INFO carries UNW_FLAG_CHAININFO; only the first record's
        BeginAddress is a real function entry. Treating every record as its own
        function is what produced the bogus 0x1402552D9 boundary in the notes,
        so chained records are folded into the preceding record here.
        """
        dd = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        if not dd.VirtualAddress:
            return []
        raw = self.read(self.base + dd.VirtualAddress, dd.Size)
        out = []
        for i in range(0, len(raw) - 11, 12):
            begin, end, unwind = struct.unpack_from("<III", raw, i)
            if begin == 0 or end <= begin:
                continue
            chained = False
            if unwind:
                try:
                    # UNWIND_INFO packs Version:3 | Flags:5, and
                    # UNW_FLAG_CHAININFO (0x4) therefore lands on bit 5.
                    chained = bool(self.read(self.base + unwind, 1)[0] & 0x20)
                except KeyError:
                    chained = False
            if chained and out:
                ob, oe = out[-1]
                out[-1] = (ob, max(oe, end))
            else:
                out.append((begin, end))
        return out

    def containing(self, va):
        """Innermost function whose [begin,end) covers va, as absolute addrs."""
        rva = va - self.base
        best = None
        for b, e in self.funcs:
            if b <= rva < e:
                best = (b, e)
                break
        if not best:
            return None
        return (self.base + best[0], self.base + best[1])

    def func_name(self, va):
        c = self.containing(va)
        return f"sub_{c[0]:X}" if c else "?"

    # -- code walking --------------------------------------------------------
    def insns(self, start, end):
        return self.md.disasm(self.read(start, end - start), start)

    def call_sites(self, target_va):
        """Direct call/jmp rel32 to target, plus [rip+x] IAT thunks."""
        hits = []
        for b, e in self.funcs:
            begin, end = self.base + b, self.base + e
            for ins in self.insns(begin, end):
                if ins.mnemonic not in ("call", "jmp"):
                    continue
                op = ins.operands[0] if ins.operands else None
                if op is None:
                    continue
                if op.type == capstone.x86.X86_OP_IMM and op.imm == target_va:
                    hits.append((ins.address, self.func_name(ins.address), ins.op_str))
        return hits


def parse_addr(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s, 16)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    img = Image()
    cmd = argv[1]

    if cmd == "funcs":
        lo = parse_addr(argv[2]) if len(argv) > 2 else img.base
        hi = parse_addr(argv[3]) if len(argv) > 3 else 0xFFFFFFFFFFFFFFFF
        for b, e in img.funcs:
            va = img.base + b
            if lo <= va <= hi:
                print(f"{va:012X} - {img.base+e:012X}  ({e-b:6d} B)")
        print(f"total functions: {len(img.funcs)}")
        return 0

    if cmd == "containing":
        for a in argv[2:]:
            va = parse_addr(a)
            c = img.containing(va)
            print(f"{va:012X} -> {c[0]:012X} .. {c[1]:012X} "
                  f"({c[1]-c[0]} B)" if c else f"{va:012X} -> not found")
        return 0

    if cmd == "dis":
        start, end = parse_addr(argv[2]), parse_addr(argv[3])
        for ins in img.insns(start, end):
            print(f"{ins.address:012X}  {ins.bytes.hex():<24s} {ins.mnemonic} {ins.op_str}")
        return 0

    if cmd == "callers":
        target = parse_addr(argv[2])
        hits = img.call_sites(target)
        for a, f, txt in hits:
            print(f"{a:012X} in {f}: {txt}")
        print(f"{len(hits)} direct call sites")
        return 0

    if cmd == "chain":
        return chain_cmd(img, argv[2])

    if cmd == "xref-imm":
        target = parse_addr(argv[2])
        n = 0
        for b, e in img.funcs:
            for ins in img.insns(img.base + b, img.base + e):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_IMM and op.imm == target:
                        print(f"{ins.address:012X} in {img.func_name(ins.address)}: "
                              f"{ins.mnemonic} {ins.op_str}")
                        n += 1
        print(f"{n} hits")
        return 0

    if cmd == "xref-field":
        disp = int(argv[2], 0)
        n = 0
        for b, e in img.funcs:
            for ins in img.insns(img.base + b, img.base + e):
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.disp == disp:
                        print(f"{ins.address:012X} in {img.func_name(ins.address)}: "
                              f"{ins.mnemonic} {ins.op_str}")
                        n += 1
        print(f"{n} hits")
        return 0

    print(__doc__)
    return 1



# --- stack chain reconstruction ---------------------------------------------
def _call_target_at(img, ret_va):
    """If the instruction ending at ret_va is call/jmp, return its immediate
    target; None otherwise. Tries the three plausible encodings."""
    for back in (2, 5, 6, 7):
        start = ret_va - back
        try:
            ins = next(img.insns(start, ret_va + 1))
        except (StopIteration, KeyError):
            continue
        if ins.address != start or ins.address + ins.size != ret_va:
            continue
        if ins.mnemonic not in ("call", "jmp"):
            continue
        op = ins.operands[0] if ins.operands else None
        if op and op.type == capstone.x86.X86_OP_IMM:
            return op.imm
    return None


def chain_cmd(img, path):
    entries = []
    immediate = None
    for line in open(path):
        if line.startswith("immediate_ret="):
            immediate = int(line.split()[0].split("=")[1], 16)
            continue
        if not line.startswith("frame "):
            continue
        parts = line.split()
        off = int(parts[1].lstrip("+"), 16)
        entries.append((off, int(parts[2], 16)))
    entries.sort()
    if not entries or immediate is None:
        print("no frames")
        return 1
    print(f"{'off':>6} {'return addr':>14} {'function':>14} {'size':>6}  how")
    c = img.containing(immediate)
    print(f"{'':>6} {immediate:014X} {c[0]:014X} {c[1]-c[0]:6d}  immediate (SetDIBitsToDevice)")
    cur_off = 0
    start = c[0]
    depth = 0
    while depth < 24:
        found = None
        for off, a in entries:
            if off <= cur_off:
                continue
            if _call_target_at(img, a) == start:
                found = (off, a)
                break
        if not found:
            print("  ... no consistent caller found")
            break
        depth += 1
        cur_off, cur = found
        c = img.containing(cur)
        print(f"{cur_off:6x} {cur:014X} {c[0]:014X} {c[1]-c[0]:6d}  call->{start:014X}")
        start = c[0]
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
