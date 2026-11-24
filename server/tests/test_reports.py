"""F3 reports: a launcher sends what its player saw drawn wrong (the game's capture, its log) in parts; the server
writes each file once its parts are in, under its reports folder, and logs the report with the player. Desync reports:
a launcher sends its game log's lines of a match that went out of step, and the server pairs both players' reports."""

from __future__ import annotations

import base64
import os
import struct
import zlib

import pytest

from server import protocol as P
from server.client import ServerError

RID = "0123456789abcdef"


def qcompress(data: bytes) -> bytes:
    """As Qt's qCompress: the length big-endian, then zlib."""
    return struct.pack(">I", len(data)) + zlib.compress(data)


def part(name, data: bytes, index, count, *, rid=RID, game="sc5", compressed=False):
    msg = {"type": "report_file", "report": rid, "game": game, "name": name, "part": index, "parts": count,
           "data": base64.b64encode(data).decode()}
    if compressed:
        msg["zlib"] = True
    return msg


async def test_a_report_is_written_and_logged(server, connect, tmp_path, caplog):
    server.cfg.reports_dir = str(tmp_path / "reports")
    c = await connect("Raiden")
    picture = os.urandom(20000)
    c.send(part("final.jpg", picture[:12000], 0, 2))
    c.send(part("final.jpg", picture[12000:], 1, 2))
    text = b"0000 draw prim 5 count 12\n" * 2000
    c.send(part("frame.txt", qcompress(text), 0, 1, compressed=True))
    with caplog.at_level("INFO", logger="sc5.server"):
        saved = await c.request({"type": "report_done", "report": RID})
    assert saved["type"] == "report_saved" and saved["files"] == 2
    [day] = os.listdir(tmp_path / "reports")
    [folder] = os.listdir(tmp_path / "reports" / day)
    assert folder.endswith("-Raiden-sc5-" + RID)
    where = tmp_path / "reports" / day / folder
    assert (where / "final.jpg").read_bytes() == picture
    assert (where / "frame.txt").read_bytes() == text
    assert any("REPORT Raiden sc5" in r.getMessage() and "2 files" in r.getMessage() for r in caplog.records)


async def test_report_parts_come_in_order_within_limits(server, connect, tmp_path, monkeypatch):
    server.cfg.reports_dir = str(tmp_path / "reports")
    c = await connect("Kitana")
    for bad in (part("final.exe", b"x", 0, 1), part("../up.jpg", b"x", 0, 1), part("a.jpg", b"x", 1, 1),
                part("a.jpg", b"x", 0, 1, game="sf2"), {**part("a.jpg", b"x", 0, 1), "data": "not base64!"}):
        with pytest.raises(ServerError) as exc:
            await c.request(bad)
        assert exc.value.code == "bad_request"
    with pytest.raises(ServerError) as exc:  # nothing came: nothing to finish
        await c.request({"type": "report_done", "report": "fedcba9876543210"})
    assert exc.value.code == "bad_request"
    # a part that skips one
    c.send(part("next_00.jpg", b"abc", 0, 3, rid="1111111111111111"))
    with pytest.raises(ServerError) as exc:
        await c.request(part("next_00.jpg", b"abc", 2, 3, rid="1111111111111111"))
    assert exc.value.code == "bad_request"
    # too large: the report's files together
    monkeypatch.setattr("server.app.REPORT_MAX_BYTES", 1000)
    with pytest.raises(ServerError) as exc:
        await c.request(part("big.png", b"x" * 1001, 0, 1, rid="2222222222222222"))
    assert exc.value.code == "too_large"
    # a compressed file that inflates past the limit
    with pytest.raises(ServerError) as exc:
        await c.request(part("log.txt", qcompress(b"y" * 5000), 0, 1, rid="2222222222222222", compressed=True))
    assert exc.value.code == "too_large"
    # three reports a connection
    with pytest.raises(ServerError) as exc:
        await c.request(part("a.jpg", b"x", 0, 1, rid="3333333333333333"))
    assert exc.value.code == "too_many"


async def test_a_server_without_a_reports_folder_takes_none(server, connect):
    server.cfg.reports_dir = None
    c = await connect("Jax")
    with pytest.raises(ServerError) as exc:
        await c.request(part("final.jpg", b"x", 0, 1))
    assert exc.value.code == "unavailable"


# --- desync reports (docs/PROTOCOL.md 1.13.1) ------------------------------------------------------------------------

def desync_lines(side: int, *, stage: str = "17", p1: str = "Siegfried", p2: str = "Mitsurugi", detail: int = 80) -> list[str]:
    """A report's lines as an SCV 0.1.4 launcher sends them: the setup, the last statistics, the last lines before
    the failed check, the check and the game's own detail lines after it, the game files and the PC."""
    i = "[22:14:03.120] [info] "
    lines = [f"{i}OS: Windows 10.0.26200", f"{i}CPU: AMD Ryzen 7 9800X3D 8-Core Processor (16 threads)",
             f"{i}sc5 link: command start game=sc5 mode=versus side={side} local_port=7001 remote=x.x.x.x:7001 "
             f"delay=2 seed=12345 p1={p1} p2={p2} rounds=5 ranked=0 match=7658cd7bfcd7",
             f"{i}sc5 link: start versus as player {side}, match 7658cd7bfcd7, seed 12345, best of 5",
             f"{i}sc5 autopilot: side 1 picked {p1} (slot 8) at frame 3039",
             f"{i}sc5 autopilot: side 2 picked {p2} (slot 16) at frame 3182",
             f"{i}sc5 versus: VS settings at 402E1B5C: sides 1/1 and 2/2, rounds to win 3 and 3, round time 60, "
             f"stage {stage} (frame 3830)",
             f"{i}sc5 net: meeting for match 1 at frame 4958 (this player's buttons in a fight: the game's own)",
             f"{i}sc5 net: frame 40958: 61 rollbacks (140 frames run again, the longest 4 frames and 6.1 ms)",
             f"{i}sc5 versus: ring out at frame 41712"]
    lines += [f"{i}sc5 versus: round end {n} at frame {41000 + n}" for n in range(30)]
    lines.append("[2026-11-20 22:14:04.001] [error] [core] [t1] sc5 net: DESYNC at frame 41730: 1234ABCD5678EF00 "
                 "here, 0FEDCBA987654321 there")
    lines += [f"{i}sc5 desync: frame {41700 + n}: player {1 + n % 2} " + "x" * 120 for n in range(detail)]
    lines += ["Game files: 587 files, 4050117929 bytes, names and sizes 3017906901d6341d, contents 0011223344556677",
              "This PC: AMD Ryzen 7 9800X3D 8-Core Processor, 16 threads, Windows 10.0.26200, game code for x86-64-v3"]
    return lines


async def test_a_long_desync_report_is_taken_and_logged(server, connect, caplog):
    """The game's detail lines after its failed check (up to about 80) make a report longer than any other message:
    up to 200 lines of 200 printable characters, in a message up to 64 KB; anything else that long still closes the
    connection, and the lines are checked as strictly as before."""
    c = await connect("Raiden")
    lines = desync_lines(1, detail=80) + [f"extra {n} " + "y" * 180 for n in range(60)]
    assert len(lines) <= 200 and len(P.encode({"type": "desync_report", "lines": lines})) > P.MAX_LINE
    with caplog.at_level("INFO", logger="sc5.server"):
        ack = await c.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": 41730,
                               "lines": lines})
    assert ack["type"] == "desync_ack"
    said = [r.getMessage() for r in caplog.records]
    assert any(f"match 7658cd7bfcd7 at frame 41730: {len(lines)} lines" in m for m in said)
    assert sum(1 for m in said if m.startswith("DESYNC_REPORT Raiden | ") and "sc5 desync:" in m) == 80
    for bad in ({"lines": ["line"] * 201}, {"lines": ["x" * 201]}, {"lines": ["tab\there"]}):
        with pytest.raises(ServerError) as exc:
            await c.request({"type": "desync_report", "game": "sc5", "match": "m", "frame": 1, **bad})
        assert exc.value.code == "bad_request"
    # a chat line as long is not a desync report: refused, the connection closed
    c.send({"type": "lobby_chat", "text": "z" * (P.MAX_LINE + 100)})
    err = await c.expect("error", code="too_long")
    assert err["code"] == "too_long"


async def test_too_long_even_for_a_desync_report(server, connect):
    c = await connect("Kitana")
    c.send({"type": "desync_report", "game": "sc5", "match": "m", "frame": 1, "lines": ["w" * 199] * 400})
    err = await c.expect("error", code="too_long")
    assert err["code"] == "too_long"


async def test_both_players_reports_of_a_match_make_one_pair_line(server, connect, caplog):
    """Once both players' reports of a match are in, the server logs one DESYNC_PAIR line naming both players, the
    frame each game saw the check fail at, the stage and the two fighters, read from each report's lines."""
    a, b, c = await connect("Taki"), await connect("Kilik"), await connect("Ivy")
    with caplog.at_level("INFO", logger="sc5.server"):
        await a.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": 41730,
                         "lines": desync_lines(1)})
        await a.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": 41730,
                         "lines": desync_lines(1)})  # the same player again: still waiting for the other
        await c.request({"type": "desync_report", "game": "sc5", "match": "aaaaaaaaaaaa", "frame": 30,
                         "lines": desync_lines(1)})  # another match's: no pair
        assert not [r for r in caplog.records if r.getMessage().startswith("DESYNC_PAIR")]
        await b.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": 41731,
                         "lines": desync_lines(2, stage="12")})
    pairs = [r.getMessage() for r in caplog.records if r.getMessage().startswith("DESYNC_PAIR")]
    assert pairs == ["DESYNC_PAIR 7658cd7bfcd7 Taki at frame 41730, Kilik at frame 41731 (the frames differ); "
                     "stage 17 / 12 (they differ); Siegfried vs Mitsurugi (launchers 0.1.0 and 0.1.0)"]
    assert "7658cd7bfcd7" not in server.desyncs and "aaaaaaaaaaaa" in server.desyncs


async def test_a_round_played_again_is_reported_and_the_pair_says_so(server, connect, caplog):
    """Two games that went out of step and played the round again (SCV 0.1.4): each launcher's report has `again`,
    the server's lines say the match went on, and a second one in the same match pairs again."""
    a, b = await connect("Taki"), await connect("Kilik")
    with caplog.at_level("INFO", logger="sc5.server"):
        for frame in (930, 4020):
            await a.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": frame,
                             "again": True, "lines": desync_lines(1)})
            await b.request({"type": "desync_report", "game": "sc5", "match": "7658cd7bfcd7", "frame": frame,
                             "again": True, "lines": desync_lines(2)})
        with pytest.raises(ServerError):
            await a.request({"type": "desync_report", "game": "sc5", "match": "m", "frame": 1, "again": "yes",
                             "lines": []})
    said = [r.getMessage() for r in caplog.records]
    assert len([m for m in said if m.startswith("DESYNC_REPORT Taki sc5") and "the round played again" in m]) == 2
    pairs = [m for m in said if m.startswith("DESYNC_PAIR")]
    assert len(pairs) == 2 and all(m.endswith("; the round was played again and the match went on") for m in pairs)
    assert "at frame 930, Kilik at frame 930;" in pairs[0] and "at frame 4020, Kilik at frame 4020;" in pairs[1]


def test_desync_setup_reads_the_stage_and_the_fighters():
    from server.app import desync_setup
    assert desync_setup(desync_lines(1, stage="1A", p1="Ivy", p2="Astaroth")) == ("1A", ("Ivy", "Astaroth"))
    # a rematch from the result screen picks nothing on the select screen: the fighters its start line names
    rematch = ["[x] [info] [core] [t1] sc5 link: command rematch match=b06642be6eba seed=1 p1=3.0 p2=0.0 stage=keep",
               "[x] [info] [core] [t1] sc5 versus: player 1's stage 9, round time 60, 3 rounds to win, 2 words"]
    assert desync_setup(rematch) == ("9", ("3.0", "0.0"))
    assert desync_setup([]) == ("?", ("?", "?"))
