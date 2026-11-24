"""Fight-input scripts that end rounds by ring out (tests of what a ring out does to two games in step): player 1 walks
in and keeps hitting with moves that push (run-in attacks, forward attacks), player 2 walks away to the edge and stands
there. Deterministic for a seed.
    python -I tools/py/ringout.py <1|2> <last frame> [seed 1]"""
import random
import sys

who, end = int(sys.argv[1]), int(sys.argv[2])
rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
X, Y, K = "X", "Y", "B"  # the 360 pad: horizontal, vertical, kick
steps, t = [], 100
if who == 2:
    while t < end:  # away from player 1 (who starts on the left), a breath now and then
        steps.append(f"{t}:RIGHT")
        t += rng.randint(120, 300)
        steps.append(f"{t}:-")
        t += rng.randint(2, 10)
else:
    while t < end:
        r = rng.random()
        if r < 0.45:  # run in (forward twice) with an attack
            steps += [f"{t}:RIGHT", f"{t + 3}:-", f"{t + 5}:RIGHT", f"{t + 9}:RIGHT+{rng.choice([X, Y, K, X + '+' + Y])}"]
            t += 16
        elif r < 0.8:  # a forward attack
            steps.append(f"{t}:RIGHT+{rng.choice([X, Y, K])}")
            t += 6
        else:  # walk in
            steps.append(f"{t}:RIGHT")
            t += rng.randint(15, 45)
        steps.append(f"{t}:RIGHT")
        t += rng.randint(8, 22)
print(",".join(steps))
