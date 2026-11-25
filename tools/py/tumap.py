"""Where the code and data of one build of the game are in another (title update 1 -> title update 3).

  python tools/py/tumap.py build <old.bin> <new.bin> <map>   match the two images, write the map, print a summary
  python tools/py/tumap.py q <map> <address> [...]           where an old address is in the new image
  python tools/py/tumap.py diff <map> <address> [--all]      the old function at <address> beside its new partner
                                                           (only what differs and a little around it; --all: all)
  python tools/py/tumap.py callers <map> <address>           who branches to an old address, and where each one's
                                                           partner branches to
  python tools/py/tumap.py off <map> <offset> [<function>]   what became of a structure offset: the matched
                                                           instructions with that displacement, by their new one
                                                           (in the old function at <function> only, when given)

The images are tools/imagedump's (file offset = address - 0x82000000); the map is a pickle beside them
(orig/analysis/tu1_tu3.map). How it works:

Code. The text is cut into blocks: the .pdata functions and what lies between them (leaf functions, cut at their
returns). A block's fingerprint is its instructions without what a relink changes (branch targets, the halves of
addresses, every 16-bit displacement). Blocks with the same fingerprint are paired in order (difflib over the two
lists of fingerprints), the changed ones between them by likeness. Then the callers correct that: wherever two
paired instructions branch, the old target's partner is the new target, and wherever the code forms the address of
a table, the code pointers in it pair up one by one. A block the order paired differently from what its callers say
is paired again (a run of look-alike functions with one added in the middle pairs off by one otherwise); a block the
order left out is paired by its callers. Inside a changed block the instructions are matched with difflib, a call
counted as the same only when it goes to the partner.

Data. Every address the paired code forms (lis + a 16-bit low half) gives an old -> new pair. An address between two
pairs moves as they do when both moved alike; when they did not, the words there decide (the same constants, or
pointers that are each other's partners), and the answer says UNSURE when they cannot.

Structure offsets are not addresses and nothing maps them: `off` and `diff` show how the game's own code changed
(lwz r11,0x34D8(r4) -> 0x34E0), which is how to port an offset the sources use.
"""
import bisect
import difflib
import os
import pickle
import struct
import sys
from collections import Counter, defaultdict

BASE = 0x82000000
DFORM = set([14, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
             55, 58, 62])


def sections(d):
    e = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, e + 6)[0]
    opt = struct.unpack_from('<H', d, e + 20)[0]
    s = e + 24 + opt
    out = {}
    for i in range(n):
        name = d[s + 40 * i:s + 40 * i + 8].rstrip(b'\0').decode()
        vs, va = struct.unpack_from('<II', d, s + 40 * i + 8)
        out[name] = (BASE + va, vs)
    return out


def pdata(d, secs):
    va, vs = secs['.pdata']
    out = []
    for o in range(va - BASE, va - BASE + vs, 8):
        begin, word = struct.unpack_from('>II', d, o)
        if begin == 0:
            break
        out.append((begin, ((word >> 8) & 0x3FFFFF) * 4))
    return out


def sext(v):
    return v - 0x10000 if v & 0x8000 else v


def rel(w):
    d = w & 0x03FFFFFC
    return d - 0x04000000 if d & 0x02000000 else d


class Image:
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        self.secs = sections(self.d)
        lo, size = self.secs['.text']
        self.text = (lo, lo + size)
        n = len(self.d) // 4
        self.words = struct.unpack('>%dI' % n, self.d[:n * 4])
        self.end = BASE + n * 4
        self.blocks = self._blocks()

    def w(self, addr):
        return self.words[(addr - BASE) >> 2]

    def in_text(self, v):
        return self.text[0] <= v < self.text[1]

    def _blocks(self):
        lo, hi = self.text
        out = []
        at = lo
        for begin, length in sorted(pdata(self.d, self.secs)):
            if begin < at or begin >= hi:
                continue
            if begin > at:
                out.extend(self._gap(at, begin))
            out.append((begin, length, True))
            at = begin + length
        if at < hi:
            out.extend(self._gap(at, hi))
        return out

    def _gap(self, lo, hi):
        """What lies between two .pdata functions: leaf functions, cut at each return (blr, bctr, b); the zero
        padding dropped."""
        out = []
        start = None
        a = lo
        while a < hi:
            w = self.w(a)
            if w == 0:
                if start is not None:
                    out.append((start, a - start, False))
                    start = None
            else:
                if start is None:
                    start = a
                if w == 0x4E800020 or w == 0x4E800420 or (w >> 26 == 18 and not w & 1):
                    out.append((start, a + 4 - start, False))
                    start = None
            a += 4
        if start is not None:
            out.append((start, hi - start, False))
        return out

    def fingerprint(self, begin, length):
        out = []
        for a in range(begin, begin + length, 4):
            w = self.w(a)
            op = w >> 26
            if op == 18:
                out.append(w & 0xFC000003)
            elif op == 15 or op in DFORM:
                out.append(w & 0xFFFF0000)
            else:
                out.append(w)
        return out

    def for_matching(self, begin, length, partner):
        """A changed block's instructions for difflib: conditional branches without their distance, a branch out of
        the block by where it goes (`partner` gives what to call that place, the same on both sides)."""
        out = []
        end = begin + length
        for a in range(begin, end, 4):
            w = self.w(a)
            op = w >> 26
            if op == 18:
                if w & 2:
                    out.append(w)
                else:
                    t = a + rel(w)
                    out.append((18, w & 1, 'in' if begin <= t < end else partner(t)))
            elif op == 16:
                out.append(w & 0xFFFF0003)
            elif op == 15 or op in DFORM:
                out.append(w & 0xFFFF0000)
            else:
                out.append(w)
        return out


def pair_changed(ta, tb):
    """Pair the blocks of two runs that are not the same, in order, by how alike they are."""
    n, m = len(ta), len(tb)
    if n == 0 or m == 0:
        return []
    if n * m > 4000:
        pairs = []
        j = 0
        for i in range(n):
            best = None
            for k in range(j, min(m, j + 12)):
                r = difflib.SequenceMatcher(None, ta[i], tb[k], autojunk=False).quick_ratio()
                if r > 0.7 and (best is None or r > best[0]):
                    best = (r, k)
            if best:
                pairs.append((i, best[1]))
                j = best[1] + 1
        return pairs
    score = [[0.0] * m for _ in range(n)]
    for i in range(n):
        for k in range(m):
            la, lb = len(ta[i]), len(tb[k])
            if min(la, lb) * 3 < max(la, lb) and max(la, lb) > 12:
                continue
            sm = difflib.SequenceMatcher(None, ta[i], tb[k], autojunk=False)
            if sm.quick_ratio() < 0.5:
                continue
            score[i][k] = sm.ratio()
    best = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for k in range(m - 1, -1, -1):
            take = score[i][k] + best[i + 1][k + 1] if score[i][k] >= 0.5 else 0.0
            best[i][k] = max(take, best[i + 1][k], best[i][k + 1])
    pairs = []
    i = k = 0
    while i < n and k < m:
        if score[i][k] >= 0.5 and best[i][k] == score[i][k] + best[i + 1][k + 1]:
            pairs.append((i, k))
            i += 1
            k += 1
        elif best[i + 1][k] >= best[i][k + 1]:
            i += 1
        else:
            k += 1
    return pairs


def formed(A, B, paired):
    """The addresses the paired code forms: votes old -> new, and the paired instructions whose 16-bit
    displacement differs without being half of an address (a structure's offset that moved)."""
    votes = defaultdict(Counter)
    moved = []
    big = defaultdict(Counter)      # an offset of more than 16 bits (addis rD,rA,hi + a low half): old -> new
    for ba, bb, same, runs in paired:
        hia = {}
        hib = {}
        ofa = {}
        ofb = {}
        for xa, xb, size in runs:
            if not same:
                hia = {}
                hib = {}
                ofa = {}
                ofb = {}
            for k in range(size):
                aa = ba + 4 * (xa + k)
                ab = bb + 4 * (xb + k)
                wa = A.w(aa)
                op = wa >> 26
                if op == 15:
                    wb = B.w(ab)
                    r = (wa >> 21) & 31
                    if (wa >> 16) & 31 == 0:
                        hia[r] = (wa & 0xFFFF) << 16
                        hib[r] = (wb & 0xFFFF) << 16
                        ofa.pop(r, None)
                        ofb.pop(r, None)
                    else:
                        hia.pop(r, None)
                        hib.pop(r, None)
                        ofa[r] = sext(wa & 0xFFFF) << 16
                        ofb[r] = sext(wb & 0xFFFF) << 16
                elif op in DFORM:
                    wb = B.w(ab)
                    ra = (wa >> 16) & 31
                    rd = (wa >> 21) & 31
                    src = rd if op in (24, 25) else ra
                    if src in hia and src != 0:
                        if op in (24, 25):
                            va = hia[src] | (wa & 0xFFFF)
                            vb = hib[src] | (wb & 0xFFFF)
                        else:
                            va = (hia[src] + sext(wa & 0xFFFF)) & 0xFFFFFFFF
                            vb = (hib[src] + sext(wb & 0xFFFF)) & 0xFFFFFFFF
                        if BASE <= va < A.end and BASE <= vb < B.end:
                            votes[va][vb] += 1
                    elif src in ofa and op not in (24, 25):
                        big[ofa[src] + sext(wa & 0xFFFF)][ofb[src] + sext(wb & 0xFFFF)] += 1
                    elif (wa & 0xFFFF) != (wb & 0xFFFF):
                        moved.append((aa, ab))
                    if op == 14 or (32 <= op <= 35) or (40 <= op <= 43) or op == 58:
                        hia.pop(rd, None)
                        hib.pop(rd, None)
                        ofa.pop(rd, None)
                        ofb.pop(rd, None)
                    elif op in (24, 25):
                        hia.pop(ra, None)
                        hib.pop(ra, None)
                        ofa.pop(ra, None)
                        ofb.pop(ra, None)
                elif op == 31 or op == 7 or op == 8 or op == 12 or op == 13:
                    for r in ((wa >> 21) & 31, (wa >> 16) & 31):
                        hia.pop(r, None)
                        hib.pop(r, None)
                        ofa.pop(r, None)
                        ofb.pop(r, None)
                elif op == 18 or op == 16 or op == 19:
                    if wa & 1 or op == 19:
                        for r in list(hia):
                            if r <= 12:
                                hia.pop(r, None)
                                hib.pop(r, None)
                        for r in list(ofa):
                            if r <= 12:
                                ofa.pop(r, None)
                                ofb.pop(r, None)
    return votes, moved, big


def build(path_a, path_b, out):
    A, B = Image(path_a), Image(path_b)
    fa = [A.fingerprint(b, n) for b, n, _ in A.blocks]
    fb = [B.fingerprint(b, n) for b, n, _ in B.blocks]
    ha = [hash(tuple(t)) for t in fa]
    hb = [hash(tuple(t)) for t in fb]
    pair = {}              # index in A.blocks -> index in B.blocks
    how = {}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ha, hb, autojunk=False).get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                pair[i1 + k] = j1 + k
                how[i1 + k] = 'order'
        else:
            for i, k in pair_changed(fa[i1:i2], fb[j1:j2]):
                pair[i1 + i] = j1 + k
                how[i1 + i] = 'order'
    owner = {ib: ia for ia, ib in pair.items()}
    start_a = {b: i for i, (b, _, _) in enumerate(A.blocks)}
    start_b = {b: i for i, (b, _, _) in enumerate(B.blocks)}
    print('blocks %d and %d, paired by order %d' % (len(A.blocks), len(B.blocks), len(pair)))

    text = {}
    ratio = {}
    paired = []
    data_sections = [s for name, s in A.secs.items() if name in ('.rdata', '.data')]

    def in_data(v):
        return any(lo <= v < lo + size for lo, size in data_sections)

    moves = Counter()
    for rounds in range(1, 40):
        old_text = text
        mapped_b = set(old_text.values())
        text = {}
        paired = []
        ratio = {}
        for ia, ib in pair.items():
            ba, na, _ = A.blocks[ia]
            bb, nb, _ = B.blocks[ib]
            if ha[ia] == hb[ib]:
                runs = [(0, 0, na // 4)]
                same = True
            else:
                ma = A.for_matching(ba, na, old_text.get)
                mb = B.for_matching(bb, nb, lambda t: t if t in mapped_b else None)
                sm = difflib.SequenceMatcher(None, ma, mb, autojunk=False)
                runs = [(x.a, x.b, x.size) for x in sm.get_matching_blocks() if x.size]
                ratio[ia] = sm.ratio()
                same = False
            paired.append((ba, bb, same, runs))
            for xa, xb, size in runs:
                for k in range(size):
                    text[ba + 4 * (xa + k)] = bb + 4 * (xb + k)
        # what the callers say
        votes = defaultdict(Counter)
        sure = set()
        for ba, bb, same, runs in paired:
            if same:
                sure.update(range(ba, ba + 4 * runs[0][2], 4))
        for aa, ab in text.items():
            wa = A.w(aa)
            if wa >> 26 == 18 and not wa & 2:
                # a call in a block that did not change says more than one difflib placed in a changed block
                votes[aa + rel(wa)][ab + rel(B.w(ab))] += 4 if aa in sure else 1
        # and the tables of code pointers whose address the code forms
        data, _, _ = formed(A, B, paired)
        for da, c in data.items():
            db = c.most_common(1)[0][0]
            if da & 3 or db & 3 or not in_data(da):
                continue
            for i in range(2048):
                xa, xb = da + 4 * i, db + 4 * i
                if xa >= A.end or xb >= B.end or (i and xa in data):
                    break
                wa, wb = A.w(xa), B.w(xb)
                ca, cb = A.in_text(wa), B.in_text(wb)
                if ca and cb:
                    votes[wa][wb] += 4
                elif ca != cb:
                    break
                elif wa != wb and not (BASE <= wa < A.end and BASE <= wb < B.end):
                    break
        changes = 0
        for target, c in votes.items():
            ia = start_a.get(target)
            if ia is None:
                continue
            new, n = c.most_common(1)[0]
            ib = start_b.get(new)
            if ib is None:
                continue
            cur = pair.get(ia)
            if cur == ib or moves[ia] >= 3:
                continue
            if cur is not None:
                if n <= c.get(B.blocks[cur][0], 0):
                    continue
                if ha[ia] == hb[cur] and ha[ia] != hb[ib]:
                    continue        # an exact partner is not given up for a changed one
            other = owner.get(ib)
            if other is not None:
                oc = votes.get(A.blocks[other][0])
                if oc is not None and oc.get(new, 0) >= n:
                    continue        # its own callers hold it there
                del pair[other]
                how.pop(other, None)
            if cur is not None:
                del owner[cur]
            pair[ia] = ib
            owner[ib] = ia
            how[ia] = 'callers x%d' % n
            moves[ia] += 1
            changes += 1
        print('round %d: %d blocks paired or paired again by their callers' % (rounds, changes))
        if not changes:
            break

    wrong = []
    for target, c in votes.items():
        new, n = c.most_common(1)[0]
        if target in text and text[target] != new:
            wrong.append((target, text[target], dict(c)))
    data_votes, moved, big = formed(A, B, paired)
    data = {}
    odd = {}
    for va, c in data_votes.items():
        data[va] = c.most_common(1)[0][0]
        if len(c) > 1:
            odd[va] = dict(c)
    fn = {}
    for ia, ib in pair.items():
        ba, na, _ = A.blocks[ia]
        bb, nb, _ = B.blocks[ib]
        fn[ba] = (bb, 'same' if ha[ia] == hb[ib] else 'changed %.2f' % ratio.get(ia, 0), how.get(ia, 'order'), na, nb)
    same_n = sum(1 for p in paired if p[2])
    print('blocks the same %d, changed %d, only in old %d, only in new %d' % (
        same_n, len(paired) - same_n, len(A.blocks) - len(pair), len(B.blocks) - len(pair)))
    print('instructions mapped %d of %d' % (len(text), (A.text[1] - A.text[0]) // 4))
    print('branch targets the map and the callers still disagree on: %d of %d' % (len(wrong), len(votes)))
    print('data pairs %d, with more than one answer %d' % (len(data), len(odd)))
    print('paired instructions whose displacement differs %d' % len(moved))
    m = {
        'text': text, 'fn': fn, 'data': data, 'odd': odd, 'moved': moved, 'wrong': wrong,
        'big': {k: dict(v) for k, v in big.items()},
        'blocks_a': A.blocks, 'blocks_b': B.blocks, 'secs_a': A.secs, 'secs_b': B.secs,
        'path_a': os.path.abspath(path_a), 'path_b': os.path.abspath(path_b),
    }
    with open(out, 'wb') as f:
        pickle.dump(m, f)


class Map:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.m = pickle.load(f)
        self.text = self.m['text']
        self.data = self.m['data']
        self.fn = self.m['fn']
        self.keys = sorted(self.data)
        self.starts = [b for b, _, _ in self.m['blocks_a']]
        lo, size = self.m['secs_a']['.text']
        self.text_range = (lo, lo + size)
        self._a = self._b = None

    @property
    def A(self):
        if self._a is None:
            self._a = Image(self.m['path_a'])
        return self._a

    @property
    def B(self):
        if self._b is None:
            self._b = Image(self.m['path_b'])
        return self._b

    def block(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        b, n, _ = self.m['blocks_a'][i]
        return (b, n) if addr < b + n else None

    def alike(self, old, new, words=6):
        """How many of the words at an old data address and a new one are the same thing."""
        A, B = self.A, self.B
        n = 0
        for i in range(words):
            xa, xb = (old & ~3) + 4 * i, (new & ~3) + 4 * i
            if xa >= A.end or xb >= B.end:
                break
            wa, wb = A.w(xa), B.w(xb)
            if wa == wb:
                n += 1 if wa else 0
            elif self.text.get(wa) == wb or self.data.get(wa) == wb:
                n += 2
            else:
                n -= 2
        return n

    def lookup(self, addr):
        """(new address or None, what is known about it)."""
        if self.text_range[0] <= addr < self.text_range[1]:
            blk = self.block(addr)
            f = self.fn.get(blk[0]) if blk else None
            if addr in self.text:
                return self.text[addr], 'code, block %s, paired by %s' % (f[1], f[2]) if f else 'code'
            if f:
                return None, 'code: the instruction has no partner (block %08X -> %08X, %s)' % (blk[0], f[0], f[1])
            return None, 'code: its block has no partner in the new image'
        if addr in self.data:
            note = 'data, formed by code'
            if addr in self.m['odd']:
                note += ' (answers: %s)' % ', '.join('%08X x%d' % kv for kv in self.m['odd'][addr].items())
            # The code may form a base and add the rest in the instruction that uses it, and split the two
            # differently in the new image: such a pair is not an object's old and new place. It shows as a pair
            # that moved unlike the pairs on both sides of it.
            i = bisect.bisect_left(self.keys, addr)
            d = self.data[addr] - addr
            near = [self.data[k] - k for k in self.keys[max(0, i - 3):i] + self.keys[i + 1:i + 4]]
            if near and d not in near:
                note += ', UNSURE: its neighbours moved %s' % ', '.join('%+X' % x for x in sorted(set(near)))
            return self.data[addr], note
        i = bisect.bisect_right(self.keys, addr)
        below = self.keys[i - 1] if i > 0 else None
        above = self.keys[i] if i < len(self.keys) else None
        db = self.data[below] - below if below is not None else None
        da = self.data[above] - above if above is not None else None
        if db is not None and db == da:
            return addr + db, 'data, between %08X and %08X (both %+X)' % (below, above, db)
        if db is None or da is None:
            return None, 'data: no pair near'
        sb, sa = self.alike(addr, addr + db), self.alike(addr, addr + da)
        text = '%08X below moved %+X (words alike %d), %08X above moved %+X (words alike %d)' % (
            below, db, sb, above, da, sa)
        if sb >= 4 and sb > sa + 2:
            return addr + db, 'data, by its words: ' + text
        if sa >= 4 and sa > sb + 2:
            return addr + da, 'data, by its words: ' + text
        return addr + db, 'data, UNSURE: ' + text


def decoder():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'thirdparty', 'ps3recomp'))
    from ppu_disasm import decode
    return decode


def show_diff(m, addr, everything):
    decode = decoder()
    blk = m.block(addr)
    f = m.fn.get(blk[0]) if blk else None
    if not f:
        print('no partner for the block at %08X' % addr)
        return
    A, B = m.A, m.B
    ba, na = blk
    bb, nb = f[0], f[4]
    print('old %08X+%X -> new %08X+%X, %s, paired by %s' % (ba, na, bb, nb, f[1], f[2]))
    back = {}
    for a in range(ba, ba + na, 4):
        if a in m.text:
            back[m.text[a]] = a
    rows = []          # (old address or None, new address or None, differs)
    b = bb
    for a in range(ba, ba + na, 4):
        t = m.text.get(a)
        if t is None:
            rows.append((a, None, True))
            continue
        while b < t:
            if b not in back:
                rows.append((None, b, True))
            b += 4
        wa, wb = A.w(a), B.w(t)
        differs = False
        op = wa >> 26
        if op in DFORM and (wa & 0xFFFF) != (wb & 0xFFFF):
            differs = (a, t) in m.moved_set
        rows.append((a, t, differs))
        b = t + 4
    while b < bb + nb:
        if b not in back:
            rows.append((None, b, True))
        b += 4
    keep = set()
    for i, r in enumerate(rows):
        if r[2] or everything:
            keep.update(range(max(0, i - 3), min(len(rows), i + 4)))
    last = -1
    for i, (a, b, differs) in enumerate(rows):
        if i not in keep:
            continue
        if i != last + 1:
            print('        ...')
        last = i
        left = right = ''
        if a is not None:
            ins = decode(A.w(a), a)
            left = '%08X  %-8s %s' % (a, ins.mnemonic, ins.operands)
        if b is not None:
            ins = decode(B.w(b), b)
            right = '%08X  %-8s %s' % (b, ins.mnemonic, ins.operands)
        print('%s %-46s | %s' % ('*' if differs else ' ', left, right))


def show_offset(m, offset, fn):
    if not -0x8000 <= offset < 0x8000:
        # more than 16 bits: what the paired code forms with addis + a low half (anywhere; no function filter)
        got = m.m['big'].get(offset)
        if not got:
            print('%X: no paired code forms it (addis + low half)' % offset)
        for new, n in sorted((got or {}).items(), key=lambda kv: -kv[1]):
            print('%6X -> %-8X x%d' % (offset, new, n))
        return
    A, B = m.A, m.B
    blk = m.block(fn) if fn is not None else None
    became = Counter()
    where = defaultdict(list)
    lo, hi = (blk[0], blk[0] + blk[1]) if blk else m.text_range
    for a in range(lo, hi, 4):
        wa = A.w(a)
        if wa >> 26 not in DFORM or sext(wa & 0xFFFF) != offset or (wa >> 16) & 31 in (0, 1, 2, 13):
            continue
        t = m.text.get(a)
        if t is None:
            became[None] += 1
            where[None].append(a)
            continue
        if (a, t) not in m.moved_set and (wa & 0xFFFF) != (B.w(t) & 0xFFFF):
            continue          # half of an address
        new = sext(B.w(t) & 0xFFFF)
        became[new] += 1
        where[new].append(a)
    for new, n in became.most_common():
        print('%6s -> %-8s x%-5d e.g. %s' % ('%X' % offset, 'no partner' if new is None else '%X' % new, n,
                                              ' '.join('%08X' % a for a in where[new][:6])))


def main():
    cmd = sys.argv[1]
    if cmd == 'build':
        build(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    m = Map(sys.argv[2])
    if cmd == 'q':
        for s in sys.argv[3:]:
            a = int(s, 16)
            new, note = m.lookup(a)
            print('%08X -> %s  %s' % (a, '%08X' % new if new is not None else '--------', note))
    elif cmd == 'diff':
        m.moved_set = set(m.m['moved'])
        show_diff(m, int(sys.argv[3], 16), '--all' in sys.argv)
    elif cmd == 'callers':
        target = int(sys.argv[3], 16)
        A, B = m.A, m.B
        for a in range(A.text[0], A.text[1], 4):
            w = A.w(a)
            if w >> 26 == 18 and not w & 2 and a + rel(w) == target:
                t = m.text.get(a)
                blk = m.block(a)
                f = m.fn.get(blk[0]) if blk else None
                print('%08X in block %s (%s) -> %s' % (
                    a, '%08X' % blk[0] if blk else '?', f[1] if f else 'no partner',
                    'no partner' if t is None else '%08X, which branches to %08X' % (t, t + rel(B.w(t)))))
    elif cmd == 'off':
        m.moved_set = set(m.m['moved'])
        show_offset(m, int(sys.argv[3], 16), int(sys.argv[4], 16) if len(sys.argv) > 4 else None)


if __name__ == '__main__':
    main()
