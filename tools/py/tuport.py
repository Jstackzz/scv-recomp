"""Move the game addresses in the sources from one build of the game to another, with tumap.py's map.

  python tools/py/tuport.py list <map> [--all] [path ...]    every game address in the sources and how sure its new
                                                           place is (only the unsure ones without --all)
  python tools/py/tuport.py apply <map> [path ...]           rewrite the sure ones in place and list the rest by
                                                           file and line

An address is `sub_8XXXXXXX`, `0x8XXXXXXX` or a bare 8XXXXXXX (the image is 0x82000000 to 0x836B0000). Sure: an
instruction with a partner, an address the game's code forms, a data address between two such that moved alike or
whose own words say where it went. Everything else is left as it is and listed: look at it by hand (tumap.py diff,
off, q). Structure offsets are not addresses: nothing here touches them.
"""
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tumap  # noqa: E402

PAT = re.compile(r'(?<![0-9A-Za-z])(sub_|0x|Sc5Sequence_|)(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
DEFAULT = ['sc5/src', 'sc5/config', 'sc5/tests', 'sc5/player', 'tools', 'launcher/src', 'docs', 'README.md']
EXT = ('.cpp', '.h', '.toml', '.py', '.sh', '.ps1', '.cmd', '.md', '.txt', '.cmake')
SKIP = ('tumap.py', 'tuport.py')


def files(paths):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for root, dirs, names in os.walk(p):
            dirs[:] = [x for x in dirs if x not in ('thirdparty', '__pycache__', 'generated')]
            for n in names:
                if n.endswith(EXT) and n not in SKIP:
                    yield os.path.join(root, n)


def place(m, addr):
    """(new address or None, sure, note)."""
    new, note = m.lookup(addr)
    if new is None and note.startswith('code') and (addr - 4) in m.text:
        blk = m.block(addr)
        prev = m.block(addr - 4)
        if blk is None or (prev and prev != blk):
            return m.text[addr - 4] + 4, True, 'code: the end of the block before it'
    if new is None:
        return None, False, note
    if 'UNSURE' in note:
        return new, False, note
    if note.startswith('code') and 'changed' in note:
        # a changed block: its first instruction is its partner's first; inside it difflib decided
        blk = m.block(addr)
        if blk and blk[0] != addr:
            return new, False, note + ' (inside a changed block: check with diff)'
        ratio = float(note.split('changed ')[1].split(',')[0])
        if ratio < 0.5:
            return new, False, note + ' (little alike: check with diff)'
    return new, True, note


def main():
    cmd, mp = sys.argv[1], sys.argv[2]
    paths = [a for a in sys.argv[3:] if not a.startswith('--')] or DEFAULT
    m = tumap.Map(mp)
    cache = {}
    unsure = defaultdict(list)
    counts = defaultdict(int)
    for f in files(paths):
        with open(f, 'rb') as fh:
            raw = fh.read()
        text = raw.decode('utf-8', errors='surrogateescape')
        changed = False

        def sub(x):
            nonlocal changed
            addr = int(x.group(2), 16)
            if addr not in cache:
                cache[addr] = place(m, addr)
            new, sure, note = cache[addr]
            line = text.count('\n', 0, x.start()) + 1
            if not sure:
                unsure[addr].append('%s:%d' % (f.replace('\\', '/'), line))
                counts['left'] += 1
                return x.group(0)
            counts['moved'] += 1
            if new == addr:
                return x.group(0)
            changed = True
            digits = '%08X' % new
            if x.group(2).islower() and not x.group(2).isdigit():
                digits = digits.lower()
            return x.group(1) + digits

        out = PAT.sub(sub, text)
        if cmd == 'apply' and changed:
            with open(f, 'wb') as fh:
                fh.write(out.encode('utf-8', errors='surrogateescape'))
    print('addresses: %d different, %d uses sure, %d uses left' % (len(cache), counts['moved'], counts['left']))
    if cmd == 'list' and '--all' in sys.argv:
        for addr in sorted(cache):
            new, sure, note = cache[addr]
            if sure:
                print('%08X -> %08X  %s' % (addr, new, note))
    print('== left as they are')
    for addr in sorted(unsure):
        new, sure, note = cache[addr]
        print('%08X -> %s  %s' % (addr, '%08X' % new if new is not None else '--------', note))
        for w in unsure[addr][:6]:
            print('      ' + w)


if __name__ == '__main__':
    main()
