"""A netplay log (frame P1 P2, each buttons:LTRT:LXLYRXRY) as the two fight-input scripts (SC5_FIGHT_INPUT_P1/P2),
exactly: each entry `delay` frames before the frame it played on (the script is read when the input is sent), the
analog values as testinput's LX= LY= RX= RY= LT= RT= tokens.
    python -I tools/py/netlog2script.py <net log> <delay> <last frame>   (two lines: player 1's, player 2's)"""
import sys
NAMES = [(0x0001, "UP"), (0x0002, "DOWN"), (0x0004, "LEFT"), (0x0008, "RIGHT"), (0x0100, "LB"), (0x0200, "RB"),
         (0x0040, "LS"), (0x0080, "RS"), (0x1000, "A"), (0x2000, "B"), (0x4000, "X"), (0x8000, "Y")]
path, delay, upto = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])


def signed(h):
    v = int(h, 16)
    return v - 256 if v > 127 else v


last = [None, None]
out = [[], []]
for line in open(path):
    f = line.split()
    if len(f) < 3 or not f[0].isdigit():
        continue
    frame = int(f[0])
    if frame > upto:
        break
    for p in (0, 1):
        b, t, s = f[1 + p].split(":")
        b = int(b, 16)
        tokens = [n for bit, n in NAMES if b & bit]
        for name, val in (("LT", int(t[:2], 16)), ("RT", int(t[2:4], 16))):
            if val:
                tokens.append(f"{name}={val}")
        for name, val in zip(("LX", "LY", "RX", "RY"), (signed(s[i:i + 2]) for i in (0, 2, 4, 6))):
            if val:
                tokens.append(f"{name}={val}")
        key = "+".join(tokens) or "-"
        if key != last[p]:
            out[p].append(f"{max(0, frame - delay)}:{key}")
            last[p] = key
print(",".join(out[0]))
print(",".join(out[1]))
