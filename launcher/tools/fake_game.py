#!/usr/bin/env python3
"""Development stand-in for the game executable (sc5.exe): speaks the launcher <-> game file protocol
(docs/PROTOCOL.md section 3) without any game, so the launcher can be tested end to end.

Point the game's "Executable" in the launcher's Settings at this file (the launcher runs .py files with Python).
It reads SC5_STATUS and SC5_CMD from the environment, like the real game.

    booting -> ready
    start (online)        starting, binds local_port, exchanges heartbeats with remote, writes in_match; after
                          SC5_FAKE_SECONDS (default 6) writes a result derived from the seed, so both sides of a
                          match always agree (Online Practice, mode=sparring: sparring_over); disconnected if the
                          peer goes quiet, "error no connection" if it never answers. With feed=127.0.0.1:<port> (a
                          lobby match) it writes a spectator feed there as a real game would: its READY (type 6),
                          its inputs (7) every 0.1 s, its checks (8) every second and the frame it left at (9).
    start watch=1         a spectator: reads both players' feed from feed=, in_match once both READYs came, the
                          result (from the seed) once both players' type 9 came
    start mode=practice   in_match match=practice until park
    park                  back to ready
    rematch [seed=N]      another match with the same peer
    choose=1 (start or rematch line): after its result the game writes `chose choice=rematch match=<id>` (the loser's
                          choice on the result screen, SC5_FAKE_CHOICE=select for the other) after
                          SC5_FAKE_CHOOSE_SECONDS (default 2), as both of a lobby match's games do
    quit                  exit

A result names the fighters as the real game does, by the game's own character ids with the costume after a dot
(p1_char=3.0: launcher/data/sc5.json game_id): the start line's, or for a side picked on the select screen (-) one from
the seed, alike in both games.

Other knobs: SC5_FAKE_BOOT (seconds before "ready", default 1.5), SC5_FAKE_RESULT=desync to report
"desync frame=600" instead of a result (its game.log, beside status.txt, then has a match's lines as SCV's game
writes them: the start, the picks, the stage, the meeting, the agreed words, the per-10-s statistics, round ends, the
DESYNC line and SC5_FAKE_DETAIL (default 60) `sc5 desync:` detail lines after it, for the launcher's desync report),
SC5_FAKE_RESULT=draw for a drawn match (winner=0, a double KO in the last round), SC5_FAKE_STUCK=1 for a game that
never takes `park` once a match is over (it stays at that match's last status; `quit` still ends it),
SC5_FAKE_REFUSED=<name>: <why> for a game whose player's created character was held back at the select screen
(`starting ... refused=1 refused_why=...`, which the launcher reports to the server). The game's log
starts with every SC5_ variable it was given (the launcher's side of docs/PROTOCOL.md 3.1, for tests to check).
"""

from __future__ import annotations

import json
import os
import socket
import struct
import sys
import time

FEED_MAGIC = b"SC5R"  # the spectator feed's packets (server/app.py FEED_MAGIC): magic, type, side (0 P1, 1 P2), u16 barrier


def simulated_result(seed: int, rounds: int = 3) -> tuple[int, int, int]:
    """(winner, p1_rounds, p2_rounds) from the seed, the same rule as server/client.py."""
    need = rounds // 2 + 1
    winner = 1 if seed % 2 == 0 else 2
    loser_rounds = (seed >> 1) % need
    return (winner, need, loser_rounds) if winner == 1 else (winner, loser_rounds, need)


def game_ids() -> dict[str, int]:
    """The launcher's roster ids -> the game's own character ids (launcher/data/sc5.json, game_id)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "sc5.json")
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return {}
    return {c["id"]: c["game_id"] for roster in data.get("rosters", {}).values() for c in roster if "game_id" in c}


def fighter(pick: str, seed: int, side: int, ids: dict[str, int]) -> str:
    """A side's fighter as the game's result line names it ("3.0": its own id, the costume after a dot): the start
    line's game id or roster id, or for "-" (picked on the select screen) one from the seed, alike in both games."""
    if pick[:1].isdigit():
        return pick if "." in pick else pick + ".0"
    if pick in ids:
        return f"{ids[pick]}.0"
    pool = sorted(set(ids.values())) or [0]
    return f"{pool[(seed >> (4 * side)) % len(pool)]}.{(seed >> (8 + side)) % 2}"


def desync_log(start: dict[str, str], match: str, side: int) -> list[str]:
    """What SCV's game logs of a match that went out of step (sc5/src/sc5: link, autopilot, versus, net/netplay), its
    levels as the log has them: the setup, a statistics report every 10 s, round ends, the failed check and the
    detail lines after it (`sc5 desync:`, the game's own since 0.1.4)."""
    seed = int(start.get("seed", "0"))
    names = {"Siegfried": 8, "Mitsurugi": 16, "Ivy": 12}
    p1, p2 = (start.get(k, "-") for k in ("p1", "p2"))
    p1, p2 = (p if p in names else "Astaroth" if i == 0 else "Kilik" for i, p in enumerate((p1, p2)))
    i = "[info] [core] [t1] "
    out = [f"{i}sc5 link: start versus as player {side}, match {match}, seed {seed}, best of {start.get('rounds', '5')}, "
           f"the other at 127.0.0.1:{start.get('remote', ':0').rpartition(':')[2]}",
           f"{i}sc5 net: player {side} on port {start.get('local_port', '0')}, the other at "
           f"{start.get('remote', '127.0.0.1:0')}, rollback with input delay {start.get('delay', '1')}",
           f"{i}sc5 autopilot: side 1 picked {p1} (slot {names.get(p1, 3)}) at frame 3039",
           f"{i}sc5 autopilot: side 1 costume 1 at frame 3053",
           f"{i}sc5 autopilot: side 2 picked {p2} (slot {names.get(p2, 5)}) at frame 3182",
           f"{i}sc5 autopilot: side 2 costume 2 at frame 3196",
           f"{i}sc5 versus: VS settings at 402E1B5C: sides 1/1 and 2/2, rounds to win 3 and 3, round time 60, stage 17 "
           "(frame 3830)",
           f"{i}sc5 versus: stage 17, round time 60, 3 rounds to win, sent to player 2's game",
           f"{i}sc5 net: meeting for match 1 at frame 4958 (this player's buttons in a fight: the game's own)",
           f"{i}sc5 net: match 1: 2 blocks of the fight (384 bytes) differ in player 2's game, sent",
           f"{i}sc5 net: match 1: 3 agreed words changed: 82771B48:000013CA>000013CB 82771B4C:00000698>00000699",
           f"{i}sc5 net: match 1 in sync, seed {seed & 0xFFFFFFFF:08X}, input delay {start.get('delay', '1')}"]
    for n in range(1, 61):  # a long match: a statistics report every 600 frames, a round's end now and then
        f = 4958 + 600 * n
        out += [f"{i}sc5 net: frame {f}: {n * 3} rollbacks ({n * 7} frames run again, the longest 4 frames and 6.1 ms, "
                "1.52 ms a frame run again), 0 waits",
                f"{i}sc5 net: keeping a frame top 0.12 ms ({n * 590} kept, {n * 10} needed none); a rollback: 0.46 ms "
                "putting the battle back"]
        if n % 12 == 0:
            out.append(f"{i}sc5 versus: round {n // 12} over at frame {f + 30} (ring out by player {1 + n % 2})")
    out += [f"{i}sc5 autopilot: a round's end at frame 41500",
            f"{i}sc5 versus: ring out at frame 41712",
            f"[error] [core] [t1] sc5 net: DESYNC at frame 41730: 1234ABCD5678EF00 here, 0FEDCBA987654321 there"]
    for n in range(int(os.environ.get("SC5_FAKE_DETAIL", "60"))):
        out.append(f"{i}sc5 desync: frame {41700 + n % 30}: player {1 + n % 2} state {n:04X} pos 1.25 -0.50 3.75 "
                   f"inputs 00{n:02X} differs at block {n % 4} +{n * 64:04X}")
    out.append(f"[warning] [core] [t1] sc5 net: match 1 over: desync")
    return out


def log(text: str) -> None:
    print(f"[fake_game {os.environ.get('SC5_INSTANCE', '1')}] {text}", flush=True)


def write_atomic(path: str, text: str) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text + "\n")
    for _ in range(200):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.005)
    log(f"could not replace {path}")


def take_command(path: str) -> str | None:
    taken = path + ".taken"
    try:
        os.replace(path, taken)
    except FileNotFoundError:
        return None
    except PermissionError:
        return None
    with open(taken, encoding="ascii", errors="replace") as fh:
        return fh.readline().strip()


def parse(line: str) -> tuple[str, dict[str, str]]:
    verb, *rest = line.split()
    keys = {}
    for token in rest:
        if "=" in token:
            k, _, v = token.partition("=")
            keys[k] = v
    return verb, keys


def feed_packet(kind: int, side: int, barrier: int, payload: bytes = b"") -> bytes:
    pkt = FEED_MAGIC + bytes([kind, side]) + struct.pack("<H", barrier & 0xFFFF) + payload
    return struct.pack("<H", len(pkt)) + pkt


class Feed:
    """The TCP link to the launcher's FeedLink (feed=127.0.0.1:<port>): a player writes, a spectator reads."""

    def __init__(self, endpoint: str) -> None:
        host, _, port = endpoint.partition(":")
        self.addr = (host or "127.0.0.1", int(port or "0"))
        self.sock: socket.socket | None = None
        self.next_try = 0.0
        self.buf = b""

    def connected(self, now: float) -> bool:
        if self.sock is not None:
            return True
        if now < self.next_try:
            return False
        self.next_try = now + 0.5  # a game that lost the connection tries again every half second
        try:
            s = socket.create_connection(self.addr, timeout=1)
            s.setblocking(False)
            self.sock = s
            self.buf = b""
        except OSError:
            return False
        return True

    def send(self, data: bytes, now: float) -> None:
        if not self.connected(now):
            return
        try:
            self.sock.sendall(data)
        except OSError:
            self.close()

    def packets(self, now: float) -> list[bytes]:
        if not self.connected(now):
            return []
        while True:
            try:
                data = self.sock.recv(65536)
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                self.close()
                break
            if not data:
                self.close()
                break
            self.buf += data
        out = []
        while len(self.buf) >= 2:
            n = struct.unpack_from("<H", self.buf)[0]
            if len(self.buf) < 2 + n:
                break
            out.append(self.buf[2:2 + n])
            self.buf = self.buf[2 + n:]
        return out

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None


class FakeGame:
    def __init__(self, status_path: str, cmd_path: str) -> None:
        self.status_path = status_path
        self.cmd_path = cmd_path
        self.match_seconds = float(os.environ.get("SC5_FAKE_SECONDS", "6"))
        self.sock: socket.socket | None = None
        self.feed: Feed | None = None
        self.state = "booting"
        self.start: dict[str, str] = {}
        self.match = ""
        self.side = 1
        self.started_at = 0.0
        self.connected_at = 0.0
        self.last_peer = 0.0
        self.last_send = 0.0
        self.last_check = 0.0
        self.sent = 0
        self.frame = 0
        self.seen_ready: set[int] = set()
        self.seen_left: set[int] = set()
        self.choose_at = 0.0  # choose=1: when the loser's choice on the result screen is said

    def status(self, line: str) -> None:
        log(f"status: {line}")
        write_atomic(self.status_path, line)
        self.game_log(f"link: status {line}")  # every one, for tests that poll slower than a status lasts

    def game_log(self, *lines: str) -> None:
        """Lines as a real game's game.log has them (beside status.txt), what the launcher's reports read."""
        path = os.path.join(os.path.dirname(os.path.abspath(self.status_path)), "game.log")
        t = time.monotonic()
        with open(path, "a", encoding="ascii", errors="replace") as f:
            for line in lines:
                f.write(f"[{t:9.3f}] {line}\n")

    def close_socket(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None
        if self.feed is not None:
            self.feed.close()
            self.feed = None

    def begin(self, keys: dict[str, str]) -> None:
        self.close_socket()
        self.choose_at = 0.0
        self.start = keys
        self.match = keys.get("match", "match")
        self.started_at = time.monotonic()
        self.connected_at = 0.0
        self.last_peer = 0.0
        self.frame = 0
        self.seen_ready.clear()
        self.seen_left.clear()
        self.game_log(f"link: start {' '.join(f'{k}={v}' for k, v in keys.items() if k not in ('remote',))}")
        if keys.get("mode") == "practice":
            self.state = "practice"
            self.status("in_match match=practice")
            return
        self.status(f"starting match={self.match}")
        self.watching = keys.get("watch") == "1"
        if self.refused_keys():  # a pick the real game held back at its select screen, on every line from then on
            self.status(f"starting match={self.match}{self.refused_keys()}")
        if keys.get("feed"):
            self.feed = Feed(keys["feed"])
        if keys.get("watch") == "1":
            self.state = "watching"
            log(f"watching match {self.match} from feed {keys.get('feed')}")
            return
        self.side = int(keys.get("side", "1"))
        port = int(keys.get("local_port", "7001"))
        host, _, rport = keys.get("remote", "127.0.0.1:7002").partition(":")
        self.remote = (host, int(rport))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # a loopback peer needs no public socket (and then Windows asks nothing about the firewall)
        self.sock.bind(("127.0.0.1" if host.startswith("127.") else "0.0.0.0", port))
        self.sock.setblocking(False)
        self.state = "connecting"
        log(f"side {keys.get('side')} on udp {port} -> {self.remote}, seed {keys.get('seed')}")

    def handle(self, line: str) -> bool:
        log(f"cmd: {line}")
        verb, keys = parse(line)
        if verb == "quit":
            self.close_socket()
            return False
        if verb == "park" and os.environ.get("SC5_FAKE_STUCK") == "1" and self.state == "over":
            log("park not taken (SC5_FAKE_STUCK)")  # a game stuck at the match's end
        elif verb == "park":
            self.close_socket()
            self.state = "ready"
            self.status("ready")
        elif verb == "start":
            self.begin(keys)
        elif verb == "rematch" and self.start:
            again = dict(self.start)
            again.update(keys)
            self.begin(again)
        else:
            self.status(f"error bad command {verb}")
        return True

    def pump_network(self, now: float) -> None:
        if self.sock is None:
            return
        if now - self.last_send >= 0.1:
            self.sent += 1
            hb = {"type": "hb", "match": self.match, "side": self.side, "n": self.sent}
            try:
                self.sock.sendto(json.dumps(hb).encode(), self.remote)
            except OSError:
                pass
            self.last_send = now
            if self.feed is not None and self.state == "playing":  # the frames' inputs, as a spectator needs them
                self.frame += 6
                self.feed.send(feed_packet(7, self.side - 1, 1, struct.pack("<IB", self.frame, 1) + b"\0" * 8), now)
                if now - self.last_check >= 1.0:
                    self.last_check = now
                    self.feed.send(feed_packet(8, self.side - 1, 1, struct.pack("<II", self.frame, self.frame * 2654435761 & 0xFFFFFFFF)), now)
        while True:
            try:
                data, _ = self.sock.recvfrom(2048)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if msg.get("type") == "hb" and msg.get("match") == self.match:
                self.last_peer = now

    def finish(self) -> None:
        """the match is over in sync: the result (or Online Practice's end), and the frame it left at for spectators"""
        self.state = "over"
        if self.feed is not None:
            self.feed.send(feed_packet(9, self.side - 1, 1, struct.pack("<I", self.frame)), time.monotonic())
        if self.start.get("mode") == "sparring":
            self.status(f"sparring_over match={self.match}")
        elif os.environ.get("SC5_FAKE_RESULT") == "desync":
            self.game_log(*desync_log(self.start, self.match, self.side))
            self.status(f"desync frame=600 match={self.match}")
        else:
            self.status(self.result_line())
            if self.start.get("choose") == "1":
                self.choose_at = time.monotonic() + float(os.environ.get("SC5_FAKE_CHOOSE_SECONDS", "2"))

    def refused_keys(self) -> str:
        """SC5_FAKE_REFUSED: the status line's ending for a pick the game held back, as the real game writes it"""
        refused = os.environ.get("SC5_FAKE_REFUSED", "")
        return f" refused=1 refused_why={refused}" if refused and not getattr(self, "watching", False) else ""

    def result_line(self) -> str:
        seed = int(self.start.get("seed", "0"))
        rounds = int(self.start.get("rounds", "3"))
        if os.environ.get("SC5_FAKE_RESULT") == "draw":  # a double KO in the last round: nobody won
            winner, r1, r2 = 0, rounds // 2, rounds // 2
        else:
            winner, r1, r2 = simulated_result(seed, rounds)
        ids = game_ids()
        c1, c2 = (fighter(self.start.get(k, "-"), seed, side, ids) for side, k in ((1, "p1"), (2, "p2")))
        return f"result winner={winner} p1_rounds={r1} p2_rounds={r2} p1_char={c1} p2_char={c2} match={self.match}"

    def watch(self, now: float) -> None:
        for pkt in self.feed.packets(now) if self.feed else []:
            if len(pkt) < 8 or pkt[:4] != FEED_MAGIC:
                continue
            kind, side = pkt[4], pkt[5]
            if kind == 6:
                self.seen_ready.add(side)
            elif kind == 9:
                self.seen_left.add(side)
        if self.state == "watching" and self.seen_ready >= {0, 1}:
            self.state = "watching_match"
            self.status(f"in_match match={self.match}{self.refused_keys()}")
        if self.state == "watching_match" and self.seen_left >= {0, 1}:
            self.state = "over"
            if self.start.get("mode") == "sparring":
                self.status(f"sparring_over match={self.match}")
            else:
                self.status(self.result_line())

    def step(self, now: float) -> None:
        if self.state in ("watching", "watching_match"):
            self.watch(now)
        elif self.state == "connecting":
            self.pump_network(now)
            if self.last_peer:
                self.state = "playing"
                self.connected_at = now
                self.status(f"in_match match={self.match}{self.refused_keys()}")
                if self.feed is not None:  # the READY words and the seed at the meeting point
                    seed = int(self.start.get("seed", "0")) & 0xFFFFFFFF
                    self.feed.send(feed_packet(6, self.side - 1, 1, struct.pack("<I", seed)), now)
            elif now - self.started_at > 10:
                self.close_socket()
                self.state = "over"
                self.status("error no connection")
        elif self.state == "playing":
            self.pump_network(now)
            if now - self.last_peer > 3:
                self.close_socket()
                self.state = "over"
                self.status(f"disconnected match={self.match}")
            elif now - self.connected_at >= self.match_seconds:
                self.finish()
        elif self.state == "over":
            self.pump_network(now)  # keep answering the peer until park
            if self.choose_at and now >= self.choose_at:
                self.choose_at = 0.0
                self.status(f"chose choice={os.environ.get('SC5_FAKE_CHOICE', 'rematch')} match={self.match}")

    def run(self) -> int:
        env = " ".join(f"{k}={v}" for k, v in sorted(os.environ.items()) if k.startswith("SC5_"))
        self.game_log("Fake game (launcher/tools/fake_game.py)", f"CPU: {os.environ.get('PROCESSOR_IDENTIFIER', '?')}",
                      "Game: default.xex in fake, sha1 0000000000000000000000000000000000000000", f"env: {env}")
        self.status("booting")
        time.sleep(float(os.environ.get("SC5_FAKE_BOOT", "1.5")))
        self.state = "ready"
        self.status("ready")
        while True:
            line = take_command(self.cmd_path)
            if line and not self.handle(line):
                log("bye")
                return 0
            self.step(time.monotonic())
            time.sleep(0.02)


def main() -> int:
    status_path, cmd_path = os.environ.get("SC5_STATUS"), os.environ.get("SC5_CMD")
    if not status_path or not cmd_path:
        if os.environ.get("SC5_GAME"):  # started plainly by the launcher: single player or practice, until closed
            practice = os.environ.get("SC5_TRAINING") == "1"
            log(f"plain game ({'practice' if practice else 'single player'}): runs until closed")
            while True:
                time.sleep(1)
        print("run me from the SC5 Online launcher (SC5_STATUS and SC5_CMD are not set)", file=sys.stderr)
        return 2
    return FakeGame(status_path, cmd_path).run()


if __name__ == "__main__":
    sys.exit(main())
