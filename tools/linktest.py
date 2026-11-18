"""Two games on this PC driven through the launcher's files (docs/PROTOCOL.md 3.5, the local test): each gets its own
session folder (cmd.txt, status.txt, game.log, an empty save), both boot to their hold point, get the same start line
(sides 1 and 2 on ports 7001/7002), play the match, and are parked (or rematched first) and quit. Prints every status
line both games write, with the time.

    python -I tools/linktest.py [--name linktest] [--mode versus|sparring] [--p1 Siegfried] [--p2 Mitsurugi]
                                [--rounds 1] [--lag 0] [--pick1 SLOT[.COSTUME]] [--pick2 ...] [--rematch] [--again]
                                [--park-after S] [--back-at FRAME] [--watch] [--dlc2 DIR] [--env NAME=VALUE]
                                [--seconds 600] [--keep] [--heap-shift2 MB] [--fight1 SCRIPT] [--fight2 SCRIPT]
                                [--seed N] [--port-base 7001] [--exe SC5.EXE] [--args1 ".."] [--args2 ".."] [--runs DIR]
                                [--env1 NAME=VALUE] [--env2 NAME=VALUE]

--fight1/--fight2: other fight scripts (tools/py/netlog2script.py makes them from a match's net log, so a desync between
two PCs plays again here; tools/py/mash.py makes varied ones); --heap-shift2 moves game 2's heap, as a second PC's lies
elsewhere; --exe a copy of the game, so a build can go on meanwhile; --args1/--args2 each game's own command line and
--env1/--env2 its own variables (two PCs' display settings side by side: --env2 SC5_RENDER_SCALE=2, as the launcher
passes them, docs/PROTOCOL.md 3.1); --port-base and --runs let two tests run at once (tools/heapsweep.sh).

--p1/--p2 "-": that side picks on the game's own select screen; --pick1/--pick2 then stand in for each PC's player
there (SC5_TEST_LOCAL_PICK). The fight's inputs are tools/synctest.sh's scripted ones (player 1's in game 1, player
2's in game 2); --back-at adds a BACK to player 1's at that fight frame (Online Practice: the match ends). --watch: a
third game watches the match as a spectator, through a relay that stands in for the launchers' spectator feed (the
players' games write to one port, the spectator's reads everything so far, then the rest, from another). --dlc2: game
2's own DLC folder (an empty one: a player without DLC). The release build must be there (sc5\\build.cmd release).
Session folders go in build/runs/<name>-1, -2, -3 (deleted afterwards unless --keep; the logs are copied to
build/runs/<name>_1.log ... first).
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "sc5", "out", "build", "win-amd64-release", "sc5.exe")
FIGHT_P1 = ("130:RIGHT,170:-,180:A,186:-,210:B,216:-,240:X,246:-,270:A+B,276:-,300:DOWN+A,306:-,340:Y,346:-,"
            "370:RIGHT,400:-,410:A,416:-,450:X+Y,456:-,500:B,506:-,530:A,536:-,600:RIGHT,700:-,710:A,716:-,740:B,"
            "746:-,780:A+B,786:-,820:X,826:-,860:A,866:-,900:B,906:-,940:A,946:-,980:Y,986:-")
FIGHT_P2 = ("140:LEFT,180:-,190:B,196:-,220:A,226:-,250:Y,256:-,290:B+X,296:-,320:UP,330:-,360:A,366:-,390:LEFT,"
            "420:-,440:B,446:-,480:X,486:-,520:A+B,526:-")
DONE = ("result", "desync", "disconnected", "error", "sparring_over")


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(text + "\n")
    for _ in range(200):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.005)


def read_status(path):
    try:
        with open(path) as f:
            return f.readline().strip()
    except OSError:
        return ""


class FeedRelay:
    """The launchers' spectator feed on one PC: the players' games connect to `players` and write packets (u16 length
    first); the spectator's game connects to `watcher` and gets every packet so far, then the rest."""

    def __init__(self):
        self.packets, self.lock, self.watchers = [], threading.Lock(), []
        self.players = self._listen(self._player)
        self.watcher = self._listen(self._watcher)

    def _listen(self, handler):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(("127.0.0.1", 0))
        s.listen(4)
        threading.Thread(target=self._accept, args=(s, handler), daemon=True).start()
        return s.getsockname()[1]

    def _accept(self, s, handler):
        while True:
            c, _ = s.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()

    def _player(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(65536)
            except OSError:
                return
            if not d:
                return
            buf += d
            while len(buf) >= 2:
                n = struct.unpack("<H", buf[:2])[0]
                if len(buf) < 2 + n:
                    break
                pkt = buf[:2 + n]
                buf = buf[2 + n:]
                with self.lock:
                    self.packets.append(pkt)
                    for w in list(self.watchers):
                        try:
                            w.sendall(pkt)
                        except OSError:
                            self.watchers.remove(w)

    def _watcher(self, c):
        with self.lock:
            try:
                c.sendall(b"".join(self.packets))
                self.watchers.append(c)
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="linktest")
    ap.add_argument("--mode", default="versus")
    ap.add_argument("--p1", default="Siegfried")
    ap.add_argument("--p2", default="Mitsurugi")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--lag", type=int, default=0)
    ap.add_argument("--delay", type=int, default=2, help="the start line's input delay")
    ap.add_argument("--pick1", default="")
    ap.add_argument("--pick2", default="")
    ap.add_argument("--rematch", action="store_true")
    ap.add_argument("--again", action="store_true", help="a second match after the park")
    ap.add_argument("--park-after", type=float, default=0, help="park this many seconds into the match")
    ap.add_argument("--back-at", type=int, default=0, help="player 1 presses BACK at this fight frame")
    ap.add_argument("--watch", action="store_true", help="a third game watches as a spectator")
    ap.add_argument("--dlc2", default="", help="game 2's DLC folder")
    ap.add_argument("--heap-shift2", type=int, default=0, help="MB of game 2's heap taken first (raw pointers)")
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for both games (repeatable)")
    ap.add_argument("--env1", action="append", default=[], help="NAME=VALUE for game 1 only (repeatable)")
    ap.add_argument("--env2", action="append", default=[], help="NAME=VALUE for game 2 only (repeatable)")
    # tests of two PCs' determinism (docs/GAME.md, "Two PCs"): other fights, seeds, ports and builds
    ap.add_argument("--fight1", default="", help="player 1's fight script (default: tools/synctest.sh's)")
    ap.add_argument("--fight2", default="", help="player 2's fight script")
    ap.add_argument("--seed", type=int, default=12345, help="the start line's seed")
    ap.add_argument("--port-base", type=int, default=7001, help="game 1's UDP port; game 2's the next")
    ap.add_argument("--exe", default="", help="another sc5.exe (a copy, so a build can go on meanwhile)")
    ap.add_argument("--args1", default="", help="game 1's extra command line (graphics settings, say)")
    ap.add_argument("--args2", default="", help="game 2's extra command line")
    ap.add_argument("--runs", default="", help="where the session folders and logs go (default build/runs)")
    a = ap.parse_args()

    relay = FeedRelay() if a.watch else None
    runs = a.runs or os.path.join(ROOT, "build", "runs")
    os.makedirs(runs, exist_ok=True)
    exe = a.exe or EXE
    games, sessions = [], []
    count = 3 if a.watch else 2
    t0 = time.time()
    for i in range(1, count + 1):
        s = os.path.join(runs, f"{a.name}-{i}")
        shutil.rmtree(s, ignore_errors=True)
        os.makedirs(os.path.join(s, "user"))
        # the shader cache: a copy of the test saves' (an empty one makes the first fights stutter)
        cache = os.path.join(ROOT, "game", "user", "cache")
        if os.path.isdir(cache):
            shutil.copytree(cache, os.path.join(s, "cache"))
        env = dict(os.environ)
        env.update({
            "SC5_NET": "1", "SC5_GAME": "sc5", "SC5_SESSION_DIR": s, "SC5_INSTANCE": str(i),
            "SC5_CMD": os.path.join(s, "cmd.txt"), "SC5_STATUS": os.path.join(s, "status.txt"),
            "SC5_LOG": os.path.join(s, "game.log"),
            "SC5_GAME_DIR": os.path.join(ROOT, "game", "disc"), "SC5_UPDATE_DIR": os.path.join(ROOT, "game", "update"),
            "SC5_DLC_DIR": a.dlc2 if i == 2 and a.dlc2 else os.path.join(ROOT, "game", "dlc"),
            "SC5_USER_DIR": os.path.join(s, "user"),
            "SC5_CACHE_DIR": os.path.join(s, "cache"), "SC5_NET_LOG": os.path.join(s, "net-{match}.log"),
        })
        if i <= 2:
            script = (a.fight1 or FIGHT_P1) if i == 1 else (a.fight2 or FIGHT_P2)
            if i == 1 and a.back_at:
                script += f",{a.back_at}:BACK,{a.back_at + 6}:-"
            env[f"SC5_FIGHT_INPUT_P{i}"] = script
        for kv in a.env + (a.env1 if i == 1 else a.env2 if i == 2 else []):
            k, _, v = kv.partition("=")
            env[k] = v.replace("{i}", str(i))
        if a.lag and i <= 2:
            env["SC5_NET_LAG"] = str(a.lag)
        if i == 2 and a.heap_shift2:
            env["SC5_TEST_HEAP_SHIFT"] = str(a.heap_shift2)
        pick = a.pick1 if i == 1 else a.pick2 if i == 2 else ""
        if pick:
            env["SC5_TEST_LOCAL_PICK"] = pick
        extra = (a.args1 if i == 1 else a.args2 if i == 2 else "").split()
        p = subprocess.Popen([exe, "--fullscreen=false", "--window_width=960", "--window_height=540"] + extra,
                             cwd=os.path.dirname(exe), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        games.append(p)
        sessions.append(s)
        time.sleep(2)

    last = [""] * count
    stage = "boot"
    rematched = False
    again_done = False
    in_match_at = 0
    match = f"local-{int(t0)}"
    try:
        while time.time() - t0 < a.seconds:
            st = [read_status(os.path.join(s, "status.txt")) for s in sessions]
            for i in range(count):
                if st[i] != last[i]:
                    print(f"{time.time() - t0:7.1f}s game {i + 1}: {st[i]}", flush=True)
                    last[i] = st[i]
            if stage != "quit" and any(p.poll() is not None for p in games):
                print("a game exited", [p.poll() for p in games])
                break
            if stage == "boot" and all(x == "ready" for x in st):
                for i in (0, 1):
                    line = (f"start game=sc5 mode={a.mode} side={i + 1} local_port={a.port_base + i} "
                            f"remote=127.0.0.1:{a.port_base + 1 - i} delay={a.delay} seed={a.seed} p1={a.p1} p2={a.p2} "
                            f"rounds={a.rounds} ranked=0 match={match}")
                    if relay:
                        line += f" feed=127.0.0.1:{relay.players}"
                    write_atomic(os.path.join(sessions[i], "cmd.txt"), line)
                if relay:
                    write_atomic(os.path.join(sessions[2], "cmd.txt"),
                                 f"start game=sc5 mode={a.mode} watch=1 feed=127.0.0.1:{relay.watcher} seed={a.seed} "
                                 f"p1={a.p1} p2={a.p2} rounds={a.rounds} ranked=0 match={match}")
                print(f"{time.time() - t0:7.1f}s start sent", flush=True)
                stage = "match"
            elif stage == "match" and a.park_after and all(x.startswith("in_match") for x in st[:2]):
                if not in_match_at:
                    in_match_at = time.time()
                elif time.time() - in_match_at > a.park_after:
                    for s in sessions:
                        write_atomic(os.path.join(s, "cmd.txt"), "park")
                    print(f"{time.time() - t0:7.1f}s park sent in the match", flush=True)
                    stage = "park"
            elif stage == "match" and all(x.startswith(DONE) for x in st):
                if a.rematch and not rematched and all(x.startswith("result") for x in st[:2]):
                    rematched = True
                    for s in sessions[:2]:
                        write_atomic(os.path.join(s, "cmd.txt"), f"rematch seed=777 match={match}-2")
                    print(f"{time.time() - t0:7.1f}s rematch sent", flush=True)
                    time.sleep(1)
                    continue
                for s in sessions:
                    write_atomic(os.path.join(s, "cmd.txt"), "park")
                print(f"{time.time() - t0:7.1f}s park sent", flush=True)
                stage = "park"
            elif stage == "park" and all(x == "ready" for x in st) and a.again and not again_done:
                again_done = True
                match = match + "-again"
                stage = "boot"
                print(f"{time.time() - t0:7.1f}s again", flush=True)
                continue
            elif stage == "park" and all(x == "ready" for x in st):
                for s in sessions:
                    write_atomic(os.path.join(s, "cmd.txt"), "quit")
                print(f"{time.time() - t0:7.1f}s quit sent", flush=True)
                stage = "quit"
            elif stage == "quit" and all(p.poll() is not None for p in games):
                break
            time.sleep(0.1)
    finally:
        for p in games:
            if p.poll() is None:
                p.kill()
        time.sleep(1)
        for i, s in enumerate(sessions):
            src = os.path.join(s, "game.log")
            if os.path.exists(src):
                shutil.copy(src, os.path.join(runs, f"{a.name}_{i + 1}.log"))
            for f in os.listdir(s):
                if (f.startswith("net-") and f.endswith(".log")) or f.startswith("stats-"):
                    shutil.copy(os.path.join(s, f), os.path.join(runs, f"{a.name}_{i + 1}_{f}"))
            if not a.keep:
                shutil.rmtree(s, ignore_errors=True)
    if relay:
        print(f"the feed passed {len(relay.packets)} packets, {sum(len(p) for p in relay.packets) // 1024} KB")
    print(f"done after {time.time() - t0:.0f}s at stage {stage}")


if __name__ == "__main__":
    main()
