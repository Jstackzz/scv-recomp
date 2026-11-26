"""A stand-in for the launcher's side of the game's online menus (docs/PROTOCOL.md 3.8), for tests and pictures: it
writes ui.json into a game's session folder as the launcher would (replaced whole, `t` moving on every second) and
answers the game's ask.txt (claimed by renaming it to ask.txt.taken) the way the server and the launcher would, from a
small model of its own: a search finds an opponent after a while, a match found counts down, lobbies have members, the
leaderboard has rows. A timeline (--timeline, JSON) sets any field at a given second, so every state the launcher can be
in shows: signed out, offline, not answering (`"stop": true`), an update, a notice, a series after a match.

    python -I tools/fake_launcher.py --session DIR [--name Taki] [--account signed_in|signed_out|connecting|offline]
                                     [--found-after 6] [--no-find] [--timeline FILE] [--control FILE]
                                     [--seconds 3600] [--log FILE] [--quiet]

Prints every ask it takes (and appends it to --log, one line each: the seconds since start, then the ask's line), every
change of the game's status.txt, and what it changes. The game must run with SC5_NET=1, SC5_MENUS=1 and SC5_SESSION_DIR
= the same folder (tools/linktest.py --menus starts such games).

Timeline: a JSON list of steps, each {"at": seconds, "set": {"queue.state": "found", "account.state": "offline", ...}}
(dotted paths into ui.json; a whole object or list as the value), or {"at": s, "message": "text", "kind": "error"},
or {"at": s, "stop": true} / {"at": s, "stop": false}: ui.json is no longer written (the launcher not answering), and
again. --control FILE: the same steps without "at", one JSON object a line, each applied as soon as it is appended
(a test driving the game steers the launcher with it as it goes).
"""
import argparse
import copy
import json
import os
import sys
import time
import urllib.parse

OPPONENTS = [("Kilik_Fan", "soul-warrior", 38), ("NightmareMain", "veteran", 71), ("Leixia88", "squire", 22)]
LOBBIES = [
    {"code": "K7Q2", "name": "Late Night Fights", "host": "Voldo_Enjoyer", "players": 2, "max": 8, "mode": "versus",
     "koth": True, "locked": False, "region": "EU"},
    {"code": "M3X9", "name": "Training Room", "host": "PyrrhaO", "players": 1, "max": 2, "mode": "sparring",
     "koth": False, "locked": False, "region": "NA"},
    {"code": "Z1B4", "name": "First to Three", "host": "Siegfried_Schtauffen", "players": 5, "max": 8,
     "mode": "versus", "koth": False, "locked": False, "region": "NA"},
    {"code": "Q8L0", "name": "Casual Sets", "host": "Astaroth", "players": 8, "max": 8, "mode": "versus",
     "koth": True, "locked": True, "region": "EU"},
]
BOARD_NAMES = ["Xiba_Prime", "EdgeMasterRay", "Viola_Eyes", "Kilik_Fan", "Mitsurugi99", "NightmareMain", "{me}",
               "ZweiTank", "Leixia88", "Natsu_Kunoichi", "Raphael_F", "Hilde_Wolf", "Tira_Jolly", "Ivy_Snake",
               "AeonCalcos", "Yoshi_Mitsu", "Maxi_Nunchaku", "Cervantes_Gun", "AlgolKing", "Ezio_A"]
RANKS = ["soul-champion", "champion", "hero", "veteran", "soul-warrior", "squire", "novice"]


def now_ms():
    return int(time.time() * 1000)


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    for _ in range(200):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.005)


def claim(path):
    """ask.txt -> ask.txt.taken (the game writes the next one once this one is gone), its line"""
    taken = path + ".taken"
    try:
        os.replace(path, taken)
    except OSError:
        return None
    for _ in range(20):
        try:
            with open(taken, encoding="ascii") as f:
                return f.readline().strip()
        except OSError:
            time.sleep(0.005)
    return None


def parse_ask(line):
    parts = line.split()
    if len(parts) < 3 or parts[0] != "ask" or not parts[1].startswith("n="):
        return None, None, {}
    keys = {}
    for tok in parts[3:]:
        k, _, v = tok.partition("=")
        keys[k] = urllib.parse.unquote(v)
    return int(parts[1][2:]), parts[2], keys


class Launcher:
    def __init__(self, a):
        self.a = a
        self.me = a.name
        self.ui = {
            "v": 1, "t": now_ms(),
            "account": {"state": a.account, "name": self.me if a.account == "signed_in" else "", "rank": "hero",
                        "elo": 1612, "wins": 48, "losses": 31,
                        "error": "the server did not answer" if a.account == "offline" else ""},
            "server": {"online": 37, "in_matches": 12, "notice": ""},
            "side": "off",
            "queue": {"state": "idle", "kind": "", "secs": 0, "accept_left": 0, "opponent": "", "opponent_rank": "",
                      "ping": 0, "text": ""},
            "series": {"kind": "", "opponent": "", "mine": 0, "theirs": 0, "to": 0, "state": "", "text": ""},
            "lobbies": {"at": 0, "list": []},
            "lobby": None,
            "leaderboard": {"at": 0, "rows": []},
            "message": {"id": 0, "kind": "info", "text": ""},
            "update": {"version": "", "text": ""},
        }
        self.stopped = False
        self.events = []  # (at, fn): the model's own timers
        self.t0 = time.time()
        self.opp = 0
        self.found_at = time.time()
        self.search_start = time.time()
        self.search_gen = 0
        self.written = ""
        self.last_write = 0

    def say(self, text):
        if not self.a.quiet:
            print(f"{time.time() - self.t0:7.1f}s {text}", flush=True)

    def later(self, secs, fn):
        self.events.append((time.time() + secs, fn))

    def message(self, text, kind="info"):
        self.ui["message"] = {"id": self.ui["message"]["id"] + 1, "kind": kind, "text": text}

    def set_path(self, path, value):
        node = self.ui
        keys = path.split(".")
        for k in keys[:-1]:
            if node.get(k) is None:
                node[k] = {}
            node = node[k]
        node[keys[-1]] = copy.deepcopy(value)

    # --- the asks ---

    def ask(self, verb, k):
        q = self.ui["queue"]
        if verb == "queue":
            if self.ui["lobby"] is not None:
                self.ui["lobby"] = None
            q.update(state="searching", kind=k.get("kind", "casual"), secs=0, accept_left=0, opponent="",
                     opponent_rank="", ping=0, text="")
            self.search_start = time.time()
            if self.a.found_after >= 0:
                self.search_gen += 1
                gen = self.search_gen
                self.later(self.a.found_after, lambda: self.found(gen))
        elif verb == "cancel":
            q.update(state="idle", kind="", secs=0, text="")
        elif verb in ("accept", "decline"):
            if q["state"] != "found":
                return
            if verb == "decline":
                q.update(state="idle", kind="", secs=0, accept_left=0, opponent="", text="")
            else:
                q.update(state="accepted", text="")
                gen = self.search_gen
                self.later(2.5, lambda: gen == self.search_gen and q["state"] == "accepted" and q.update(state="connecting", text="Reaching the other player's game"))
                self.later(5.0, lambda: gen == self.search_gen and q["state"] == "connecting" and q.update(state="starting", text=""))
        elif verb == "leave":
            self.ui["series"] = {"kind": "", "opponent": "", "mine": 0, "theirs": 0, "to": 0, "state": "", "text": ""}
        elif verb == "side":
            self.ui["side"] = k.get("value", "off") if k.get("value") in ("off", "left", "right") else "off"
        elif verb == "lobbies":
            self.ui["lobbies"] = {"at": now_ms(), "list": copy.deepcopy(LOBBIES)}
        elif verb == "lobby_create":
            code = "R5T1"
            self.ui["lobby"] = {"code": code, "name": k.get("name", "Lobby"), "host": self.me,
                                "mode": k.get("mode", "versus"), "koth": k.get("koth") == "1", "ready": False,
                                "state": "waiting", "text": f"Waiting for players. Others join with the code {code}.",
                                "members": [{"name": self.me, "ready": False, "role": "player", "wins": 0}]}
            self.later(3, self.someone_joins)
        elif verb == "lobby_join":
            row = next((r for r in LOBBIES if r["code"] == k.get("code")), None)
            if row is None:
                self.message("That lobby is gone.", "error")
                return
            watch = k.get("watch") == "1"
            members = [{"name": row["host"], "ready": True, "role": "player", "wins": 3},
                       {"name": "Mitsurugi99", "ready": False, "role": "player", "wins": 1},
                       {"name": self.me, "ready": False, "role": "spectator" if watch else "player", "wins": 0}]
            self.ui["lobby"] = {"code": row["code"], "name": row["name"], "host": row["host"], "mode": row["mode"],
                                "koth": row["koth"], "ready": False, "state": "waiting",
                                "text": "You are watching: the matches show here as they start." if watch else
                                "Next up: Voldo_Enjoyer against Mitsurugi99, then you.",
                                "members": members}
        elif verb == "lobby_ready":
            lob = self.ui["lobby"]
            if lob:
                lob["ready"] = k.get("value") == "1"
                for m in lob["members"]:
                    if m["name"] == self.me:
                        m["ready"] = lob["ready"]
        elif verb == "lobby_leave":
            self.ui["lobby"] = None
        elif verb == "leaderboard":
            rows = []
            for i, n in enumerate(BOARD_NAMES):
                name = self.me if n == "{me}" else n
                rows.append({"place": i + 1, "name": name, "rank": RANKS[min(i // 3, len(RANKS) - 1)],
                             "elo": 1900 - i * 23, "wins": 120 - i * 4, "losses": 30 + i * 2})
            self.ui["leaderboard"] = {"at": now_ms(), "rows": rows}
        else:
            self.say(f"(an ask this stand-in does not know: {verb})")

    def found(self, gen):
        q = self.ui["queue"]
        if gen != self.search_gen or q["state"] != "searching":
            return
        name, rank, ping = OPPONENTS[self.opp % len(OPPONENTS)]
        self.opp += 1
        q.update(state="found", opponent=name, opponent_rank=rank, ping=ping, accept_left=15, text="")

    def someone_joins(self):
        lob = self.ui["lobby"]
        if lob and len(lob["members"]) == 1:
            lob["members"].append({"name": "Mitsurugi99", "ready": True, "role": "player", "wins": 0})
            lob["text"] = "Mitsurugi99 joined. The match starts when both players are ready."

    def tick(self):
        q = self.ui["queue"]
        if q["state"] == "searching":
            q["secs"] = int(time.time() - self.search_start)
        elif q["state"] == "found":
            q["accept_left"] = max(0, 15 - int(time.time() - self.found_at))
            if q["accept_left"] == 0:
                q.update(state="idle", kind="", opponent="", opponent_rank="", ping=0,
                         text="The match was not accepted in time.")
        for e in [e for e in self.events if e[0] <= time.time()]:
            self.events.remove(e)
            e[1]()

    def write(self, path):
        """ui.json replaced whole when anything changed, and at least once a second (`t` moving on)"""
        if self.stopped:
            return
        body = dict(self.ui)
        body["t"] = 0
        text = json.dumps(body, sort_keys=True)
        if text != self.written or time.time() - self.last_write >= 1.0:
            self.written = text
            self.last_write = time.time()
            self.ui["t"] = now_ms()
            write_atomic(path, json.dumps(self.ui, ensure_ascii=False, indent=1) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True)
    ap.add_argument("--name", default="Taki")
    ap.add_argument("--account", default="signed_in", choices=["signed_in", "signed_out", "connecting", "offline"])
    ap.add_argument("--found-after", type=float, default=6, help="seconds from a search to its match found")
    ap.add_argument("--no-find", action="store_true", help="a search finds nobody")
    ap.add_argument("--timeline", default="", help="a JSON file of steps (see the top of this file)")
    ap.add_argument("--control", default="", help="a file of steps (one JSON object a line) applied as they are appended")
    ap.add_argument("--seconds", type=float, default=3600)
    ap.add_argument("--log", default="", help="the asks taken, one line each")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    if a.no_find:
        a.found_after = -1
    ui_path = os.path.join(a.session, "ui.json")
    ask_path = os.path.join(a.session, "ask.txt")
    status_path = os.path.join(a.session, "status.txt")
    timeline = []
    if a.timeline:
        with open(a.timeline, encoding="utf-8") as f:
            timeline = sorted(json.load(f), key=lambda s: s.get("at", 0))
    L = Launcher(a)
    control_at = 0

    def apply(step):
        for path, value in step.get("set", {}).items():
            L.set_path(path, value)
            L.say(f"set {path} = {json.dumps(value)[:80]}")
        if "message" in step:
            L.message(step["message"], step.get("kind", "info"))
            L.say(f"message {step['message']!r}")
        if "stop" in step:
            L.stopped = bool(step["stop"])
            L.say("ui.json no longer written (the launcher does not answer)" if L.stopped else "ui.json written again")

    last_status = ""
    found_state = ""
    while time.time() - L.t0 < a.seconds:
        el = time.time() - L.t0
        while timeline and timeline[0].get("at", 0) <= el:
            apply(timeline.pop(0))
        if a.control and os.path.exists(a.control):
            with open(a.control, encoding="utf-8", newline="") as f:
                f.seek(control_at)
                for row in f:
                    if not row.endswith("\n"):
                        break
                    control_at += len(row.encode("utf-8"))
                    if row.strip():
                        apply(json.loads(row))
        line = claim(ask_path)
        if line:
            n, verb, keys = parse_ask(line)
            L.say(f"ask {line}")
            if a.log:
                with open(a.log, "a", encoding="utf-8") as f:
                    f.write(f"{el:.1f} {line}\n")
            if verb:
                L.ask(verb, keys)
        # the accept countdown, from when the match was found (by the model or the timeline)
        q = L.ui["queue"]
        if q["state"] == "found" and found_state != "found":
            L.found_at = time.time()
        found_state = q["state"]
        L.tick()
        L.write(ui_path)
        try:
            with open(status_path) as f:
                st = f.readline().strip()
        except OSError:
            st = ""
        if st != last_status:
            L.say(f"status {st}")
            last_status = st
        time.sleep(0.1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
