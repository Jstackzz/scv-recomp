"""A fight-input script of varied moves for one player (tests that look for heap pointers in the fight's state): walks
both ways (so throws land whichever side the player is on), attacks, throws (A+G and B+G), shortcuts, quarter circles
and the Critical Edge motion, guard. Deterministic for a seed.
    python -I tools/py/mash.py <seed> <last frame>"""
import random
import sys

rng = random.Random(int(sys.argv[1]))
end = int(sys.argv[2])
X, Y, K, G = "X", "Y", "B", "A"  # the 360 pad: horizontal, vertical, kick, guard
attacks = [X, Y, K, f"{X}+{Y}", f"{Y}+{K}", f"{X}+{K}", f"{X}+{G}", f"{Y}+{G}", f"{X}+{Y}+{K}", "LT", "RT", "LB", "RB"]
dirs = ["", "", "RIGHT", "LEFT", "UP", "DOWN", "DOWN+RIGHT", "DOWN+LEFT", "UP+RIGHT", "UP+LEFT"]
steps, t = [], 100


def add(at, what):
    steps.append(f"{at}:{what or '-'}")


while t < end:
    r = rng.random()
    if r < 0.18:  # walk in (or out) for a while, then a throw
        d = rng.choice(["RIGHT", "LEFT"])
        n = rng.randint(20, 70)
        add(t, d)
        t += n
        add(t, f"{d}+{rng.choice([f'{X}+{G}', f'{Y}+{G}'])}")
        t += 6
    elif r < 0.30:  # a quarter circle and an attack
        side = rng.choice(["RIGHT", "LEFT"])
        for d in ("DOWN", f"DOWN+{side}", side):
            add(t, d)
            t += 3
        add(t, f"{side}+{rng.choice([X, Y, K])}")
        t += 6
    elif r < 0.34:  # the Critical Edge motion (twice down-forward) with A+B+K
        side = rng.choice(["RIGHT", "LEFT"])
        for d in ("DOWN", f"DOWN+{side}", side, "DOWN", f"DOWN+{side}", side):
            add(t, d)
            t += 3
        add(t, f"{side}+{X}+{Y}+{K}")
        t += 8
    elif r < 0.42:  # guard a while
        add(t, G)
        t += rng.randint(10, 40)
    else:  # an attack, maybe with a direction
        d = rng.choice(dirs)
        a = rng.choice(attacks)
        add(t, f"{d}+{a}" if d else a)
        t += rng.randint(4, 8)
    add(t, "-")
    t += rng.randint(6, 18)
print(",".join(steps))
