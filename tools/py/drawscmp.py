"""Two games' fight draws side by side (SC5_DBG_DRAWS files, sc5/rng.h): the first match frames whose Park-Miller draws
differ, with how many each game drew and the draws around the first one apart (caller:state before).
    python -I tools/py/drawscmp.py <game 1's file> <game 2's file> [frames shown, default 8]"""
import sys


def load(path):
    out = {}
    for line in open(path):
        f, _, rest = line.rstrip("\n").partition(":")
        if f.isdigit():
            out[int(f)] = rest.split()
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
shown = int(sys.argv[3]) if len(sys.argv) > 3 else 8
both = sorted(set(a) & set(b))
n = 0
for f in both:
    x, y = a[f], b[f]
    if x == y:
        continue
    k = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
    callers = lambda d: " ".join(v.split(":")[0] for v in d)
    print(f"frame {f}: {len(x)} draws / {len(y)}; first apart at draw {k}:")
    print(f"   game 1: {' '.join(x[max(0, k - 2):k + 4])}")
    print(f"   game 2: {' '.join(y[max(0, k - 2):k + 4])}")
    if callers(x) != callers(y):
        print(f"   callers 1: {callers(x[max(0, k - 2):k + 8])}")
        print(f"   callers 2: {callers(y[max(0, k - 2):k + 8])}")
    n += 1
    if n >= shown:
        break
print(f"{len(both)} frames in both files ({both[0] if both else '-'} to {both[-1] if both else '-'}), {'none' if not n else 'the first ' + str(n)} apart shown")
