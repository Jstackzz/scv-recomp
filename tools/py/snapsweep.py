"""Two games' netplay snapshots of every check frame both dumped (SC5_NET_DUMP_DIR: net1_<f> and net2_<f>, .bin and
.blocks) compared word by word, matched by the guest address each block came from, leaving out what battle.cpp's
FightHash does (the poses, the known raw pointers, heap, stack) and -0.0 against +0.0: each word that differs, by
address, with the first frame and values, marked "pointer" when the two differ by exactly the heap shift game 2 ran
with (linktest.py --heap-shift2), else "other" (a value two PCs would compute apart).
    python -I tools/py/snapsweep.py <dumps dir> [shift MB, default 64]"""
import collections
import glob
import os
import re
import struct
import sys

d = sys.argv[1]
shift = int(float(sys.argv[2]) * (1 << 20)) if len(sys.argv) > 2 else 64 << 20
PLAYERS = {0x8294AC10: "P1", 0x829A8740: "P2"}
PSIZE = 0x5DB30
PTRS = [0x22638, 0x5BE90, 0x5CB44, 0x5CBF4, 0x5CCA4, 0x5CD54, 0x5CE04, 0x5CEB4, 0x5D2B4, 0x5D30C, 0x5D364, 0x5D3BC,
        0x5DACC, 0x5DAD0]


def counted(a):
    for base in PLAYERS:
        if base <= a < base + PSIZE:
            o = a - base
            for k in range(3):  # each player's buffers lie on round addresses: P2's 0x30 lower and 0x50 higher
                lo = (0x3190 if base == 0x8294AC10 else 0x3160) + k * 0x8000
                if lo <= o < lo + 6208:
                    return False
                dr = (0x1B310 if base == 0x8294AC10 else 0x1B360) + k * 0x800
                if dr <= o < dr + 0x800:
                    return False
            return not (0x22638 <= o < 0x5DAD4 and any(p <= o < p + 4 for p in PTRS))
    if 0x82A08248 <= a < 0x82A08250:
        return False
    if 0x82A09170 <= a < 0x82A09170 + 16 * 64 and (a - 0x82A09170) % 64 < 8:
        return False
    if 0x82A06ED0 <= a < 0x82A06ED0 + 880 and 16 <= (a - 0x82A06ED0) % 176 < 20:
        return False
    if (0x82A090C8 <= a < 0x82A090D0 or 0x82A09574 <= a < 0x82A09578 or 0x82A09170 <= a < 0x82A09178
            or 0x8277B81C <= a < 0x8277B820 or 0x8277B940 <= a < 0x8277B948):
        return False
    if 0x40000000 <= a < 0x80000000 or a >= 0xA0000000:
        return False
    return True


def where(a):
    for base, name in PLAYERS.items():
        if base <= a < base + PSIZE:
            return f"{name}+{a - base:05X}"
    return f"{a:08X}"


def load(path):
    data = open(path + ".bin", "rb").read()
    blocks = [tuple(int(x, 16) if i == 2 else int(x) for i, x in enumerate(l.split())) for l in open(path + ".blocks")]
    return data, blocks


found = collections.OrderedDict()
frames = sorted(int(re.search(r"_(\d+)\.bin$", p).group(1)) for p in glob.glob(os.path.join(d, "net1_*.bin")))
compared = 0
first_other = None
for f in frames:
    p1, p2 = os.path.join(d, f"net1_{f}"), os.path.join(d, f"net2_{f}")
    if not os.path.exists(p2 + ".bin"):
        continue
    compared += 1
    (d1, b1), (d2, b2) = load(p1), load(p2)
    by = {}
    for pos, size, src in b2:
        by.setdefault((src, size), []).append(pos)
    for pos, size, src in b1:
        c = by.get((src, size))
        if not c:
            continue
        q = c.pop(0)
        x, y = d1[pos:pos + size], d2[q:q + size]
        if x == y:
            continue
        for i in range(0, size - size % 4, 4):
            a = src + i
            if x[i:i + 4] != y[i:i + 4] and counted(a):
                u1, u2 = struct.unpack(">I", x[i:i + 4])[0], struct.unpack(">I", y[i:i + 4])[0]
                if {u1, u2} == {0x80000000, 0}:  # -0.0 and +0.0: the check takes them as one (battle.cpp)
                    continue
                kind = "pointer" if (u2 - u1) & 0xFFFFFFFF == shift else "other"
                key = (where(a), kind)
                if key not in found:
                    found[key] = (f, u1, u2, 0)
                ff, a1, a2, n = found[key]
                found[key] = (ff, a1, a2, n + 1)
                if kind == "other" and first_other is None:
                    first_other = (f, where(a), u1, u2)
print(f"{compared} check frames compared")
items = list(found.items())
for (w, kind), (f, u1, u2, n) in [x for x in items if x[0][1] == "pointer"]:
    print(f"  {w:>14} {kind:7} first at frame {f}: {u1:08X} / {u2:08X}, in {n} frames")
others = [x for x in items if x[0][1] == "other"]
for (w, kind), (f, u1, u2, n) in others[:25]:
    print(f"  {w:>14} {kind:7} first at frame {f}: {u1:08X} / {u2:08X}, in {n} frames")
if len(others) > 25:
    print(f"  ... {len(others) - 25} more other words")
if first_other:
    print("first other difference:", first_other)
