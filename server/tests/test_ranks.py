"""Ranked titles by Elo (16 titles, each with its badge): the ladder, and the title the server sends with every
rating, ranked or not (lobby members, a match's players)."""

from __future__ import annotations

import asyncio

from server import elo
from server import protocol as P
from server.__main__ import (RECOUNT_RECORDS_FLAG, RESET_RANKS_FLAG, RESTORE_RECORDS_FLAG, recount_ranked_records_if_asked,
                             reset_ranks_if_asked, restore_ranked_records_if_asked)
from server.db import Database

from .conftest import hello_and_accept, queue_msg
from .test_lobby import create_msg
from .test_series import play

LADDER = [
    ("Novice", None), ("Apprentice", 575), ("Squire", 675), ("Wanderer", 775), ("Duelist", 875), ("Knight", 975),
    ("Vanguard", 1075), ("Blademaster", 1175), ("Champion", 1275), ("Conqueror", 1375), ("Hero", 1475),
    ("Legend", 1575), ("Soul Warrior", 1675), ("Soul Master", 1795), ("Spirit Sword", 1915), ("Eternal Soul", 2035),
]


def test_titles_by_elo():
    assert [(r["title"], r["from"]) for r in elo.ladder()] == LADDER
    assert elo.title(elo.INITIAL) == "Novice"  # where a new player starts
    for (name, floor), (_, upper) in zip(LADDER, LADDER[1:] + [(None, None)]):
        if floor is not None:
            assert elo.title(floor) == name
        if upper is not None:
            assert elo.title(upper - 1) == name
    assert elo.title(-40) == "Novice" and elo.title(30000) == "Eternal Soul"


def set_rating(server, name, rating, game="sc5", mode="versus"):
    account = server.db.account_by_name(name)["id"]
    server.db.conn.execute(
        "INSERT OR REPLACE INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
        "VALUES (?, ?, ?, 'ranked', ?, 3, 2, 0)", (account, game, mode, rating))


async def test_every_ranked_rating_comes_with_its_title(server, connect, udp_peers):
    conns = [await connect(n) for n in ("Fujin", "Raijin", "Shinnok")]
    assert conns[0].welcome["ranks"] == elo.ladder()  # the ladder, for the launcher to show
    for name, rating in (("Fujin", 1840), ("Raijin", 1780), ("Shinnok", 500)):
        set_rating(server, name, rating)

    prof = await conns[0].request({"type": "profile"})
    assert prof["stats"]["sc5"]["versus"]["ranked"] == {"elo": 1840, "title": "Soul Master", "wins": 3, "losses": 2}
    board = await conns[2].request({"type": "leaderboard", "game": "sc5", "mode": "versus"})
    assert [(r["name"], r["elo"], r["title"]) for r in board["rows"]] == [
        ("Fujin", 1840, "Soul Master"), ("Raijin", 1780, "Soul Warrior"), ("Shinnok", 500, "Novice")]
    assert board["me"] == {"rank": 3, "elo": 500, "title": "Novice", "wins": 3, "losses": 2, "respect": 0}

    players = conns[:2]
    udps = [await udp_peers(), await udp_peers()]
    queued = [await c.request(queue_msg(ranked=True)) for c in players]
    assert [(q["elo"], q["title"]) for q in queued] == [(1840, "Soul Master"), (1780, "Soul Warrior")]
    # the opponent stays hidden until both accept, then comes with their rating and its title
    founds = [await c.expect("match_found") for c in players]
    assert all(f["opponent"] is None for f in founds)
    for c, udp, found in zip(players, udps, founds):
        await hello_and_accept(server, c, udp, found)
    peers = [await c.expect("match_peer") for c in players]
    assert (peers[0]["opponent"]["elo"], peers[0]["opponent"]["title"]) == (1780, "Soul Warrior")
    assert (peers[1]["opponent"]["elo"], peers[1]["opponent"]["title"]) == (1840, "Soul Master")
    reports = await asyncio.gather(*[
        udp.punch(peer["peer"], match=peer["match"], key=peer["key"], side=peer["side"], timeout=5)
        for udp, peer in zip(udps, peers)])
    for c, peer, report in zip(players, peers, reports):
        c.send({"type": "ping_report", "match": peer["match"], **report})
    gos = [await c.expect("match_go") for c in players]

    # a set's state names the opponent's title; its end, each player's title before and after
    by_side = {go["side"]: c for c, go in zip(players, gos)}
    s1, s2 = await play(by_side[1], by_side[2], gos[0]["match"], 1)
    assert {s["opponent"]: s["opponent_title"] for s in (s1, s2)} == {"Raijin": "Soul Warrior", "Fujin": "Soul Master"}
    left = await players[0].request({"type": "series_ready", "series": s1["series"], "ready": False})
    assert left["type"] == "series_left"
    over = await players[1].expect("series", state="over")
    assert over["winner"] == "Raijin"
    fujin = over["elo"]["Fujin"]
    assert (fujin["title_before"], fujin["title_after"]) == ("Soul Master", "Soul Master")
    # up a rank, Soul Warrior to Soul Master (a 120 Elo step), from beating a Soul Master who left the set
    # (a set won gains twice the Elo change: 2 x 19)
    assert over["elo"]["Raijin"] == {"before": 1780, "after": 1818, "delta": 38, "title_before": "Soul Warrior",
                                     "title_after": "Soul Master"}


async def test_lobby_members_come_with_their_titles_in_its_game_and_mode(server, connect):
    host, guest = await connect("Kitana"), await connect("Jade")
    set_rating(server, "Kitana", 1700)  # a Soul Warrior at versus
    created = await host.request(create_msg())
    code = created["lobby"]["id"]
    assert [(m["name"], m["title"]) for m in created["lobby"]["members"]] == [("Kitana", "Soul Warrior")]
    # Jade never played ranked: the title everyone starts with
    joined = await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    assert [(m["name"], m["title"]) for m in joined["lobby"]["members"]] == [("Kitana", "Soul Warrior"),
                                                                             ("Jade", "Novice")]


async def test_a_new_player_starts_a_novice_and_a_record_from_the_old_start_stays(server, connect):
    c = await connect("Goro")
    assert elo.INITIAL == 475
    start = {"elo": 475, "title": "Novice", "wins": 0, "losses": 0}
    prof = await c.request({"type": "profile"})
    assert all(prof["stats"][game][mode]["ranked"] == start for game, modes in P.GAMES.items() for mode in modes
               if mode not in P.LOBBY_ONLY_MODES)  # Online Practice keeps no record
    set_rating(server, "Goro", 1516)  # played ranked while everyone started at 1500
    prof = await c.request({"type": "profile"})
    assert prof["stats"]["sc5"]["versus"]["ranked"] == {"elo": 1516, "title": "Hero", "wins": 3, "losses": 2}
    assert "sparring" not in prof["stats"]["sc5"]  # Online Practice has no record at all


EVERY_RECORD = [("Alice", "sc5", "versus", "casual", 1500), ("Alice", "sc5", "versus", "ranked", 1700),
                ("Bob", "sc5", "versus", "ranked", 1640), ("Carol", "sc5", "versus", "ranked", 1300)]


def database_with_records(tmp_path):
    """EVERY_RECORD, each 5-4, in a database file; Dave has an account and no record. Returns the file and the
    accounts by name."""
    path = str(tmp_path / "server.sqlite")
    db = Database(path)
    accounts = {name: db.create_account(name, "x") for name in ("Alice", "Bob", "Carol", "Dave")}
    for name, game, mode, kind, rating in EVERY_RECORD:
        db.conn.execute("INSERT INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
                        "VALUES (?, ?, ?, ?, ?, 5, 4, 0)", (accounts[name], game, mode, kind, rating))
    db.close()
    return path, accounts


def records(path):
    db = Database(path)
    rows = [tuple(r) for r in db.conn.execute(
        "SELECT a.name, s.game, s.mode, s.kind, s.elo FROM stats s JOIN accounts a ON a.id = s.account_id "
        "ORDER BY a.name, s.game, s.mode, s.kind")]
    db.close()
    return rows


def test_a_rank_reset_happens_once_when_asked_and_keeps_a_copy(tmp_path):
    path, accounts = database_with_records(tmp_path)
    flag = tmp_path / RESET_RANKS_FLAG

    def copies():
        return [c for c in tmp_path.glob("server.sqlite.before-rank-reset-*") if not c.name.endswith(("-wal", "-shm"))]

    reset_ranks_if_asked(path)  # not asked: nothing happens
    assert records(path) == EVERY_RECORD and not copies()

    flag.touch()
    reset_ranks_if_asked(path)
    assert records(path) == [("Alice", "sc5", "versus", "casual", 1500)]  # the casual record stays
    assert not flag.exists()
    assert [records(str(c)) for c in copies()] == [EVERY_RECORD]  # the database as it was
    db = Database(path)
    for name in ("Alice", "Bob"):  # a new player's, and the account stays
        assert db.rating(accounts[name], "sc5", "versus") == {"elo": elo.INITIAL, "wins": 0, "losses": 0}
        assert db.account_by_name(name) is not None
    db.close()

    reset_ranks_if_asked(path)  # the next start does not do it again
    assert len(copies()) == 1


def full_records(path):
    db = Database(path)
    rows = [tuple(r) for r in db.conn.execute(
        "SELECT a.name, s.game, s.mode, s.kind, s.elo, s.wins, s.losses FROM stats s JOIN accounts a "
        "ON a.id = s.account_id ORDER BY a.name, s.game, s.mode, s.kind")]
    db.close()
    return rows


def test_restoring_brings_back_every_players_elo_and_records(tmp_path):
    path, accounts = database_with_records(tmp_path)
    (tmp_path / RESET_RANKS_FLAG).touch()
    reset_ranks_if_asked(path)
    db = Database(path)
    for name, rating, wins, losses in (("Alice", 491, 2, 1),  # played since the reset
                                       ("Dave", 507, 3, 0)):  # a record that did not exist before it
        db.conn.execute("INSERT INTO stats (account_id, game, mode, kind, elo, wins, losses, updated) "
                        "VALUES (?, 'sc5', 'versus', 'ranked', ?, ?, ?, 0)", (accounts[name], rating, wins, losses))
    db.close()

    flag = tmp_path / RESTORE_RECORDS_FLAG
    restore_ranked_records_if_asked(path)  # not asked: nothing happens
    assert ("Alice", "sc5", "versus", "ranked", 491, 2, 1) in full_records(path)

    flag.touch()
    restore_ranked_records_if_asked(path)
    assert not flag.exists()
    expected = [("Alice", "sc5", "versus", "casual", 1500, 5, 4),
                ("Alice", "sc5", "versus", "ranked", 1700, 7, 5),  # played since: the Elo it had, the records add up
                ("Bob", "sc5", "versus", "ranked", 1640, 5, 4),  # nothing played since: as it was
                ("Carol", "sc5", "versus", "ranked", 1300, 5, 4),
                ("Dave", "sc5", "versus", "ranked", 507, 3, 0)]  # not in the copy: stays
    assert full_records(path) == expected

    restore_ranked_records_if_asked(path)  # the next start does not add them again
    assert full_records(path) == expected
    assert [c for c in tmp_path.glob("server.sqlite.before-records-restore-*") if not c.name.endswith(("-wal", "-shm"))]


def test_a_recount_makes_the_ranked_records_sets_won_and_lost_once(tmp_path):
    path, accounts = database_with_records(tmp_path)  # every record 5-4: matches
    alice, bob = accounts["Alice"], accounts["Bob"]
    db = Database(path)
    for set_id, p1, p2, winner in (("s1", alice, bob, alice),
                                   ("s2", bob, alice, bob),
                                   ("s3", bob, alice, alice),
                                   ("s4", alice, bob, None)):  # void: neither
        db.log_set(set_id, "sc5", "versus", p1, p2, 0, 1, "won" if winner else "void", winner, 0, 0, None)
    db.close()

    flag = tmp_path / RECOUNT_RECORDS_FLAG
    recount_ranked_records_if_asked(path)  # not asked: nothing happens
    assert ("Alice", "sc5", "versus", "ranked", 1700, 5, 4) in full_records(path)

    flag.touch()
    recount_ranked_records_if_asked(path)
    assert not flag.exists()
    expected = [("Alice", "sc5", "versus", "casual", 1500, 5, 4),  # casual counts matches
                ("Alice", "sc5", "versus", "ranked", 1700, 2, 1),  # the Elo stays
                ("Bob", "sc5", "versus", "ranked", 1640, 1, 2),
                ("Carol", "sc5", "versus", "ranked", 1300, 0, 0)]  # no set in the log
    assert full_records(path) == expected
    assert [c for c in tmp_path.glob("server.sqlite.before-records-recount-*") if not c.name.endswith(("-wal", "-shm"))]


def test_a_rank_reset_that_cannot_copy_the_database_clears_nothing(tmp_path, monkeypatch):
    path, _ = database_with_records(tmp_path)
    flag = tmp_path / RESET_RANKS_FLAG
    flag.touch()

    def no_room(self, copy):
        raise OSError("no space left on the device")

    monkeypatch.setattr(Database, "backup", no_room)
    reset_ranks_if_asked(path)  # the server still starts
    assert records(path) == EVERY_RECORD and flag.exists()
