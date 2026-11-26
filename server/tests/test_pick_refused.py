"""A created character the game held back at the select screen (SCV 0.1.7): the launcher sends the game's reason, and
the server logs one PICK_REFUSED line per refused pick with both players' names and launchers, the match and why."""

from __future__ import annotations

import pytest

from server import protocol as P
from server.client import ServerError

from .test_results import setup_match


def test_the_server_says_it_takes_refused_picks():
    assert "pick_refused" in P.FEATURES


async def test_a_refused_pick_is_logged_with_both_players(server, connect, udp_peers, caplog):
    sides, match = await setup_match(server, connect, udp_peers, ["Finn", "Defi"], ranked=False)
    (c1, go1), (c2, _) = sides[1], sides[2]
    reason = ("Hair: it wears downloadable parts the other player's PC does not have, even without its equipment")
    with caplog.at_level("INFO", logger="sc5.server"):
        ack = await c1.request({"type": "pick_refused", "game": "sc5", "match": match, "reason": reason})
        assert ack["type"] == "pick_refused_ack"
        await c2.request({"type": "pick_refused", "game": "sc5", "match": match,
                          "reason": "player 1's created character from the other PC was refused: its checksum is wrong"})
    said = [r.getMessage() for r in caplog.records if r.getMessage().startswith("PICK_REFUSED")]
    me, them = go1["p1_name"], go1["p2_name"]
    assert said == [f"PICK_REFUSED {me} (launcher 0.1.0) vs {them} (launcher 0.1.0) sc5 match {match}: {reason}",
                    f"PICK_REFUSED {them} (launcher 0.1.0) vs {me} (launcher 0.1.0) sc5 match {match}: player 1's "
                    "created character from the other PC was refused: its checksum is wrong"]


async def test_a_refused_pick_outside_a_match_and_bad_ones(server, connect, caplog):
    c = await connect("Raiden")
    with caplog.at_level("INFO", logger="sc5.server"):
        await c.request({"type": "pick_refused", "game": "sc5", "match": "local-1", "reason": "Head: waiting"})
    assert any(r.getMessage() == "PICK_REFUSED Raiden (launcher 0.1.0) vs ? (launcher ?) sc5 match local-1: Head: waiting"
               for r in caplog.records)
    for bad in ({"reason": ""}, {"reason": "tab\there"}, {"reason": "x" * 201}, {"reason": 5}, {"game": "mk9x"},
                {"match": "m\x01"}):
        msg = {"type": "pick_refused", "game": "sc5", "match": "m", "reason": "r", **bad}
        with pytest.raises(ServerError) as exc:
            await c.request(msg)
        assert exc.value.code == "bad_request"


async def test_refused_picks_stop_being_logged_after_the_session_limit(server, connect, caplog):
    from server.app import PICK_REFUSALS_PER_SESSION

    c = await connect("Kitana")
    with caplog.at_level("INFO", logger="sc5.server"):
        for n in range(PICK_REFUSALS_PER_SESSION + 5):
            c.send({"type": "pick_refused", "game": "sc5", "match": "m", "reason": f"pick {n}"})
        await c.request({"type": "pick_refused", "game": "sc5", "match": "m", "reason": "the last"})
    said = [r.getMessage() for r in caplog.records if r.getMessage().startswith("PICK_REFUSED Kitana")]
    assert len(said) == PICK_REFUSALS_PER_SESSION
