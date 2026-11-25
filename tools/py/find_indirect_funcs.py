"""Functions only reached through code pointers that the codegen scanner did not find: leaf thunks and adjustors
without .pdata entries. Adapted from mk9-rollback/tools/find_vtable_funcs.py; the section layout comes from the
image's own PE header instead of fixed addresses.

Two sources of pointers:
  data  words in .rdata/.data that point into code (vtables, callback tables), in runs of at least two that are
        not jump tables (most entries inside one .pdata function), or alone when --single is given
  code  addresses the code builds itself: lis rX,HI then addi/ori rY,rX,LO within a few instructions
        (a callback handed to a library, e.g. CRI's at 0x825F0438)

A target is kept when it is not a known function start, lies outside every .pdata function, and the instruction
before it ends something (blr, bctr, an unconditional b, or padding). Targets inside a .pdata function are labels
(jump tables, exception handlers) and are left alone.

Usage: python tools/py/find_indirect_funcs.py <image.bin> <generated_dir> <out.toml> [--single]
"""
import bisect
import os
import re
import struct
import sys

BASE = 0x82000000


def sections(b):
    lfanew = struct.unpack_from("<I", b, 0x3C)[0]
    n = struct.unpack_from("<H", b, lfanew + 6)[0]
    opt = struct.unpack_from("<H", b, lfanew + 20)[0]
    out = {}
    for i in range(n):
        s = b[lfanew + 24 + opt + 40 * i:lfanew + 24 + opt + 40 * (i + 1)]
        name = s[:8].rstrip(b"\0").decode("latin-1")
        vsize, va = struct.unpack_from("<II", s, 8)
        flags = struct.unpack_from("<I", s, 36)[0]
        out[name] = (BASE + va, BASE + va + vsize, flags)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    single = "--single" in sys.argv
    image, gen, out = args[:3]
    b = open(image, "rb").read()
    secs = sections(b)
    code = [(lo, hi) for lo, hi, fl in secs.values() if fl & 0x20]          # IMAGE_SCN_CNT_CODE
    data = [(lo, hi) for name, (lo, hi, fl) in secs.items() if name in (".rdata", ".data")]
    init = next(f for f in os.listdir(gen) if f.endswith("_init.cpp"))
    starts = set(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-F]{8}), ", open(os.path.join(gen, init)).read()))

    def word(a):
        return struct.unpack_from(">I", b, a - BASE)[0]

    def is_code_ptr(w):
        return w % 4 == 0 and any(lo <= w < hi for lo, hi in code)

    def terminator(w):
        return w in (0x4E800020, 0x4E800420, 0) or ((w >> 26) == 18 and (w & 1) == 0)

    plo, phi, _ = secs[".pdata"]
    pdata = []
    for a in range(plo, phi, 8):
        beg, w2 = struct.unpack_from(">II", b, a - BASE)
        if beg:
            pdata.append((beg, beg + ((w2 >> 8) & 0x3FFFFF) * 4))
    pdata.sort()
    pstarts = [p[0] for p in pdata]
    pends = set(p[1] for p in pdata)

    def func_index(t):
        k = bisect.bisect_right(pstarts, t) - 1
        if k >= 0 and pdata[k][0] <= t < pdata[k][1]:
            return k
        return -1 - t

    def wanted(t):
        # a switch table right after its bctr (lis/addi; lwzx; mtctr; bctr; table) holds code addresses, not code
        # (where a .pdata function ends another may begin with no padding, after a call that never returns:
        # title update 3's 0x822A6970 after "bl 0x8222ECC8")
        return (t not in starts and word(t) != 0 and not is_code_ptr(word(t)) and func_index(t) < 0
                and (terminator(word(t - 4)) or t in pends))

    found, jt = {}, 0
    # pointers in data
    for lo, hi in data:
        a = lo
        while a + 4 <= hi:
            if not is_code_ptr(word(a)):
                a += 4
                continue
            run_lo = a
            while a + 4 <= hi and is_code_ptr(word(a)):
                a += 4
            run = [(p, word(p)) for p in range(run_lo, a, 4)]
            if len(run) < 2:
                if single and wanted(run[0][1]):
                    found.setdefault(run[0][1], f"data 0x{run[0][0]:08X}")
                continue
            counts = {}
            for _, t in run:
                counts[func_index(t)] = counts.get(func_index(t), 0) + 1
            if max(counts.values()) * 4 >= len(run) * 3 and max(counts, key=counts.get) >= 0:
                jt += 1
                continue
            for p, t in run:
                if wanted(t):
                    found.setdefault(t, f"data 0x{p:08X}")
    # addresses built in code: lis rX,HI ... addi/ori rY,rX,LO
    built = 0
    for lo, hi in code:
        for a in range(lo, hi - 4, 4):
            w = word(a)
            if (w >> 26) != 15 or (w >> 16) & 31:                              # lis rX,HI (addis rX,0,HI)
                continue
            rx, high = (w >> 21) & 31, (w & 0xFFFF) << 16
            for k in range(1, 9):
                if a + 4 * k >= hi:
                    break
                v = word(a + 4 * k)
                op = v >> 26
                if op == 14 and (v >> 16) & 31 == rx:                          # addi rY,rX,LO
                    t = (high + ((v & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                elif op == 24 and (v >> 21) & 31 == rx:                        # ori rY,rX,LO
                    t = high | (v & 0xFFFF)
                else:
                    if op in (16, 18, 19) or (op == 15 and (v >> 21) & 31 == rx):
                        break                                                  # a branch or rX set again
                    continue
                if is_code_ptr(t):
                    built += 1
                    # the base of a computed jump just past its own bctr (lis/addi; add r12,r12,r0; mtctr; bctr;
                    # case code) is a label inside the function
                    switch_base = word(t - 4) == 0x4E800420 and 0 < t - (a + 4 * k) <= 0x20
                    if wanted(t) and not switch_base:
                        found.setdefault(t, f"code 0x{a + 4 * k:08X}")
                break
    print(f"{len(pdata)} .pdata functions, {len(starts)} known starts, skipped {jt} jump-table-like runs, "
          f"{built} code-built code addresses")
    with open(out, "w", newline="\n") as f:
        f.write("# Functions only reached through code pointers (vtables, callback tables, addresses built in code):\n")
        f.write("# leaf thunks and adjustors without .pdata. Generated by tools/py/find_indirect_funcs.py from the\n")
        f.write("# title update 3 image; regenerate after a codegen change.\n")
        f.write("[functions]\n")
        for t in sorted(found):
            f.write(f"0x{t:08X} = {{}}   # from {found[t]}\n")
    print(f"{len(found)} functions written to {out}")


if __name__ == "__main__":
    main()
