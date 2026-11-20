"""Created characters with one area garbled, loaded by two games with the check off: what the game itself does with
them (docs/GAME.md, "Created characters online"). The check (sc5/src/sc5/creation_check.cpp) must refuse whatever
hangs, stalls or crashes a game here; its own tests are sc5/tests/creation_check_test.cpp.

    python -I tools/creationfuzz.py [--runs DIR] [--exe EXE] [--record original|patroklos] [--checked] [case ...]

Each case starts from a record in sc5/tests/data (original.rec, sticker.rec for the sticker cases; with --record
patroklos the customized Patroklos, patroklos.rec, for every case), garbles one area, makes its checksum right again
and puts it in slot 1 of game 1 (SC5_TEST_CREATION_SLOT); game 1's player picks it (the Creation slot's first entry, or
Patroklos's third costume) and game 2's autopilot follows, taking the record over the link. The fighter cases (the
default with --record patroklos, after the others) change the customized Patroklos as the check's own test does
(sc5/tests/creation_check_test.cpp): his other costumes, another fighter's hair, another fighter's costume, a creation
body's base type, no parts at all. Both games run with
SC5_TEST_CREATION_UNCHECKED=1 (any record of the right size is taken, never in a real match). Per case: whether both
games reached the fight in sync and the result, or where they stopped (--checked: both games check the record as in
a real match, so a refused one ends the match with game 2's error line or is never offered). The release build must be
there (sc5\\build.cmd
release). Two games at a time, a few minutes a case.
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "sc5", "tests", "data")
SIZE, HEADER, PICTURE = 13340, 132, 16360


def rotl(w, r):
    return ((w << r) | (w >> (32 - r))) & 0xFFFFFFFF if r else w


def checksum(rec):
    """The game's (sub_82245F98): each word turned left by an amount the word before sets, XORed together."""
    acc, rot = 0, 7
    for j in range(3334):
        w = rotl(struct.unpack_from(">I", rec, 4 * j)[0], rot)
        acc ^= w
        s = (w >> 24) + (w >> 16 & 0xFF) + (w >> 8 & 0xFF) + (w & 0xFF)
        rot = s % 29 - j % 7 + 7
        rot = rot + 32 if rot < 0 else rot - 32 if rot > 31 else rot
    return acc


def put32(b, at, v):
    struct.pack_into(">I", b, at, v & 0xFFFFFFFF)


def putf(b, at, v):
    struct.pack_into(">f", b, at, v)


def garble(b, at, n, rnd):
    for i in range(n):
        b[at + i] = rnd.randrange(256)


# each case: (the record it starts from, what it garbles)
def spoil(name, b, rnd):
    if name == "partsset":  # enCreationPartsSetID[20]
        for i in range(20):
            put32(b, 20 + 4 * i, rnd.choice([0x00FF1234, 0x7FFF7FFF, 0x0001FFFF, 0x00630005, rnd.getrandbits(32)]))
    elif name == "models":  # every break level's parts: ModelData and slot
        for lv in range(4):
            for k in range(34):
                at = 1128 + lv * 288 + k * 8
                put32(b, at, rnd.choice([0x02FFFFFF, 0x05000000, 0x7F123456, 0x02000063, rnd.getrandbits(31)]))
                b[at + 4] = rnd.randrange(256)
    elif name == "breaklevel":  # each level's tail: oppai swing, foot sound, hit material, weight
        for lv in range(4):
            garble(b, 1128 + lv * 288 + 272, 16, rnd)
    elif name == "stickers":  # every sticker slot's first 20 bytes (ids, textures) and placement
        for k in range(84):
            at = 5496 + k * 92
            garble(b, at, 20, rnd)
            for f in range(10):
                putf(b, at + 20 + 4 * f, rnd.choice([1e30, -1e30, float("nan"), float("inf"), 12345.0]))
    elif name == "stickerfields":  # the sticker's bytes but its id and texture
        at = 5496 + 76 * 92
        for o in list(range(4, 14)) + [18, 19]:
            b[at + o] = rnd.choice([0xFF, 0x7F, 0x20, 0x09, rnd.randrange(256)])
    elif name == "stickerfloats":  # the sticker's placement
        at = 5496 + 76 * 92
        for f in range(10):
            putf(b, at + 20 + 4 * f, rnd.choice([1e30, -1e30, float("nan"), float("inf"), 9999.0]))
    elif name == "colours":
        garble(b, 168, 960, rnd)
    elif name == "weapon":
        garble(b, 4192, 700, rnd)
    elif name == "weaponvariation":  # a variation its style does not have
        struct.pack_into(">H", b, 4192 + 2, 99)
    elif name == "weaponrest":  # the weapon but its style, variation, colours and stickers: effect, trail, effect info
        garble(b, 4192 + 4, 4, rnd)
        garble(b, 4192 + 408, 292, rnd)
    elif name == "bodybytes":  # the body's muscle type and level, matrix scale, figure mode only
        for o in range(4):
            b[164 + o] = rnd.choice([0x7F, 0xFE, 0xC8, 0x63, 0x10])
    elif name == "header":  # the small ids
        for at in (4, 5, 6, 8, 10, 11, 12, 13, 14, 15, 13224):
            b[at] = rnd.choice([0x7F, 0xFE, 0xC8, 0x63])
    elif name == "coords":  # parts coordinates: 19 x 96
        for k in range(19):
            at = 2368 + k * 96
            put32(b, at, 1)
            put32(b, at + 4, rnd.getrandbits(32))
            struct.pack_into(">H", b, at + 8, rnd.randrange(65536))
            b[at + 10] = rnd.randrange(256)
            for f in range(21):
                putf(b, at + 12 + 4 * f, rnd.choice([1e30, float("nan"), -7.5e20, 3.0]))
    elif name == "effects":  # break effects, parts sounds, equipped category diff
        garble(b, 4920, 576, rnd)
        garble(b, 2288, 80, rnd)
        garble(b, 2280, 8, rnd)
    elif name == "body":
        for f in range(16):
            putf(b, 100 + 4 * f, rnd.choice([1e30, float("nan"), -1e30, 50.0]))
        garble(b, 164, 4, rnd)
    elif name == "type":  # character type, style, base type
        b[9] = b[16] = b[7] = 0x7E
    elif name == "names":  # names without their end, control characters
        for at, n in ((13225, 5), (13230, 15), (13245, 45), (13290, 45)):
            for i in range(n):
                b[at + i] = rnd.choice([0x01, 0x41, 0xFF, 0x0A, 0x25])
    # the customized Patroklos (patroklos.rec: parts set 00130035 last, model 00350000 in part 29 of each break level)
    elif name == "fighter":  # as Creation made him
        pass
    elif name == "fighter2p":  # his second costume
        put32(b, 20 + 4 * 19, 0x00130135)
        wear(b, 29, 0x00350100, 0x1F)
    elif name == "fighter3p":  # his third
        put32(b, 20 + 4 * 19, 0x00130435)
        wear(b, 29, 0x00350300, 0x1F)
    elif name == "fighterhair":  # Mitsurugi's hair too, which any fighter's table has
        put32(b, 20 + 4 * 1, 0x02020001)
        wear(b, 33, 0x01000000, 0x0F)
    elif name == "fightercostume":  # Mitsurugi's costume on him (the check refuses it)
        put32(b, 20 + 4 * 19, 0x00130001)
        wear(b, 29, 0x00010000, 0x1F)
    elif name == "fighterbase":  # his costume on a creation body, base type 0 (the check refuses it)
        b[7] = 0
    # the header's small ids one at a time (the header case's values), which a customized fighter may read otherwise
    elif name == "fighterselect":  # select number, variation
        b[4] = b[5] = 200
    elif name == "fightermodel":  # model
        b[6] = 254
    elif name == "fighterface":  # facial motion, eyebrow
        b[8] = 200
        b[14] = 127
    elif name == "fightervoice":  # voice
        b[10] = 254
    elif name.startswith("fightervoice") and name[12:].isdigit():  # a voice the game knows (sub_82351040)
        b[10] = int(name[12:])
    elif name == "fighterheight":  # height
        b[13224] = 127
    elif name == "fighternone":  # no parts at all
        for lv in range(4):
            for k in range(34):
                put32(b, 1128 + lv * 288 + k * 8, 0xFFFFFFFF)
                b[1128 + lv * 288 + k * 8 + 4] = 0xFF
    else:
        raise SystemExit(f"no case {name}")


CASES = ["partsset", "models", "breaklevel", "stickers", "stickerfields", "stickerfloats", "colours", "weapon",
         "weaponvariation", "weaponrest", "header", "coords", "effects", "body", "bodybytes", "type", "names"]
FIGHTER_CASES = ["fighter", "fighter2p", "fighter3p", "fighterhair", "fightercostume", "fighterbase", "fighternone"]


def wear(b, part, model, slot):
    """the same part in every break level"""
    for lv in range(4):
        at = 1128 + lv * 288 + part * 8
        put32(b, at, model)
        b[at + 4] = slot


def slot_of(record):
    """A whole creation slot (29,832 bytes) around a record: the header a created character has, no picture."""
    s = bytearray(HEADER + SIZE + PICTURE)
    s[HEADER:HEADER + SIZE] = record
    put32(s, 112, 1)
    put32(s, 120 if record[9] == 32 else 116, 1)
    put32(s, 124, 0x01000000)
    put32(s, 128, 0x01000000)
    return bytes(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cases", nargs="*", default=None)
    ap.add_argument("--record", choices=["original", "patroklos"], default="original")
    ap.add_argument("--checked", action="store_true", help="both games check the record as in a real match")
    ap.add_argument("--runs", default=os.path.join(ROOT, "build", "runs", "creationfuzz"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--port-base", type=int, default=7801)
    a = ap.parse_args()
    if not a.cases:
        a.cases = CASES + (FIGHTER_CASES if a.record == "patroklos" else [])
    os.makedirs(a.runs, exist_ok=True)
    work = tempfile.mkdtemp(prefix="creationfuzz-")
    for name in a.cases:
        if a.record == "patroklos" or name.startswith("fighter"):
            src = "patroklos.rec"
        else:
            src = "sticker.rec" if name.startswith("sticker") else "original.rec"
        with open(os.path.join(DATA, src), "rb") as f:
            b = bytearray(f.read())
        spoil(name, b, random.Random(name))
        put32(b, 13336, checksum(bytes(b)))
        path = os.path.join(work, f"{name}.slot")
        with open(path, "wb") as f:
            f.write(slot_of(bytes(b)))
        # the pick: the Creation slot's first entry, or Patroklos's third costume (his own two, then the created one)
        pick = "0.2" if src == "patroklos.rec" else "24.0"
        cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "linktest.py"), "--name", name, "--p1", "-",
               "--p2", "Viola", "--pick1", pick, "--rounds", "1", "--lag", "0", "--delay", "1", "--seed", "1000",
               "--env1", f"SC5_TEST_CREATION_SLOT={path}", "--env1", "SC5_TEST_STAGE=5",
               "--runs", a.runs, "--port-base", str(a.port_base), "--seconds", "280"]
        if not a.checked:
            cmd += ["--env", "SC5_TEST_CREATION_UNCHECKED=1"]
        if a.exe:
            cmd += ["--exe", a.exe]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=330).stdout
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            out += "\n(timed out: a game hung)"
        results = [l.strip() for l in out.splitlines() if "result winner" in l]
        errors = [l.strip() for l in out.splitlines() if ": error " in l]
        last = out.strip().splitlines()[-1] if out.strip() else "nothing"
        if results:
            verdict = "fought in sync, " + results[0].split(": ", 1)[-1]
        elif errors:  # the check's refusal (--checked), or where the game went wrong
            error = errors[0].split(": ", 1)[-1]
            verdict = ("refused: " if "was refused" in error else "stopped: ") + error
        else:
            verdict = "no result: " + last
        print(f"{name}: {verdict}", flush=True)


if __name__ == "__main__":
    main()
