"""The release launchers are offered to update to: the update file, welcome, the outdated reply, the push."""

from __future__ import annotations

import asyncio
import json
import os

import pytest
import pytest_asyncio

from server import __version__
from server import protocol as P
from server.__main__ import build_parser, config_from_args
from server.app import Server, ServerConfig
from server.client import Connection, ServerError
from server.matchmaking import MatchmakingConfig

RELEASE = {
    "version": "0.1.8",
    "url": "https://example.org/sc5-rollback/releases/download/v0.1.8/SC5-Online-0.1.8-windows.zip",
    "sha256": "ab" * 32, "size": 96_000_000, "notes": "What is new.",
    "page": "https://example.org/sc5-rollback/releases/tag/v0.1.8",
}
PAGE = "https://example.org/sc5-rollback/releases"  # a server's --releases-page


async def start(tmp_path, tls_files, **cfg) -> Server:
    cert, key, _ = tls_files
    srv = Server(ServerConfig(
        host="127.0.0.1", port=0, udp_port=0, db_path=str(tmp_path / "test.sqlite"), certfile=cert, keyfile=key,
        tick_seconds=0.05, update_check_interval=0.1, min_client="0.1.7", update_file=str(tmp_path / "update.json"),
        mm=MatchmakingConfig(allow_same_ip=True), **cfg,
    ))
    await srv.start()
    return srv


@pytest_asyncio.fixture
async def server(tmp_path, tls_files):
    """A server without a releases page of its own (the default)."""
    srv = await start(tmp_path, tls_files)
    try:
        yield srv
    finally:
        await srv.stop()


@pytest_asyncio.fixture
async def paged_server(tmp_path, tls_files):
    """A server with a releases page (--releases-page)."""
    srv = await start(tmp_path, tls_files, releases_page=PAGE)
    try:
        yield srv
    finally:
        await srv.stop()


def write_update(server: Server, content: str, stamp: int) -> None:
    """The update file, with its own modification time (two writes in one clock tick would look unchanged)."""
    path = server.cfg.update_file
    assert path
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    os.utime(path, ns=(stamp * 1_000_000_000, stamp * 1_000_000_000))


async def launcher(server: Server, version: str, build: str = "") -> tuple[Connection, dict]:
    conn = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    welcome = await conn.connect(version=version, build=build)
    return conn, welcome


def test_parse_update_checks_every_field():
    assert P.parse_update(RELEASE) == RELEASE
    assert P.parse_update(RELEASE, PAGE) == RELEASE  # its own page comes first
    # without a page of its own the release names the server's releases page, or no page at all
    bare = {k: v for k, v in RELEASE.items() if k not in ("notes", "page")}
    assert P.parse_update(bare, PAGE)["page"] == PAGE and P.parse_update(bare)["page"] == ""
    local = P.parse_update({**RELEASE, "url": "http://127.0.0.1:8000/SC5-Online-0.1.8-windows.zip"})
    assert local["url"].startswith("http://127.0.0.1")
    for bad in ({"version": "0.1.8-beta"}, {"url": "http://example.com/a.zip"}, {"url": "file:///C:/a.zip"},
                {"sha256": "AB" * 32}, {"sha256": "ab" * 31}, {"size": 0}, {"size": True}, {"notes": "x" * 2001},
                {"page": "javascript:alert(1)"}, {"page": ""}, {"page": None}):
        with pytest.raises(ValueError):
            P.parse_update({**RELEASE, **bad})
    with pytest.raises(ValueError):
        P.parse_update([RELEASE])


async def test_welcome_offers_a_newer_release(server):
    conn, welcome = await launcher(server, "0.1.7")
    assert "update" not in welcome  # no update file yet
    await conn.close()

    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    older, welcome = await launcher(server, "0.1.7")
    assert welcome["update"] == RELEASE
    current, welcome = await launcher(server, "0.1.8")
    assert "update" not in welcome
    for c in (older, current):
        await c.close()

    write_update(server, '{"version": "0.1.9"', 1_800_000_100)  # broken: ignored, nothing offered
    conn, welcome = await launcher(server, "0.1.7")
    assert "update" not in welcome
    await conn.close()


async def test_outdated_launcher_gets_the_link_and_the_release(server):
    old = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    with pytest.raises(ServerError) as exc:
        await old.connect(version="0.1.6")
    # no update file and no releases page: the message names no page
    assert exc.value.code == "client_outdated" and exc.value.msg["message"] == "Update the launcher to 0.1.7 or newer."
    assert "update" not in exc.value.msg
    await old.close()

    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    old = Connection("127.0.0.1", server.tcp_port, fingerprint=server.fingerprint)
    with pytest.raises(ServerError) as exc:
        await old.connect(version="0.1.6")
    # what a launcher without the updater shows, and what one with it installs
    assert exc.value.msg["message"] == f"Update the launcher to 0.1.7 or newer: {RELEASE['page']}"
    assert exc.value.msg["update"] == RELEASE
    await old.close()


async def test_the_servers_releases_page_when_the_update_file_names_none(paged_server):
    async def outdated() -> dict:
        old = Connection("127.0.0.1", paged_server.tcp_port, fingerprint=paged_server.fingerprint)
        with pytest.raises(ServerError) as exc:
            await old.connect(version="0.1.6")
        await old.close()
        assert exc.value.code == "client_outdated"
        return exc.value.msg

    assert (await outdated())["message"] == f"Update the launcher to 0.1.7 or newer: {PAGE}"
    write_update(paged_server, json.dumps({k: v for k, v in RELEASE.items() if k != "page"}), 1_800_000_000)
    reply = await outdated()
    assert reply["message"] == f"Update the launcher to 0.1.7 or newer: {PAGE}" and reply["update"]["page"] == PAGE
    write_update(paged_server, json.dumps(RELEASE), 1_800_000_100)  # a page of its own comes first
    reply = await outdated()
    assert reply["message"] == f"Update the launcher to 0.1.7 or newer: {RELEASE['page']}"
    assert reply["update"] == RELEASE


def test_the_command_line(tmp_path, capsys):
    """--releases-page (https://, none by default) goes into the server's config; --version names the server."""
    db = str(tmp_path / "server.sqlite")
    assert config_from_args(build_parser().parse_args(["--db", db])).releases_page == ""
    assert config_from_args(build_parser().parse_args(["--db", db, "--releases-page", PAGE])).releases_page == PAGE
    for bad in ("http://example.org/releases", "javascript:alert(1)", "example.org/releases"):
        with pytest.raises(SystemExit):
            build_parser().parse_args(["--releases-page", bad])
        assert "is not an https:// link" in capsys.readouterr().err
    with pytest.raises(SystemExit):
        build_parser().parse_args(["--version"])
    assert capsys.readouterr().out.strip() == f"sc5-server {__version__}"


async def test_a_release_announced_reaches_connected_launchers(server):
    older, _ = await launcher(server, "0.1.7")
    current, _ = await launcher(server, "0.1.8")
    write_update(server, json.dumps(RELEASE), 1_800_000_000)
    pushed = await older.expect("update", timeout=3)
    assert {k: pushed[k] for k in RELEASE} == RELEASE
    await asyncio.sleep(0.3)  # more ticks: offered once, and not to a launcher that has it
    assert not older.take_all("update") and not current.take_all("update")
    for c in (older, current):
        await c.close()


def test_an_update_file_may_name_its_build():
    built = {**RELEASE, "build": "2026-11-20 06:17"}
    assert P.parse_update(built) == built
    for bad in ("", "x" * 41, "2026-11-20\n06:17", 20261120):
        with pytest.raises(ValueError):
            P.parse_update({**RELEASE, "build": bad})
    assert P.client_build(" 2026-11-20 06:17 ") == "2026-11-20 06:17"
    assert P.client_build("bad\x07") == "" and P.client_build(None) == "" and P.client_build(7) == ""


async def test_a_test_build_of_the_releases_number_is_offered_the_release(server):
    """A launcher of the release's own version is offered it when it is another build of it (a test build given out
    before the release took its number: the version alone never told): both builds known, and different. Newer
    launchers (a developer's), launchers that name no build and an update file that names none are offered nothing."""
    built = {**RELEASE, "build": "2026-11-20 06:17"}
    write_update(server, json.dumps(built), 1_800_000_000)
    for version, build, offered in (("0.1.8", "2026-11-19 22:10", True), ("0.1.8.0", "2026-11-19 22:10", True),
                                    ("0.1.8", "2026-11-20 06:17", False), ("0.1.8", "", False),
                                    ("0.1.9", "2026-11-19 22:10", False), ("0.1.7", "2026-11-20 06:17", True)):
        conn, welcome = await launcher(server, version, build)
        assert ("update" in welcome) is offered, (version, build)
        if offered:
            assert welcome["update"] == built
        await conn.close()
    write_update(server, json.dumps(RELEASE), 1_800_000_100)  # no build named: the version alone decides
    conn, welcome = await launcher(server, "0.1.8", "2026-11-19 22:10")
    assert "update" not in welcome
    await conn.close()


async def test_a_new_build_of_a_release_is_pushed_to_the_other_builds(server):
    """The update file names another build of the same version (the release made again): the launchers connected
    with the earlier one are offered it within the check interval, those with it already are not."""
    write_update(server, json.dumps({**RELEASE, "build": "2026-11-20 06:17"}), 1_800_000_000)
    await asyncio.sleep(0.3)
    first, _ = await launcher(server, "0.1.8", "2026-11-20 06:17")
    newest, _ = await launcher(server, "0.1.8", "2026-11-21 09:00")
    write_update(server, json.dumps({**RELEASE, "build": "2026-11-21 09:00"}), 1_800_000_100)
    pushed = await first.expect("update", timeout=3)
    assert pushed["build"] == "2026-11-21 09:00"
    await asyncio.sleep(0.3)
    assert not newest.take_all("update") and not first.take_all("update")
    for c in (first, newest):
        await c.close()
