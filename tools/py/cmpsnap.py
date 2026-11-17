"""Compare two games' battle snapshots, as the netplay dumps them for tests (SC5_NET_DUMP_AT=<frames> with
SC5_NET_DUMP_DIR: net1_m<match>at<frame>.bin / .blocks from player 1's game, net2_... from player 2's).

    python -I tools/py/cmpsnap.py <dump dir> <tag, e.g. m1at0> [shown=20]

Prints the blocks of the fight (battle.cpp's Kind::Fight, byte by byte: not the poses, heap objects, the stack, nor
the raw pointers) that differ, matched by their guest address (and how many times it came before, as a block's
address can repeat), with the first differing bytes and where in a player's object they are; then whether the fight's
bytes in snapshot order (what battle::FightHash hashes) are the same.
"""
import os
import sys
from collections import defaultdict

P1, P2, PSIZE = 0x82937070, 0x82994BA0, 0x5DB30
# battle.cpp's KindOf
POINTERS = {0x22638, 0x5CB44, 0x5CBF4, 0x5CCA4, 0x5CD54, 0x5CE04, 0x5CEB4, 0x5D2B4, 0x5D30C, 0x5D364, 0x5D3BC,
            0x5DACC, 0x5DAD0}


def kind(a):
    for p in (P1, P2):
        if any(p + b <= a < p + b + 6208 for b in (0x3190, 0xB190, 0x13190)):
            return "pose"
        if p + 0x22638 <= a < p + 0x5DAD4 and any(p + o <= a < p + o + 4 for o in POINTERS):
            return "pointer"
    if 0x829F552C <= a < 0x829F5530 or 0x829F59D4 <= a < 0x829F59D8:
        return "pointer"
    if 0x70000000 <= a < 0x80000000:
        return "stack"
    if 0x40000000 <= a < 0x70000000 or a >= 0xA0000000:
        return "heap"
    return "fight"


def load(d, n, tag):
    data = open(os.path.join(d, f"net{n}_{tag}.bin"), "rb").read()
    blocks = []
    for line in open(os.path.join(d, f"net{n}_{tag}.blocks")):
        pos, size, src = line.split()
        blocks.append((int(pos), int(size), int(src, 16)))
    return data, blocks


def fight_bytes(data, pos, size, src):
    return bytes(data[pos + i] for i in range(size) if kind(src + i) == "fight")


def where(a):
    for p, name in ((P1, "P1"), (P2, "P2")):
        if p <= a < p + PSIZE:
            return f" ({name}+{a - p:X})"
    return ""


def main():
    d, tag = sys.argv[1], sys.argv[2]
    shown = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    (a, ba), (b, bb) = load(d, 1, tag), load(d, 2, tag)
    print(f"{tag}: {len(a)} and {len(b)} bytes, {len(ba)} and {len(bb)} blocks")

    def by_address(data, blocks):
        seen, out = defaultdict(int), {}
        for pos, size, src in blocks:
            fb = fight_bytes(data, pos, size, src)
            if fb:
                out[(src, seen[src])] = (data[pos:pos + size], src)
                seen[src] += 1
        return out

    fa, fb = by_address(a, ba), by_address(b, bb)
    only = [k for k in fa if k not in fb] + [k for k in fb if k not in fa]
    differ = []
    for k in fa:
        if k not in fb:
            continue
        x, y = fa[k][0], fb[k][0]
        idx = [i for i in range(min(len(x), len(y))) if x[i] != y[i] and kind(k[0] + i) == "fight"]
        if idx:
            differ.append((k, idx))
    print(f"  fight blocks: {len(fa)} and {len(fb)}, in one only: {len(only)}, differing: {len(differ)}")
    for (src, occ), idx in sorted(differ)[:shown]:
        x, y = fa[(src, occ)][0], fb[(src, occ)][0]
        i = idx[0]
        print(f"   {src:08X}#{occ} size {len(x)}: {len(idx)} bytes, first at {src + i:08X}{where(src + i)}: "
              f"{x[i:i + 8].hex()} vs {y[i:i + 8].hex()}")
    for k in only[:5]:
        print(f"   in one only: {k[0]:08X}#{k[1]}")
    sa = [fight_bytes(a, *blk) for blk in ba]
    sb = [fight_bytes(b, *blk) for blk in bb]
    same = b"".join(sa) == b"".join(sb)
    print(f"  the fight's bytes in snapshot order (FightHash): {'the same' if same else 'differ'}")


if __name__ == "__main__":
    main()
