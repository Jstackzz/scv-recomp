"""Created characters in two linked games as players have them: the same character on both PCs (a copy from the other's
save), downloadable parts on one PC only or on neither, characters made under title update 1, values at and beyond
Creation's limits (docs/GAME.md, "Created characters online"). Each case builds its characters from sc5/tests/data's
records (their checksums made right), puts them in each game's first slots (SC5_TEST_CREATION_SLOT), has each side's
player pick on the game's own select screen (tools/linktest.py's --pick1/--pick2: SC5_TEST_LOCAL_PICK, a second pick
after a comma for when the first is held back) and plays a match of one round with the check on, as in a real match.

    python -I tools/creationmatch.py [case ...] [--runs DIR] [--exe EXE] [--port-base 7601] [--keep-logs]

Per case and game: the match's end (result or where it stopped), how each created character fights (as made, its
plain body) and every pick held back with why, from the game's log; then whether the two games agree. The release
build must be there (sc5\\build.cmd release); two games at a time, about three minutes a case. The DLC is the
worktree's game\\dlc (the owner's packages with their data) or an empty folder (a PC without any).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from creationfuzz import checksum, put32, slot_of  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "sc5", "tests", "data")


def record(name):
    with open(os.path.join(DATA, name + ".rec"), "rb") as f:
        return bytearray(f.read())


def rename(b, s):
    for at in (13245, 13290):
        b[at:at + 45] = s.encode().ljust(45, b"\0")


def part(b, levels, index, model, slot):
    for lv in levels:
        at = 1128 + lv * 288 + index * 8
        put32(b, at, model)
        b[at + 4] = slot


def sealed(b):
    put32(b, 13336, checksum(bytes(b)))
    return bytes(b)


def characters():
    """The characters the cases pick (all original characters, male: original.rec's body)."""
    c = {}
    # a downloadable head piece (DLC06's 7F060800, parts set 00030602) in break levels 0 and 1, the plain body in 2
    # and 3, as title update 3's Creation makes it
    b = record("original")
    rename(b, "Head")
    put32(b, 20 + 8, 0x00030602)
    part(b, (0, 1), 16, 0x7F060800, 29)
    c["head"] = sealed(b)
    # a downloadable hairstyle (DLC09's 7F090600, parts set 00010627) in every break level: no plain body without it
    b = record("original")
    rename(b, "Hair")
    put32(b, 20, 0x00010627)
    part(b, (0, 1, 2, 3), 14, 0x7F090600, 14)
    c["hair"] = sealed(b)
    # made under title update 1: attack rate 150 (title update 3's Creation keeps 80 to 111, title update 1's 200)
    b = record("original")
    rename(b, "Old")
    b[17] = 150
    c["old"] = sealed(b)
    # a pack's part whose data is on the disc (DLC02's 05530000, parts set 00030082), one of its stickers (000C0049)
    # and attack rate 200
    b = record("sticker")
    rename(b, "Disc")
    put32(b, 20 + 8, 0x00030082)
    part(b, (0, 1), 16, 0x05530000, 29)
    at = 5496 + 76 * 92
    put32(b, at, 0x000C0049)
    struct.pack_into(">H", b, at + 14, 0x2B)
    b[17] = 200
    c["disc"] = sealed(b)
    # beyond Creation's limits: voice pitch 60 (the sliders give -50 to 50): held out on its own PC
    b = record("original")
    rename(b, "Pitch")
    b[11] = 60
    c["pitch"] = sealed(b)
    return c


# each case: per game its characters (slots 1, 2 ...), its pick(s) (the Creation slot is 24: its costumes are the
# original characters in slot order, the other PC's after the player's own), its DLC (True: game\dlc, False: none);
# and extra linktest arguments
CASES = {
    # the same character on both PCs (a copy), both with the packages and their data: as made in both
    "copy-dlc-both": ([["head"], ["head"]], ["24.0", "24.0"], [True, True], []),
    # the same, game 2 without any package: its plain body in both games, then VS Battle's own Rematch keeps it
    "copy-dlc-one": ([["head"], ["head"]], ["24.0", "24.0"], [True, False], ["--rematch", "--rematch-args", "stage=keep"]),
    # a hairstyle game 2 lacks even in the plain body: held back in game 1, which then picks its second character
    "held-back": ([["hair", "old"], ["head"]], ["24.0,24.1", "24.0"], [True, False], []),
    # neither PC with packages: a pack's part and sticker whose data is on the disc, attack rates 200 and 150, and a
    # character beyond the limits held out (game 2's slot 1)
    "no-dlc": ([["disc"], ["pitch", "old"]], ["24.0", "24.0"], [False, False], []),
}

LINES = re.compile(r"sc5 (?:autopilot: (?:side \d's created character fights .*|pick refused .*|created character .* "
                   r"against the other PC: .*|player \d's created character .*refused.*|side \d's test pick .*)|"
                   r"creation: (?:player \d's .* from the other PC.*|slot \d+ \(.*\) stays out.*)|dlc: the .*|"
                   r"net: DESYNC.*)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cases", nargs="*")
    ap.add_argument("--runs", default=os.path.join(ROOT, "build", "runs", "creationmatch"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--port-base", type=int, default=7601)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--env1", action="append", default=[], help="NAME=VALUE for game 1 ({i}: the game's number)")
    ap.add_argument("--env2", action="append", default=[], help="NAME=VALUE for game 2")
    a = ap.parse_args()
    names = a.cases or list(CASES)
    os.makedirs(a.runs, exist_ok=True)
    work = tempfile.mkdtemp(prefix="creationmatch-", dir=a.runs)
    nodlc = os.path.join(work, "nodlc")
    os.makedirs(nodlc)
    chars = characters()
    for name in names:
        per_game, picks, dlc, extra = CASES[name]
        cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "linktest.py"), "--name", name, "--p1", "-",
               "--p2", "-", "--rounds", "1", "--seed", "1000", "--runs", a.runs, "--port-base", str(a.port_base),
               "--seconds", "420", "--env1", "SC5_TEST_STAGE=5"]
        for g in (0, 1):
            paths = []
            for k, c in enumerate(per_game[g]):
                p = os.path.join(work, f"{name}-{g + 1}-{k + 1}.slot")
                with open(p, "wb") as f:
                    f.write(slot_of(chars[c]))
                paths.append(p)
            cmd += [f"--env{g + 1}", "SC5_TEST_CREATION_SLOT=" + ",".join(paths), f"--pick{g + 1}", picks[g]]
            if not dlc[g]:
                cmd += [f"--env{g + 1}", f"SC5_DLC_DIR={nodlc}"]
        for g, kvs in ((1, a.env1), (2, a.env2)):
            for kv in kvs:
                cmd += [f"--env{g}", kv]
        if a.exe:
            cmd += ["--exe", a.exe]
        out = subprocess.run(cmd + extra, capture_output=True, text=True).stdout
        results = [l.split(": ", 1)[-1] for l in out.splitlines() if " result winner" in l]
        stops = [l.split(": ", 1)[-1] for l in out.splitlines() if re.search(r": (error|desync|disconnected)", l)]
        print(f"== {name}: " + ("; ".join(results) if results else "no result: " + "; ".join(stops or out.splitlines()[-1:])))
        said = []
        for g in (1, 2):
            log = os.path.join(a.runs, f"{name}_{g}.log")
            lines = []
            if os.path.exists(log):
                with open(log, encoding="utf-8", errors="replace") as f:
                    lines = [m.group(0) for m in (LINES.search(l) for l in f) if m]
            said.append(lines)
            for l in lines:
                print(f"  game {g}: {l}")
        both = len(results) >= 2 and len(set(results)) * 2 == len(results) if results else False
        print(f"  the two games' results {'agree' if both else 'DIFFER or are missing'}")
        if not a.keep_logs:
            for f in os.listdir(a.runs):
                if f.startswith(name + "_"):
                    os.remove(os.path.join(a.runs, f))
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
