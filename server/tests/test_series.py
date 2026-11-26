"""Series after a queue match: casual rematches while both want one (back to character select, both picking again);
ranked sets to three match wins, each match's winner keeping their fighter while the loser picks again, Elo moving once
per set, and a player who leaves a set with a match counted loses it."""

from __future__ import annotations

import time

import pytest

from server.client import ServerError

from .conftest import played, queue_msg, run_to_go


async def first_match(server, connect, udp_peers, names, *, ranked, character="Scorpion", game="sc5", mode="versus"):
    conns = [await connect(n) for n in names]
    udps = [await udp_peers(), await udp_peers()]
    for conn in conns:
        await conn.request(queue_msg(game, mode, ranked=ranked, character=character))
    gos = await run_to_go(server, list(zip(conns, udps)))
    sides = {go["side"]: (conn, udp) for conn, udp, go in zip(conns, udps, gos)}
    return sides, {go["side"]: go for go in gos}


async def play(c1, c2, match, winner, fighters=(None, None)):
    """Both players report the same result (each with the fighters its game saw, by player); returns the series
    message each gets after it."""
    p1_rounds, p2_rounds = (2, 0) if winner == 1 else (0, 2)
    for c, seen in zip((c1, c2), fighters):
        report = {"type": "result", "match": match, "winner": winner, "p1_rounds": p1_rounds, "p2_rounds": p2_rounds}
        if seen is not None:
            report.update(p1_char=seen[0], p2_char=seen[1])
        await c.request(report)
    for c in (c1, c2):
        assert (await c.expect("match_result", match=match))["status"] == "agreed"
    return await c1.expect("series"), await c2.expect("series")


async def draw(c1, c2, match, fighters=(None, None)):
    """Both games saw a draw (winner 0); returns the series message each gets after it."""
    for c, seen in zip((c1, c2), fighters):
        report = {"type": "result", "match": match, "winner": 0, "p1_rounds": 1, "p2_rounds": 1}
        if seen is not None:
            report.update(p1_char=seen[0], p2_char=seen[1])
        await c.request(report)
    for c in (c1, c2):
        assert (await c.expect("match_result", match=match))["status"] == "draw"
    return await c1.expect("series"), await c2.expect("series")


async def next_match(server, sides, series_id, choices=("select", "select")):
    """Both choose the next match (CHARACTER SELECT, the loser's usual choice on the game's result screen, unless
    others are given, by side; None: the message without a choice, which is a rematch); returns both match_go by
    side."""
    (c1, u1), (c2, u2) = sides[1], sides[2]
    for c, choice in zip((c1, c2), choices):
        msg = {"type": "series_ready", "series": series_id, "ready": True}
        if choice is not None:
            msg["choice"] = choice
        assert (await c.request(msg))["type"] == "series_ack"
    gos = await run_to_go(server, [(c1, u1), (c2, u2)])
    for c in (c1, c2):
        c.take_all("series")
    return {go["side"]: go for go in gos}


async def ranked_stats(conn, name):
    prof = await conn.request({"type": "profile", "name": name})
    return prof["stats"]["sc5"]["versus"]["ranked"]


async def test_ranked_set_ends_at_three_wins_with_one_elo_change(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Shujinko", "Onaga"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, s2 = await play(c1, c2, gos[1]["match"], 1)
    assert s1["state"] == "between" and s1["ranked"] and s1["need"] == 3 and s1["opponent"] == p2
    assert (s1["you"], s1["them"], s2["you"], s2["them"]) == (1, 0, 0, 1)
    assert s1["opponent_elo"] == 475 and s1["opponent_title"] == "Novice"
    # the match counts in the set; the record (sets won and lost, 1.0.1) and the rating wait for its end
    start = {"elo": 475, "title": "Novice", "wins": 0, "losses": 0}
    assert await ranked_stats(c1, p1) == start and await ranked_stats(c1, p2) == start

    gos2 = await next_match(server, sides, s1["series"])
    assert gos2[1]["match"] != gos[1]["match"] and gos2[1]["p1_name"] == p1  # same sides
    s1, _ = await play(c1, c2, gos2[1]["match"], 1)
    # two wins took a set before 0.1.21: now it goes on to the third
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (2, 0)
    assert await ranked_stats(c1, p1) == start
    gos3 = await next_match(server, sides, s1["series"])
    over1, over2 = await play(c1, c2, gos3[1]["match"], 1)
    assert over1["state"] == "over" and over1["outcome"] == "won" and over1["winner"] == p1
    assert (over1["you"], over1["them"]) == (3, 0)
    assert over1["elo"][p1] == {"before": 475, "after": 507, "delta": 32,
                                "title_before": "Novice", "title_after": "Novice"}
    assert over2["elo"][p2] == {"before": 475, "after": 459, "delta": -16,
                                "title_before": "Novice", "title_after": "Novice"}
    # one set: one win and one loss, whatever its score
    assert await ranked_stats(c1, p1) == {"elo": 507, "title": "Novice", "wins": 1, "losses": 0}
    assert await ranked_stats(c1, p2) == {"elo": 459, "title": "Novice", "wins": 0, "losses": 1}
    board = await c2.request({"type": "leaderboard", "game": "sc5", "mode": "versus"})
    assert [(r["name"], r["wins"], r["losses"]) for r in board["rows"]] == [(p1, 1, 0), (p2, 0, 1)]
    row = server.db.set_row(over1["series"])
    assert row["outcome"] == "won" and (row["p1_wins"], row["p2_wins"]) == (3, 0)
    assert (row["p1_elo_after"], row["p2_elo_after"]) == (507, 459)
    assert not server.series and not server.series_of


async def test_ranked_set_goes_to_a_fifth_match_at_two_all(server, connect, udp_peers, caplog):
    sides, gos = await first_match(server, connect, udp_peers, ["Kitana", "Mileena"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    match = gos[1]["match"]
    for n, winner in enumerate((1, 2, 1, 2), start=1):
        s1, _ = await play(c1, c2, match, winner)
        assert s1["state"] == "between" and s1["played"] == n
        match = (await next_match(server, sides, s1["series"]))[1]["match"]
    assert (s1["you"], s1["them"]) == (2, 2)
    with caplog.at_level("INFO", logger="sc5.server"):
        _, over2 = await play(c1, c2, match, 2)
    assert over2["state"] == "over" and over2["outcome"] == "won" and over2["winner"] == p2 and over2["played"] == 5
    assert (over2["you"], over2["them"]) == (3, 2)
    # the server's log says the set's score and both new ratings with their titles
    assert any(f"over: won, {p1} 2-3 {p2}, {p2} wins it, {p1} 475 -> 459 Novice, {p2} 475 -> 507 Novice"
               in r.getMessage() for r in caplog.records)
    # two matches won each way, but one set lost and one won
    assert await ranked_stats(c1, p1) == {"elo": 459, "title": "Novice", "wins": 0, "losses": 1}
    assert await ranked_stats(c1, p2) == {"elo": 507, "title": "Novice", "wins": 1, "losses": 0}


async def test_one_match_won_and_gone_loses_the_set(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kenshi", "Mavado"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 1)  # P1 wins the first match...
    left = await c1.request({"type": "series_ready", "series": s1["series"], "ready": False})  # ...and leaves
    assert left["type"] == "series_left"
    over = await c2.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p1 and over["winner"] == p2
    assert over["elo"][p1]["delta"] == -16 and over["elo"][p2]["delta"] == 32
    # the match P1 won does not count on its own: the set it left is a loss
    assert await ranked_stats(c2, p1) == {"elo": 459, "title": "Novice", "wins": 0, "losses": 1}
    assert await ranked_stats(c2, p2) == {"elo": 507, "title": "Novice", "wins": 1, "losses": 0}


async def test_searching_again_in_a_set_loses_it(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Tanya", "Hotaru"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    await play(c1, c2, gos[1]["match"], 1)
    assert (await c1.request(queue_msg(ranked=True)))["type"] == "queued"
    over = await c2.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p1


async def test_a_set_waits_for_the_next_match_then_the_absent_one_loses(server, connect, udp_peers):
    server.cfg.set_next_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["Ashrah", "Havik"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 2)
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": True})
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1


async def test_leaving_before_anything_counted_changes_no_rating(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kabal", "Mileena"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    match = gos[1]["match"]
    await c1.request({"type": "result", "match": match, "outcome": "desync", "frame": 30})
    await c2.request({"type": "result", "match": match, "outcome": "desync", "frame": 30})
    s1 = await c1.expect("series")
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (0, 0)
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": False})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "void" and over["winner"] is None and "elo" not in over
    assert server.db.rating(server.db.account_by_name("Kabal")["id"], "sc5", "versus")["elo"] == 475


async def test_casual_rematch_while_both_want_one(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kobra", "Kira"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, s2 = await play(c1, c2, gos[1]["match"], 2)
    assert s1["state"] == "between" and not s1["ranked"] and s1["need"] == 0
    assert (s1["you"], s1["them"], s2["you"], s2["them"]) == (0, 1, 1, 0)

    # the first to choose a rematch is shown waiting to the other
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": True})
    waiting = await c1.expect("series", state="between", them_ready=True)
    assert waiting["you_ready"] is False
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": True})
    gos2 = await run_to_go(server, [sides[1], sides[2]])
    gos2 = {go["side"]: go for go in gos2}
    assert gos2[1]["p1_name"] == p1 and gos2[2]["p2_name"] == p2
    for c in (c1, c2):
        c.take_all("series")

    s1, _ = await play(c1, c2, gos2[1]["match"], 1)
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (1, 1) and s1["played"] == 2
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": False})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "left" and over["left"] == p2 and "elo" not in over
    prof = await c1.request({"type": "profile", "name": p1})
    assert played(prof["stats"]) == {"sc5": {"versus": {"casual": {"wins": 1, "losses": 1}}}}


async def test_versus_rematch_keeps_the_fighters_and_select_picks_again(server, connect, udp_peers):
    # queued with fighters from the launcher: the first match plays them; both choosing a rematch plays the fighters
    # both games saw again (the game's own Rematch), either one choosing character select sends both back there
    sides, gos = await first_match(server, connect, udp_peers, ["Raiden", "Jade"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    assert (gos[1]["p1"], gos[1]["p2"]) == ("Scorpion", "Scorpion")
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("12", "7"), ("12", "7")))
    assert s1["same_fighters"] is True and s1["you_choice"] == "" and s1["them_choice"] == ""
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("12", "7") and (gos2[2]["p1"], gos2[2]["p2"]) == ("12", "7")
    s1, _ = await play(c1, c2, gos2[1]["match"], 2, fighters=(("3", "4"), ("3", "4")))
    # one wants character select: the other sees it, and the loser picks again while the winner (player 2, with
    # fighter 4) keeps theirs (SCV 0.1.8)
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": True, "choice": "select"})
    waiting = await c1.expect("series", state="between", them_ready=True)
    assert waiting["them_choice"] == "select" and waiting["you_choice"] == ""
    gos3 = await next_match(server, sides, s1["series"], choices=("rematch", None))
    assert (gos3[1]["p1"], gos3[1]["p2"]) == ("-", "4") and gos3[1]["keep"] == 2 and gos3[2]["keep"] == 2
    # the games did not agree on the fighters: a rematch picks again too
    s1, _ = await play(c1, c2, gos3[1]["match"], 1, fighters=(("3", "4"), ("3", "5")))
    assert s1["same_fighters"] is False
    gos4 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos4[1]["p1"], gos4[1]["p2"]) == ("-", "-")


async def test_the_next_match_from_the_result_screens_needs_no_punch(server, connect, udp_peers):
    # both launchers ready with their games still at the result screen (in_place), the last match straight between
    # them: match_found says so and match_go follows at once, on the last match's route, with a new id and seed
    sides, gos = await first_match(server, connect, udp_peers, ["Kitana", "Mileena"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("12", "7"), ("12", "7")))
    for c in (c1, c2):
        msg = {"type": "series_ready", "series": s1["series"], "ready": True, "choice": "rematch", "in_place": True}
        assert (await c.request(msg))["type"] == "series_ack"
    for c, side in ((c1, 1), (c2, 2)):
        found = await c.expect("match_found")
        assert found["in_place"] is True and found["side"] == side
        go = await c.expect("match_go", match=found["match"])
        assert go["match"] != gos[1]["match"] and go["seed"] != gos[1]["seed"]
        assert (go["p1"], go["p2"]) == ("12", "7") and go["remote"] == gos[side]["remote"]
    # one of them in place only: the usual way (UDP, punch) for both
    s1, _ = await play(c1, c2, go["match"], 2, fighters=(("12", "7"), ("12", "7")))
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": True, "in_place": True})
    await c2.request({"type": "series_ready", "series": s1["series"], "ready": True})
    for c in (c1, c2):
        assert (await c.expect("match_found"))["in_place"] is False


async def test_ranked_winner_keeps_their_fighter_and_the_loser_picks_again(server, connect, udp_peers):
    # queued with fighters from the launcher (or "" to pick in the game): the first match plays what each queued with;
    # after it, the last match's winner keeps the fighter they won with (the autopilot picks it in both games) and only
    # the loser goes back to the select screen, whatever each chose
    sides, gos = await first_match(server, connect, udp_peers, ["Dairou", "Darrius"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    assert (gos[1]["p1"], gos[1]["p2"]) == ("Scorpion", "Scorpion")
    s1, s2 = await play(c1, c2, gos[1]["match"], 1, fighters=(("3", "4"), ("3", "4")))
    assert s1["keeps"] == p1 and s2["keeps"] == p1  # both are told who keeps their fighter
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "select"))
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("3", "-") and (gos2[2]["p1"], gos2[2]["p2"]) == ("3", "-")

    # P2 wins the next one with a fighter picked again: now P1 picks, P2 keeps theirs
    s1, _ = await play(c1, c2, gos2[1]["match"], 2, fighters=(("3", "9"), ("3", "9")))
    assert s1["keeps"] == p2 and (s1["you"], s1["them"]) == (1, 1)
    gos3 = await next_match(server, sides, s1["series"])
    assert (gos3[1]["p1"], gos3[1]["p2"]) == ("-", "9") and (gos3[2]["p1"], gos3[2]["p2"]) == ("-", "9")

    # a match that does not count (the games parted) is played again the same way
    for c in (c1, c2):
        await c.request({"type": "result", "match": gos3[1]["match"], "outcome": "desync", "frame": 40})
    s1 = await c1.expect("series", state="between")
    c2.take_all("series")
    assert s1["keeps"] == p2 and (s1["you"], s1["them"]) == (1, 1)
    gos4 = await next_match(server, sides, s1["series"])
    assert (gos4[1]["p1"], gos4[1]["p2"]) == ("-", "9")

    # the winner keeps what they won with, here the same fighter again
    s1, _ = await play(c1, c2, gos4[1]["match"], 2, fighters=(("12", "9"), ("12", "9")))
    assert s1["keeps"] == p2 and (s1["you"], s1["them"]) == (1, 2)
    gos5 = await next_match(server, sides, s1["series"])
    assert (gos5[1]["p1"], gos5[1]["p2"]) == ("-", "9")
    s1, _ = await play(c1, c2, gos5[1]["match"], 1, fighters=(("7", "9"), ("7", "9")))
    assert s1["keeps"] == p1 and (s1["you"], s1["them"]) == (2, 2)
    gos6 = await next_match(server, sides, s1["series"])
    assert (gos6[1]["p1"], gos6[1]["p2"]) == ("7", "-")
    over, _ = await play(c1, c2, gos6[1]["match"], 1, fighters=(("7", "2"), ("7", "2")))
    assert over["state"] == "over" and over["winner"] == p1 and (over["you"], over["them"]) == (3, 2)


async def test_ranked_winner_keeps_their_created_character(server, connect, udp_peers):
    # a created character is c.<its checksum> in the games' result lines (both games name it alike): the winner keeps
    # it as any fighter, their own game picking it again and the other following
    sides, gos = await first_match(server, connect, udp_peers, ["Dairou", "Darrius"], ranked=True, character="")
    (c1, _), (c2, _) = sides[1], sides[2]
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("c.9A142B82", "15.0"), ("c.9A142B82", "15.0")))
    assert s1["keeps"] == gos[1]["p1_name"]
    gos2 = await next_match(server, sides, s1["series"])
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("c.9A142B82", "-") and (gos2[2]["p1"], gos2[2]["p2"]) == ("c.9A142B82", "-")


async def test_a_draw_in_a_set_is_nobodys_point(server, connect, udp_peers):
    # a drawn match counts for neither player, and draws never end a set as matches that did not count do; player 1
    # chooses the next match on the result screen as a loser would (SCV 0.1.8), so through CHARACTER SELECT player 2
    # keeps the fighter of the draw and player 1 picks again
    sides, gos = await first_match(server, connect, udp_peers, ["Sophitia", "Cassandra"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("3", "4"), ("3", "4")))
    assert s1["keeps"] == p1
    match = (await next_match(server, sides, s1["series"]))[1]["match"]
    for n in range(server.cfg.series_max_no_contest + 1):
        s1, s2 = await draw(c1, c2, match, fighters=(("3", "21.1"), ("3", "21.1")))
        assert s1["state"] == "between" and (s1["you"], s1["them"], s2["you"], s2["them"]) == (1, 0, 0, 1)
        assert s1["keeps"] == gos[1]["p2_name"] and s1["played"] == n + 2
        gos2 = await next_match(server, sides, s1["series"])
        assert (gos2[1]["p1"], gos2[1]["p2"]) == ("-", "21.1") and gos2[1]["keep"] == 2
        match = gos2[1]["match"]
    s1, _ = await play(c1, c2, match, 1, fighters=(("3", "21.1"), ("3", "21.1")))
    assert s1["state"] == "between" and (s1["you"], s1["them"]) == (2, 0)


async def test_ranked_winner_whose_fighter_is_not_known_picks_again(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Reiko", "Rain"], ranked=True, character="")
    (c1, _), (c2, _) = sides[1], sides[2]
    assert (gos[1]["p1"], gos[1]["p2"]) == ("-", "-")  # both pick in the game
    # the two games named different fighters (or none): nothing to keep, both pick again
    s1, _ = await play(c1, c2, gos[1]["match"], 2, fighters=(("3", "4"), ("3", "5")))
    assert s1["keeps"] is None
    gos2 = await next_match(server, sides, s1["series"])
    assert (gos2[1]["p1"], gos2[1]["p2"]) == ("-", "-")
    s1, _ = await play(c1, c2, gos2[1]["match"], 2, fighters=(("3", "4"), ("3", "4")))
    assert s1["keeps"] == gos[1]["p2_name"]
    gos3 = await next_match(server, sides, s1["series"])
    assert (gos3[1]["p1"], gos3[1]["p2"]) == ("-", "4")
    # a later win without agreed fighters lets the winner pick again too
    s1, _ = await play(c1, c2, gos3[1]["match"], 1)
    assert s1["keeps"] is None
    gos4 = await next_match(server, sides, s1["series"])
    assert (gos4[1]["p1"], gos4[1]["p2"]) == ("-", "-")


async def test_bad_fighters_or_choice(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Nitara", "Noob"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    # a fighter id the server does not take never costs the result
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("bad id!", "4"), ("3", "4")))
    assert s1["same_fighters"] is False
    with pytest.raises(ServerError) as exc:
        await c1.request({"type": "series_ready", "series": s1["series"], "ready": True, "choice": "random"})
    assert exc.value.code == "bad_request"


async def test_casual_rematch_offer_runs_out(server, connect, udp_peers):
    server.cfg.rematch_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["BoRaiCho", "Shinnok"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    await play(c1, c2, gos[1]["match"], 1)
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "timeout" and over["winner"] is None
    # free to search again
    assert (await c1.request(queue_msg()))["type"] == "queued"


async def test_closing_the_game_mid_match_offers_the_casual_rematch_again(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["BoRai", "Kotal"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    match = gos[1]["match"]
    # P1's game closes during the match: the match ends for both at once and counts for nobody; casual rematches
    # have no end, so the rematch is offered again
    await c1.request({"type": "result", "match": match, "outcome": "left"})
    for c in (c1, c2):
        res = await c.expect("match_result", match=match)
        assert res["status"] == "abandoned" and res["left"] == p1 and res["left_side"] == 1
        between = await c.expect("series", state="between")
        assert between["you"] == 0 and between["them"] == 0 and between["played"] == 1
    # the other game's late report changes nothing, and both can play on
    ack = await c2.request({"type": "result", "match": match, "outcome": "disconnected"})
    assert ack["type"] == "result_ack"
    gos = await next_match(server, sides, between["series"])
    assert gos[1]["match"] != match


async def test_a_game_that_left_says_how_it_ended(server, connect, udp_peers, caplog):
    """A launcher whose game ended mid-match says how (from the game's log): the server's log keeps it with the match,
    and names who left the series; junk in those fields is dropped, never refused."""
    sides, gos = await first_match(server, connect, udp_peers, ["Jarek", "Tanya"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1 = gos[1]["p1_name"]
    with caplog.at_level("INFO", logger="sc5.server"):
        await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "left",
                          "exit": "asked to leave 2", "exit_code": 0})
        between = await c2.expect("series", state="between")
        assert (await c1.request({"type": "series_ready", "series": between["series"], "ready": False}))["type"] \
            == "series_left"
        await c2.expect("series", state="over")
    abandoned = next(r.getMessage() for r in caplog.records if "ABANDONED" in r.getMessage())
    assert "'exit': 'asked to leave 2'" in abandoned and "'exit_code': 0" in abandoned
    assert any(f"over: left after 1 matches ({p1} left)" in r.getMessage() for r in caplog.records)

    sides, gos = await first_match(server, connect, udp_peers, ["Kenshi", "Hsu"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    caplog.clear()
    with caplog.at_level("INFO", logger="sc5.server"):
        ack = await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "left",
                                "exit": "x" * 41, "exit_code": "0"})
        assert ack["type"] == "result_ack"
        await c2.expect("series", state="between")
    abandoned = next(r.getMessage() for r in caplog.records if "ABANDONED" in r.getMessage())
    assert "'exit'" not in abandoned and "'exit_code'" not in abandoned


async def test_closing_the_game_mid_match_loses_a_ranked_set_even_at_nil(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Kai", "Sareena"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    c2.send({"type": "match_state", "match": gos[1]["match"], "state": "in_match"})  # P2's game took the start
    await c2.request({"type": "result", "match": gos[1]["match"], "outcome": "left"})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1
    assert over["elo"][p1]["delta"] == 32 and over["elo"][p2]["delta"] == -16


async def test_a_game_that_never_started_a_sets_first_match_changes_no_rating(server, connect, udp_peers, caplog):
    """A set's first match whose leaver's own game never took the start (it failed to come up or closed while
    loading; the other's game may have): nothing of the set was played, so the set is void, as a declined match."""
    sides, gos = await first_match(server, connect, udp_peers, ["Kitana", "Jade"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p2 = gos[1]["p2_name"]
    c1.send({"type": "match_state", "match": gos[1]["match"], "state": "in_match"})  # only P1's game started
    with caplog.at_level("INFO", logger="sc5.server"):
        await c2.request({"type": "result", "match": gos[1]["match"], "outcome": "left",
                          "exit": "window closed", "exit_code": 0})
        over = await c1.expect("series", state="over")
    assert over["outcome"] == "void" and over["winner"] is None and "elo" not in over
    assert (await c2.expect("series", state="over"))["outcome"] == "void"
    for name in ("Kitana", "Jade"):
        assert server.db.rating(server.db.account_by_name(name)["id"], "sc5", "versus")["elo"] == 475
    assert any(f"{p2}'s game never started its first match: void" in r.getMessage() for r in caplog.records)


async def test_a_game_that_ended_before_its_match_sends_its_last_log_lines(server, connect, udp_peers, caplog):
    """Its launcher sends the game log's last lines with the left report: the server's log has them; junk lines are
    dropped, never refused."""
    sides, gos = await first_match(server, connect, udp_peers, ["Ashrah", "Havik"], ranked=True)
    (_, _), (c2, _) = sides[1], sides[2]
    with caplog.at_level("INFO", logger="sc5.server"):
        ack = await c2.request({"type": "result", "match": gos[2]["match"], "outcome": "left", "exit": "window closed",
                                "exit_code": 0, "lines": ["[   21.5] link: status booting", "[   48.2] Exit (0)"]})
        assert ack["type"] == "result_ack"
    said = [r.getMessage() for r in caplog.records if "LEFT_REPORT" in r.getMessage()]
    assert "its game ended before the match started, 2 lines" in said[0] and said[2].endswith("| [   48.2] Exit (0)")

    sides, gos = await first_match(server, connect, udp_peers, ["Dairou", "Darrius"], ranked=True)
    (_, _), (c2, _) = sides[1], sides[2]
    caplog.clear()
    with caplog.at_level("INFO", logger="sc5.server"):
        ack = await c2.request({"type": "result", "match": gos[2]["match"], "outcome": "left",
                                "lines": ["ok", "x" * 201]})
        assert ack["type"] == "result_ack"
    assert not any("LEFT_REPORT" in r.getMessage() for r in caplog.records)


async def test_leaving_a_later_match_of_a_set_before_its_start_still_loses_it(server, connect, udp_peers):
    """Only a set's first match: once a match of it was played, a game closed before the next one starts loses the
    set, so closing it never saves a losing set."""
    sides, gos = await first_match(server, connect, udp_peers, ["Rain", "Tremor"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 1)  # P1 leads 1-0
    gos = await next_match(server, sides, s1["series"])
    await c2.request({"type": "result", "match": gos[2]["match"], "outcome": "left"})
    over = await c1.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1


async def test_a_player_who_vanishes_mid_match_loses_the_set(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Frost", "Sektor"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 2)  # P2 leads 1-0
    gos = await next_match(server, sides, s1["series"])
    await c2.close()  # P2's launcher goes away during the next match
    await c1.request({"type": "result", "match": gos[1]["match"], "outcome": "disconnected"})
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "forfeit" and over["left"] == p2 and over["winner"] == p1


async def test_casual_rematches_go_on_through_matches_that_do_not_count(server, connect, udp_peers):
    """Games that lose each other end the match without a result; a casual series offers the rematch again however
    often that happens (a ranked set would end without a rating change after series_max_no_contest of them)."""
    sides, gos = await first_match(server, connect, udp_peers, ["Jax", "Sonya"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    for _ in range(server.cfg.series_max_no_contest + 1):
        for c in (c1, c2):
            await c.request({"type": "result", "match": gos[1]["match"], "outcome": "disconnected"})
        res = await c1.expect("match_result", match=gos[1]["match"])
        assert res["status"] == "no_contest" and res["left"] is None
        between = await c1.expect("series", state="between")
        c2.take_all("series")
        gos = await next_match(server, sides, between["series"])
    assert between["played"] == server.cfg.series_max_no_contest + 1


async def test_the_losers_choice_in_a_ranked_set(server, connect, udp_peers):
    """SCV 0.1.8: the loser of each match chooses on the game's result screen, and both launchers send that choice.
    match_go says what the game shows: the run's kind, a set's wins to take it and each side's wins before the match,
    and the side whose fighter is kept. REMATCH plays the same two fighters again in a ranked set too; CHARACTER
    SELECT keeps the winner's; LEAVE (the loser's launcher: series_ready false) loses the set."""
    sides, gos = await first_match(server, connect, udp_peers, ["Hilde", "Natsu"], ranked=True, character="")
    (c1, _), (c2, _) = sides[1], sides[2]
    for side in (1, 2):
        assert (gos[side]["series_kind"], gos[side]["set"], gos[side]["set_p1"], gos[side]["set_p2"],
                gos[side]["keep"]) == ("ranked", 3, 0, 0, 0)
    s1, _ = await play(c1, c2, gos[1]["match"], 1, fighters=(("3", "4"), ("3", "4")))
    gos2 = await next_match(server, sides, s1["series"], choices=("rematch", "rematch"))
    assert (gos2[1]["p1"], gos2[1]["p2"], gos2[1]["keep"]) == ("3", "4", 0)
    assert (gos2[1]["set_p1"], gos2[1]["set_p2"]) == (1, 0)
    s1, _ = await play(c1, c2, gos2[1]["match"], 1, fighters=(("3", "4"), ("3", "4")))
    gos3 = await next_match(server, sides, s1["series"])
    assert (gos3[2]["p1"], gos3[2]["p2"], gos3[2]["keep"]) == ("3", "-", 1)
    assert (gos3[2]["set_p1"], gos3[2]["set_p2"]) == (2, 0)
    s1, s2 = await play(c1, c2, gos3[1]["match"], 2, fighters=(("3", "7"), ("3", "7")))
    # player 1 lost and chose LEAVE: their launcher leaves the set, which they lose 1-2 whatever its score
    await c1.request({"type": "series_ready", "series": s1["series"], "ready": False})
    over = await c2.expect("series", state="over")
    assert over["outcome"] == "forfeit" and over["winner"] == gos[1]["p2_name"] and over["left"] == gos[1]["p1_name"]


async def test_the_queue_match_offers_the_losers_choice(server, connect, udp_peers):
    sides, gos = await first_match(server, connect, udp_peers, ["Leixia", "Xiba"], ranked=False, character="")
    assert gos[1]["series_kind"] == "casual" and gos[1]["set"] == 0
    (c1, _), (c2, _) = sides[1], sides[2]
    s1, _ = await play(c1, c2, gos[1]["match"], 2, fighters=(("21.0", "22.1"), ("21.0", "22.1")))
    assert s1["keeps"] == gos[1]["p2_name"]  # casual too: the winner keeps their fighter through CHARACTER SELECT
    gos2 = await next_match(server, sides, s1["series"])
    assert (gos2[1]["p1"], gos2[1]["p2"], gos2[1]["keep"]) == ("-", "22.1", 2)
    assert (gos2[1]["set_p1"], gos2[1]["set_p2"]) == (0, 1)


async def test_a_drop_at_the_result_screen_keeps_the_choice_open(server, connect, udp_peers):
    """A launcher that drops between two matches, while the loser still chooses on the game's result screen: the time
    left is never cut, and is at least set_rejoin_grace from the drop; back and ready in time, the set goes on."""
    assert server.cfg.set_rejoin_grace >= 75  # the choice comes up to 52 s after the result
    sides, gos = await first_match(server, connect, udp_peers, ["Pyrrha", "Patroklos"], ranked=True)
    (c1, u1), (c2, u2) = sides[1], sides[2]
    p2 = gos[1]["p2_name"]
    s1, _ = await play(c1, c2, gos[1]["match"], 1)
    series = server.series[s1["series"]]
    usual = series.deadline
    token = c2.account["token"]
    await c2.close()  # the loser's launcher drops at the result screen
    left = await c1.expect("series", state="between", timeout=3)
    assert series.deadline >= usual and left["seconds_left"] >= 89  # not cut: 90 s left as it was
    # a later drop with less of the usual wait left gets the grace from the drop
    series.deadline = series.between_since + 10
    back = await connect()
    await back.request({"type": "auth", "token": token})
    again = await back.expect("series", state="between")  # the state again, as after any login
    assert again["series"] == s1["series"] and again["you_ready"] is False
    await back.close()
    await c1.expect("series", state="between", timeout=3)
    assert series.deadline >= time.time() + server.cfg.set_rejoin_grace - 2
    # dropping again and again holds nobody up for long: never past the usual wait and one grace
    assert series.deadline <= series.between_since + server.cfg.set_next_timeout + server.cfg.set_rejoin_grace + 0.1
    c2 = await connect()
    c2.account = await c2.request({"type": "auth", "token": token})
    await c2.expect("series", state="between")
    sides[2] = (c2, u2)
    gos2 = await next_match(server, sides, s1["series"])
    assert gos2[2]["p2_name"] == p2 and (gos2[1]["set_p1"], gos2[1]["set_p2"]) == (1, 0)


async def test_nobody_ready_for_a_sets_next_match_loses_it_for_the_loser(server, connect, udp_peers):
    """Neither player readies in time (the loser's launcher closed or killed at the result screen, nothing sent): the
    next match was the loser's choice, so the set is theirs lost, as LEAVE would be."""
    server.cfg.set_next_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["Viola", "ZWEI"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    p1, p2 = gos[1]["p1_name"], gos[1]["p2_name"]
    await play(c1, c2, gos[1]["match"], 2)  # player 1 lost
    over = await c2.expect("series", state="over", timeout=3)
    assert over["outcome"] == "forfeit" and over["left"] == p1 and over["winner"] == p2
    assert over["elo"][p2]["delta"] > 0


async def test_nobody_ready_after_a_draw_or_before_anything_counted_ends_without_a_result(server, connect, udp_peers):
    server.cfg.set_next_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["Aeon", "Dampierre"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    server.cfg.set_next_timeout = 30
    s1, _ = await play(c1, c2, gos[1]["match"], 1)
    gos2 = await next_match(server, sides, s1["series"])
    server.cfg.set_next_timeout = 0.5
    await draw(c1, c2, gos2[1]["match"])  # the last match a draw: nobody's choice to lose by
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "timeout" and over["winner"] is None and "elo" not in over

    sides, gos = await first_match(server, connect, udp_peers, ["Elysium", "Kilik"], ranked=True)
    (c1, _), (c2, _) = sides[1], sides[2]
    for c in (c1, c2):  # nothing counted yet (a desync)
        await c.request({"type": "result", "match": gos[1]["match"], "outcome": "desync", "frame": 30})
    over = await c1.expect("series", state="over", timeout=3)
    assert over["outcome"] == "timeout" and over["winner"] is None


async def test_casual_with_nobody_ready_just_ends(server, connect, udp_peers):
    server.cfg.rematch_timeout = 0.5
    sides, gos = await first_match(server, connect, udp_peers, ["Mitsurugi", "Natsu"], ranked=False)
    (c1, _), (c2, _) = sides[1], sides[2]
    await play(c1, c2, gos[1]["match"], 2)
    over = await c2.expect("series", state="over", timeout=3)
    assert over["outcome"] == "timeout" and over["left"] is None
