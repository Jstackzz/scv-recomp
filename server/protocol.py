"""Constants, message encoding and field validation shared by the server and the test client."""

from __future__ import annotations

import ipaddress
import json
import re
from typing import Any

from . import __version__

PROTO = 1
SERVER_NAME = "sc5-server"
SERVER_VERSION = __version__

MAX_LINE = 16 * 1024
MAX_DESYNC_LINE = 64 * 1024  # a desync_report alone may be this long (its lines, docs/PROTOCOL.md 1.13.1)
MAX_DATAGRAM = 512
MAX_RELAY_DATAGRAM = 1472  # what the relay passes on: a game packet, up to a whole Ethernet frame of UDP payload

# game id -> online modes: Versus (ranked and casual, the only mode a queue has) and Online Practice ("sparring").
# The game's offline modes (Training, Arcade...) never reach the server
GAMES: dict[str, tuple[str, ...]] = {
    "sc5": ("versus", "sparring"),
}
# the modes whose players can pick on the game's own select screen (the launcher's "-"): a series' next match goes
# back there in them
SELECT_SCREEN_MODES: frozenset[str] = frozenset({"versus", "sparring"})
# Online Practice: two friends by a lobby's code (never listed, never queued), fights without a timer or a knockout
# until one of them goes back to the select screen; nothing is recorded
LOBBY_ONLY_MODES: frozenset[str] = frozenset({"sparring"})
REGIONS: tuple[str, ...] = ("na", "sa", "eu", "me", "af", "as", "oc")

NAME_RE = re.compile(r"^[A-Za-z0-9_-]{3,16}$")
CHARACTER_RE = re.compile(r"^[A-Za-z0-9_.-]{0,24}$")
TOKEN_RE = re.compile(r"^[0-9a-f]{16,128}$")
PASSWORD_MIN, PASSWORD_MAX = 6, 128
LOBBY_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
LOBBY_CODE_LEN = 5
LOBBY_NAME_MAX = 32
CHAT_MAX = 200
ROUNDS_ALLOWED = (1, 3, 5)
DELAY_MAX = 4  # a player's own input delay, frames
DELAY_AUTO = -1  # a player's "auto" input delay: the match's from its measured round trip (auto_delay)
AUTO_DELAY_MIN, AUTO_DELAY_MAX = 1, 6
# what a launcher can do beyond the protocol's start, as its hello says (`features`) and the server's welcome lists:
# "koth", King of the Hill lobbies (docs/PROTOCOL.md 1.10.2)
# "desync_detail" (SCV 0.1.4, the server's alone): desync reports of up to 200 lines in a message of up to 64 KB
# (docs/PROTOCOL.md 1.13.1); a launcher keeps its report to the 40 lines of 16 KB an older server takes without it
# "pick_refused" (SCV 0.1.7, the server's alone): it logs a created character the game held back at the select screen,
# with why (docs/PROTOCOL.md 1.13.3); a launcher sends none to a server without it
FEATURES: tuple[str, ...] = ("koth", "desync_detail", "pick_refused")
RATING_MIN, RATING_MAX = 1, 10  # a King of the Hill rating of the winner, a whole number


class ProtocolError(Exception):
    """A request that cannot be served; turned into an `error` message for the client."""

    def __init__(self, code: str, message: str = "") -> None:
        super().__init__(message or code)
        self.code = code
        self.message = message or code.replace("_", " ")


def encode(msg: dict[str, Any]) -> bytes:
    """One NDJSON line."""
    return (json.dumps(msg, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def encode_datagram(msg: dict[str, Any]) -> bytes:
    return json.dumps(msg, separators=(",", ":")).encode("utf-8")


def decode_line(line: bytes) -> dict[str, Any]:
    try:
        msg = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError("bad_json", "Message is not valid JSON.") from exc
    if not isinstance(msg, dict) or not isinstance(msg.get("type"), str):
        raise ProtocolError("bad_json", "Message must be a JSON object with a string 'type'.")
    return msg


def parse_version(text: str) -> tuple[int, ...]:
    parts = []
    for piece in str(text).split("."):
        digits = re.match(r"\d+", piece)
        parts.append(int(digits.group(0)) if digits else 0)
    return tuple(parts) or (0,)


def version_at_least(version: str, minimum: str) -> bool:
    a, b = parse_version(version), parse_version(minimum)
    width = max(len(a), len(b))
    return a + (0,) * (width - len(a)) >= b + (0,) * (width - len(b))


def download_link(url: Any) -> bool:
    """A link launchers may download from: https, or plain http to this PC (development)."""
    return isinstance(url, str) and len(url) <= 500 and (
        url.startswith("https://") or re.match(r"http://(127\.0\.0\.1|localhost)(:\d+)?/", url) is not None)


BUILD_RE = re.compile(r"[0-9A-Za-z :._-]{1,40}")  # a package's build: its VERSION.txt stamp ("2026-11-23 22:31")


def client_build(value: Any) -> str:
    """A launcher's build as its hello says it ("" for none, or one this server does not take)."""
    return value.strip() if isinstance(value, str) and BUILD_RE.fullmatch(value.strip()) else ""


def parse_update(data: Any, page: str = "") -> dict[str, Any]:
    """The release launchers are offered (the server's update file); ValueError says what is wrong with it. `page`:
    the web page the release names when the file names none (the server's --releases-page; "": no page at all)."""
    if not isinstance(data, dict):
        raise ValueError("not a JSON object")
    version, url, sha256, size = data.get("version"), data.get("url"), data.get("sha256"), data.get("size")
    notes, page = data.get("notes", ""), data.get("page", page)
    if not isinstance(version, str) or not re.fullmatch(r"\d+(\.\d+)*", version):
        raise ValueError("version must look like 0.1.8")
    if not download_link(url):
        raise ValueError("url must be an https:// link to the release's zip")
    if not isinstance(sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise ValueError("sha256 must be the zip's SHA-256, 64 lowercase hex digits")
    if isinstance(size, bool) or not isinstance(size, int) or not 0 < size <= 1 << 30:
        raise ValueError("size must be the zip's size in bytes")
    if not isinstance(notes, str) or len(notes) > 2000:
        raise ValueError("notes must be text, 2000 characters at most")
    if "page" in data and not download_link(page):  # the server's own page was checked when it started
        raise ValueError("page must be an https:// link")
    out = {"version": version, "url": url, "sha256": sha256, "size": size, "notes": notes, "page": page}
    if "build" in data:  # which build of the version: the zip's VERSION.txt stamp (a launcher's hello `build`)
        build = data["build"]
        if not isinstance(build, str) or not BUILD_RE.fullmatch(build):
            raise ValueError("build must be the release's VERSION.txt stamp, as 2026-11-23 22:31")
        out["build"] = build
    return out


def format_endpoint(addr: tuple[str, int] | None) -> str:
    if not addr:
        return ""
    return f"{addr[0]}:{addr[1]}"


def parse_endpoint(text: Any) -> tuple[str, int] | None:
    """'1.2.3.4:7001' -> ('1.2.3.4', 7001); None when malformed (IPv4 only in v1)."""
    if not isinstance(text, str) or text.count(":") != 1:
        return None
    host, _, port = text.partition(":")
    try:
        ip = ipaddress.IPv4Address(host)
        port_no = int(port)
    except ValueError:
        return None
    if not 0 < port_no < 65536:
        return None
    return str(ip), port_no


# --- field access -------------------------------------------------------------------------------


def get_str(msg: dict, key: str, *, default: str | None = None, max_len: int = 256) -> str:
    value = msg.get(key, default)
    if value is None or not isinstance(value, str):
        raise ProtocolError("bad_request", f"Field '{key}' must be a string.")
    if len(value) > max_len:
        raise ProtocolError("bad_request", f"Field '{key}' is too long.")
    return value


def get_bool(msg: dict, key: str, *, default: bool | None = None) -> bool:
    value = msg.get(key, default)
    if not isinstance(value, bool):
        raise ProtocolError("bad_request", f"Field '{key}' must be true or false.")
    return value


def get_int(msg: dict, key: str, *, lo: int, hi: int, default: int | None = None) -> int:
    value = msg.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError("bad_request", f"Field '{key}' must be an integer.")
    if not lo <= value <= hi:
        raise ProtocolError("bad_request", f"Field '{key}' must be between {lo} and {hi}.")
    return value


def get_delay(msg: dict, *, default: int = 2) -> int:
    """A player's input delay: 0..DELAY_MAX frames, or DELAY_AUTO for `"delay_auto": true` (SCV launchers from 0.1.4,
    which send a number with it for older servers) or `"delay": "auto"` (MK9 Online's form)."""
    if msg.get("delay") == "auto" or get_bool(msg, "delay_auto", default=False):
        return DELAY_AUTO
    return get_int(msg, "delay", lo=0, hi=DELAY_MAX, default=default)


def auto_delay(rtt_ms: int) -> int:
    """The input delay an "auto" player's match gets for its round trip: the frames of the one-way trip (60 a second,
    16.7 ms each) rounded up, less the one rollback covers, from 1 to 6. Under 67 ms 1, then a frame more every 33 ms:
    2 to 100 ms, 3 to 133, 4 to 166, 5 to 200, 6 above. Rollback then has at most a frame or two to correct, plus the
    network's jitter: SCV's rollbacks are cheap (1.5 ms a frame run again) but every frame of one shows as a
    character jumping. MK3D Online's rule (the one-way frames rounded, less one: delay 1 up to 83 ms) left a US-UK
    pair (2026-11-20) at delay 1 and rolling back up to 5 frames; MK9 Online's (rounded up plus half a frame, 2 to 8)
    is a frame more, for a game whose rollbacks cost 7-10 ms a frame."""
    if rtt_ms <= 0:
        return AUTO_DELAY_MIN
    frames = -(-rtt_ms * 3 // 100)  # ceil(rtt_ms / 33.33): the one-way trip's frames, rounded up
    return max(AUTO_DELAY_MIN, min(AUTO_DELAY_MAX, frames - 1))


def get_game_mode(msg: dict) -> tuple[str, str]:
    game = msg.get("game")
    if game not in GAMES:
        raise ProtocolError("bad_game", f"Unknown game {game!r}.")
    mode = msg.get("mode")
    if mode not in GAMES[game]:
        raise ProtocolError("bad_mode", f"Mode {mode!r} is not an online mode of {game}.")
    return game, mode


def get_region(msg: dict, key: str = "region") -> str:
    region = msg.get(key)
    if region not in REGIONS:
        raise ProtocolError("bad_region", f"Unknown region {region!r}.")
    return region


def get_character(msg: dict, key: str = "character") -> str:
    value = msg.get(key, "")
    if value is None:
        value = ""
    if not isinstance(value, str) or not CHARACTER_RE.match(value):
        raise ProtocolError("bad_request", "Character ids are up to 24 letters, digits, '_', '.' or '-'.")
    return value
