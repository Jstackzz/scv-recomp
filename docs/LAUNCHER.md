# SCV Online: launcher and server

The launcher (`launcher/`, Qt 6 Widgets, C++20) and the lobby/matchmaking server (`server/`, Python asyncio, standard
library only) of SCV Online, Soulcalibur V (Xbox 360) on Windows. Both are ported from MK3D Online
(`mk3d-rollback`, 1.0.10) and keep every one of its features but the jukebox: accounts and TLS pinning, Ranked and
Casual queues with MATCH FOUND, series and rematches, ranked sets and rank badges, lobbies with King of the Hill,
Respect Points and spectators, single player, practice and offline versus, the leaderboard, Build game, Controls,
Settings, the Support page, the updater, crash, desync and F3 reports and the controller in the launcher. The wire
formats and the launcher <-> game contract are in [PROTOCOL.md](PROTOCOL.md); this page is about building, running,
testing and hosting.

```
server/                  Python 3.11+ package, asyncio + standard library only
  __main__.py            python -m server ...
  app.py                 sessions, queue, matches, lobbies, King of the Hill, spectators, UDP rendezvous and relay
  matchmaking.py         pairing rules (pure functions)
  db.py, auth.py, elo.py SQLite storage, scrypt passwords / tokens, Elo and the ranked titles
  makecert.py            python -m server.makecert (self-signed certificate + fingerprint)
  client.py              python -m server.client (test client / scripted player / notices)
  deploy/install.sh      the always-on service on a Linux machine (systemd), beside other servers
  tests/                 pytest suite (python -m pytest server/tests)
launcher/                Qt 6 Widgets, C++20, CMake + Ninja + MSVC
  data/sc5.json          the game, its modes and roster (compiled into the exe)
  data/logo, data/ranks  SCV Online's emblem, icons and the 16 rank badges (tools/make_art.py draws them)
  data/sounds/           match-found.wav, the MATCH FOUND chime (MK3D Online's, synthesized)
  src/                   MainWindow, pages, ServerClient (TLS), UdpRendezvous, TunnelBridge, GameProcess,
                         MatchController, FeedLink (spectators), ReportSender, Controls, X360 (disc, packages, XEX),
                         GamepadNav, Updater
  tools/fake_game.py     the stand-in for sc5.exe: speaks PROTOCOL.md section 3 without the game
  tools/make_art.py      draws the emblem, icons and rank badges (Pillow)
  tools/aqt_split_layout.py  aqtinstall shim for Qt 6.11+, to install Qt into external\qt (MK3D Online's)
  build.cmd              configure + build + windeployqt into build\launcher
tools/online-e2e.ps1     one PC: the server, two launchers (and a spectator), their games: queue, ranked, lobby
tools/koth-e2e.ps1       one PC: a King of the Hill lobby with three launchers, then the leaderboard
```

## Server

### Run it locally

Python 3.11 or newer, nothing else for the server itself; the certificate helper uses the `cryptography` package or
falls back to the `openssl` command. From the repository's root:

```
python -m server.makecert                 # writes server-cert.pem + server-key.pem, prints the fingerprint
python -m server --host 0.0.0.0 --port 7777 --udp-port 7778 --db server.sqlite
```

The server looks for `server-cert.pem` / `server-key.pem` next to the database unless `--cert/--key` say otherwise,
and prints the SHA-256 fingerprint at start-up. Useful options:

| option | meaning |
|---|---|
| `--motd TEXT` | message of the day shown in every launcher |
| `--admins a,b` | accounts allowed to broadcast notices (`python -m server.client notice ...`) |
| `--min-client 0.1.0` | refuse older launchers (the version gate) |
| `--update-file PATH` | the release launchers are offered (default `update.json` beside the database; PROTOCOL.md 1.3) |
| `--releases-page URL` | where an outdated launcher is sent when the update file names no page (default: none) |
| `--allow-same-ip` | let ranked pair two players behind one public IP (LAN events, testing) |
| `--accept-timeout 20` | seconds to accept a found match |
| `--upnp` | open the server's TCP and UDP ports on this network's router (a server on a home PC) |
| `--no-relay` | cancel a match whose players cannot reach each other instead of relaying it through the server |
| `--relay-only` | development: relay every match (tests the relay on one PC) |
| `--log-file server.log`, `--log-level DEBUG` | logging; disputes are logged as `DISPUTE` lines |
| `--plain` | development only: no TLS (launcher address `tcp://host:port`) |

The game is `sc5` with the modes `versus` (Ranked, Casual and lobbies) and `sparring` (Online Practice, lobbies
only); the ranked titles are Soulcalibur's (Novice to Eternal Soul, PROTOCOL.md). State that survives restarts lives
in the SQLite file (accounts, login tokens as SHA-256, ranked Elo and sets, casual wins and losses, Respect Points,
the match log, every result report); queues, lobbies, series and matches in progress are in memory.

### Tests

```
python -m pip install --user pytest pytest-asyncio
python -m pytest server/tests
```

The suite (125 tests) starts a real TLS server on ephemeral ports with a throwaway certificate and database and covers
accounts, pinning, rate limiting, queue pairing, accept/decline/timeouts, the UDP rendezvous, relay and tunnel,
results, series and ranked sets, ranks, lobbies, Online Practice, spectators, King of the Hill, reports, UPnP and
updates.

### Scripted players

```
python -m server.client demo --fingerprint <fp>             # two throwaway accounts play one match
set SC5_PASSWORD=...                                        # never on the command line
python -m server.client play --fingerprint <fp> --name tester2 --register --game sc5 --mode versus --udp-port 7002
```

`play` behaves like a launcher with a simulated game: it queues, accepts, punches, answers the stand-in game's
heartbeats and reports a result derived from the match seed, so it agrees with a launcher running
`launcher/tools/fake_game.py`.

### Hosting

`server/deploy/install.sh` installs the server as the systemd service `sc5-server` on a Linux machine with a public
address: the code in `/opt/sc5`, the database, certificate, version gate (`min-client`) and `update.json` in
`/var/lib/sc5`, run as the system user `sc5`. `SC5_TCP_PORT` / `SC5_UDP_PORT` pick other ports than 7777/7778 on a
machine that already runs another server; the script opens them in the machine's own firewall (ufw, firewalld, or
the iptables rules of Oracle's Ubuntu images) and leaves every other service, folder and rule alone. The cloud's own
network firewall (Oracle's security list) must allow the two ports too. Running it again updates the server and keeps
the accounts and the certificate (whose fingerprint the launchers pin). From Windows:

```
tar -c -f sc5-server.tar --exclude __pycache__ --exclude tests server
scp -i <key> sc5-server.tar ubuntu@<address>:/tmp/
ssh -i <key> ubuntu@<address> "mkdir -p /tmp/sc5 && tar -x -f /tmp/sc5-server.tar -C /tmp/sc5 && sudo env SC5_TCP_PORT=7797 SC5_UDP_PORT=7798 sh /tmp/sc5/server/deploy/install.sh <address> <oldest launcher>"
```

The official SCV server runs that way on the Oracle Cloud machine that also runs MK3D Online's (TCP 7777, UDP 7778)
and MK9 Online's (7787, 7788): SCV's is TCP 7797 and UDP 7798. Its address and certificate fingerprint go in a
player package's `defaults.ini` beside the launcher (`[server]` `address=<host>:7797`, `fingerprint=<SHA-256>`), kept
out of the repository because it names the machine. A release launchers should take: publish its zip, write
`/var/lib/sc5/update.json` (version, url, sha256, size, notes, page; PROTOCOL.md 1.3), and raise `min-client` with
the installer's second argument when every player must take it.

## Launcher

### Build

Nothing is downloaded. Qt 6.12.0 (MSVC 2022 64-bit, ABI compatible with the VS 2026 Build Tools' MSVC 14.50) comes
from `external\qt\6.12.0\msvc2022_64`, else MK3D Online's copy (`mk3d-rollback\external\qt`, read only); SDL3's
sources (for controllers, linked statically) from `external\sdl3`, else the copy in MK3D Online's game build
(`mk3d-rollback\recomp\mkd\out\build\release\_deps\sdl3-src`, SDL 3.4.16); MSVC, CMake and Ninja from the VS 2026
Build Tools. `QT_DIR`, `SDL3_DIR` and `BUILD_DIR` override the three folders.

```
launcher\build.cmd
```

builds `build\launcher\SC5Launcher.exe` and copies the Qt runtime beside it (TLS through Windows' Schannel, no
OpenSSL). The exe carries its version and publisher (`data/version.rc.in`) and SCV Online's icon. By hand:
`cmake -S launcher -B build\launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<qt>
-DSC5_SDL3_DIR=<sdl3 sources>` in a VS developer prompt, `cmake --build build\launcher`, then `windeployqt`.

The look (`src/Style.cpp`) is Soulcalibur's colours and none of its art: navy and black, gold for titles and ranks,
a cool blue for what is active or chosen, and the gold focus (two pixels wide) that a controller or Tab moves. The
emblem (a sword through a gold V in a ring), the icons and the rank badges are drawn by `launcher/tools/make_art.py`
from plain shapes: `python launcher/tools/make_art.py` writes them again.

### Where things are

Settings, log and sessions live beside the exe (`sc5-launcher.ini`, `sc5-launcher.log`, `controls.toml`,
`display.txt`, `sessions\`), or in `%LOCALAPPDATA%\SC5\SC5 Online` when that folder is read-only. A `defaults.ini`
beside the exe (a player package's server) supplies any setting the settings file does not have.

| setting (`games/sc5/...`) | source build (default) | player package (default) |
|---|---|---|
| `folder` (the game's files) | `<repo>\game` | `games\sc5` |
| `exe` | `<repo>\sc5\out\build\win-amd64-release\sc5.exe` | `games\sc5\sc5.exe` |
| `build_script` (Build game's last step) | `<folder>\build-game.cmd` | `games\sc5\build-game.cmd` |

Below the game folder: `disc\` (the extracted disc, `default.xexp` beside `default.xex`), `update\` (`patch.cpk`),
`dlc\` (the player's own DLC), `user\` (single player's saves) and `cache\` (shaders). The repository is found by
walking up from the exe to a folder with `tools\py\x360.py` and `launcher\CMakeLists.txt` (`tools/repo_root`
overrides it); a package is a `games\` folder beside the exe. Every game the launcher starts gets a session folder
`sessions\sc5-<yyyyMMdd-hhmmss>-<instance>` (the newest 30 kept) with `cmd.txt`, `status.txt`, `game.log`, the
netplay logs, F3 captures and, for an online game, its fresh save `user\`.

### The pages

1. **Header and sign in.** The emblem, *SCV ONLINE*, the game (Soulcalibur V, Xbox 360, World: there is one, so no
   game selector) and the account button: create an account or sign in; "remember me" keeps a 30-day login token. A
   launcher that never signed in on this PC opens the window by itself, on *create account*, when it first connects.
2. **Ranked / Casual** (Casual is `casual` in the protocol): the mode (Versus), the character or *Pick my character
   on the game's own select screen* (`[online] pick_in_game`, on by default: the start line says `-` and the player
   picks with their own controller), region and max ping (Ranked: *Strict*), *Practice while you wait* (the search
   starts the game's training, `SC5_TRAINING=1`; a match found closes it and starts the match's game), *Search again
   when a set or series ends*, FIND MATCH / CANCEL SEARCH (START on a controller). The Matchmaking card shows who
   plays and searches right now (the server's `online` counts). A match found has 20 s to be accepted: the MATCH
   FOUND window stays on top of the game, plays a chime (Settings) and shows the opponent's connection (Wi-Fi or
   wired, the ping); ranked shows your rank and keeps the opponent hidden until both accept. Then the launcher
   punches the opponent on the game port (UPnP, hole punching, else the server's relay, else the tunnel over its TLS
   connection), measures the ping, writes the start line, reports the result the game writes and parks the game.
   After a match the game waits at its result screen, still connected to the other's (a queue match reached
   straight), and the series card shows the score against that opponent: Casual offers REMATCH (the same characters
   on the same stage, as the game's own Rematch, when both choose it), CHARACTER SELECT (both pick again; either
   one's choice of it takes both there) and LEAVE for 60 s after every match, in the card and in the *Rematch?* window
   that comes over the game (a controller works it); a ranked set (first to 3) asks for its next match by itself, only
   the last match's loser picks again, and *Leave set* asks first when leaving would lose it. The next match starts
   from the result screen (`MatchController::Phase::AtResult`, the game's `rematch`: both REMATCH is the game's own
   Rematch, anything else goes back through Character Select; no new punch, no restart); a series over, a relayed match or an older opponent's launcher park the game as before.
   *Your record* shows the wins and losses (and in Ranked the Elo, the rank's badge, the next rank and the whole
   ladder in its tooltip).
3. **Lobby**: create (name, mode, password, 1, 2 or 3 rounds to win, 3 as on the console, *King of the Hill*),
   join from the list or by its 5-letter code, pick a character (or *Pick in the game*), chat, *Ready*: both players
   ready start the match after
   the server's 3-2-1 countdown. After a match played straight between the two (no relay), both games stay at their
   result screens and the match's loser (player 1 after a draw) chooses REMATCH or CHARACTER SELECT in the game with
   their own controller, on a panel over the Results screen that both players see; the next match then starts from
   there by itself (PROTOCOL.md 1.10, `in_place`). Nobody choosing in a minute takes both back to the lobby, to ready
   again. *Watch* makes you a spectator: each lobby match plays in your game from both
   players' feeds (`FeedLink`, PROTOCOL.md 1.10.1 and 3.6). **King of the Hill**: the winner stays on, the next in
   line plays the king, everyone else watches and rates the winner from 1 to 10 after each match: Respect Points,
   on the leaderboard. **Online Practice** (`sparring`): two friends by the code, no timer, health refills, nobody is
   knocked out, BACK goes back to character select, nothing recorded. *Leave* closes the game the lobby started.
4. **Single player**: everything on the disc is unlocked in all three (the game's `unlocks.cpp`, every mode; paid
   DLC from the player's own packages). PLAY starts the game plainly (its own menus); PRACTICE goes to training's
   select screen (`SC5_TRAINING=1`); VERSUS to the two-player select screen (`SC5_VERSUS=1`), player 2 on the
   controller chosen below. *Close the game* closes it. A queue search can run
   alongside (standby): the match found freezes single player and thaws it afterwards. In a source build the
   **Local test** box starts two instances on this PC (UDP 7001/7002) with matching start lines, no server needed.
5. **Leaderboard**: top 100 by Elo, or by Respect Points (on a server with King of the Hill), with each rank's badge.
6. **Build game** (`BuildPage`, `X360.cpp`, PROTOCOL.md 3.7): (1) the disc image (`.iso`, XGD2/XGD3 or XISO),
   (2) the title update package (`TU_174Q21T_...`), (3) optionally the player's own DLC packages, then *CHECK AND
   BUILD*. The launcher reads the disc's file system and the Xbox Live packages itself (no Python): the disc's
   `default.xex` must be the World release's (SHA-1 `dfe4d5e1...`, version 0.0.0.2), the update must be a `LIVE`
   title update of title `4E4D083D` whose `default.xexp` (SHA-1 `47a09662...`) patches 0.0.0.2 into 0.0.1.2, a DLC
   package must be downloadable content (`00000002`) of the same title (the packages `tools/py/ps3dlc.py` makes from a
   player's PS3 DLC are taken the same way, GAME.md, the costume packs' with their converted data, up to 120 MB and
   60 files each, in STFS hash tables of one to three levels); anything else is named and refused (another
   game or update, or a modified copy; every region's disc is this World one), since both players of a match must run the same code. It checks
   the free space, extracts the disc into `disc\` on a worker thread with a progress bar (the dashboard update,
   `$SystemUpdate`, left out; a file already there with the right size kept, so a stopped extraction goes on),
   puts `default.xexp` beside `default.xex`, `patch.cpk` into `update\` and each DLC package into `dlc\`. An empty
   disc or update field uses what is already in place (checked by hash again). The last step, compiling the game's
   code for this PC, is one function (`BuildPage::compileGame`, also the *Compile the game's code* button): it runs
   the game folder's `build-game.cmd` (a player package's, sc5/player/README.md: codegen and clang from the package,
   nothing installed) and shows what it prints; without that script (a source build's `game\`) it says the step is
   not ready.
7. **Controls**: **In a fight**: what each of the Xbox 360 controller's A, B, X, Y, bumpers, triggers and stick clicks
   presses in a fight, in every mode, online and offline: any combination of the game's own A (horizontal attack), B
   (vertical attack), K (kick) and G (guard), all 16, where the game's own Control Settings offer 13 and none on the
   stick clicks; *Default fight buttons* puts the console's Type A back. Online the other player's game sees the
   presses as set here (PROTOCOL.md 3.1, `[fight]`). Then which keys press each button of the Xbox 360 controller (two
   at most; click one and press the new key, Esc cancels, *No key* clears it) and what each button of a PC controller
   presses, named by where it sits (the bottom face button is Xbox A, PlayStation cross, Switch B). Every button is
   named by what it does in a fight: "Horizontal attack (X)", "A+G (LB)". Saved at each change to `controls.toml`
   (`SC5_CONTROLS`, PROTOCOL.md 3.1); a running game reads it again within a second.
8. **Settings**: **Server** (address `host:port` and the fingerprint the server's host gives out, *Connect*; with an
   empty fingerprint the launcher shows the presented one and asks whether to trust it), **Online** (region, max
   ping, input delay 0-4 and *More delay when the ping is high*, the game UDP port (7001), strict ranked, the match
   found chime, UPnP, *Hide my IP address from opponents* and *Even when the ping through the server is high*),
   **Display**: the window (windowed, borderless, fullscreen), its resolution, the internal resolution (1280x720 as
   on the console, 2x, 3x or 4x), anti-aliasing (off, FXAA, stronger FXAA), texture filtering (the game's own or 2x
   to 16x anisotropic), v-sync (on; on with a G-SYNC or FreeSync monitor; off), brightness and frame data in
   practice (a frame meter as Street Fighter 6's over Training and Online Practice, F7 in the game hides it;
   GAME.md). Each changes only how a frame looks, never what the game computes, so two players may set them
   differently in one match; they reach the game as `SC5_*` variables and `display.txt`, which a running game follows
   (PROTOCOL.md 3.1). **Sound**: master, music, effects and voices, percent of the game's own. **Game folder**:
   folder, executable and build script. **Tools and development** (a source build): Python, the repository, the local
   test's ports.
9. **Support** (`SupportPage`): where to follow ImTheBlindGuy: YouTube (`youtube.com/@lmTheBIindGuy`) and Twitch
   (`twitch.tv/imtheblindguy`), each with a button that opens it in the browser.

**The controller in the launcher** (`GamepadNav`, SDL3): only while a launcher window is in front, since the game
reads the controllers in the background too. The d-pad or left stick moves the focus (gold) to the nearest control,
A clicks, B is Esc in a dialog (not in MATCH FOUND or the rematch prompt), LB / RB step the pages, START presses FIND
MATCH on a queue page and Ready on the Lobby page. While a match is connecting, played, reported or watched the
controller does nothing in the launcher. `SC5_PAD_ANY_WINDOW=1` takes any active launcher window as in front (tests
on Qt's offscreen platform with an SDL virtual gamepad).

**Crash reports**: a game that ends with an exception's exit code, or whose log ends with the game's crash line, is
reported to the server (PROTOCOL.md 1.13) with what was going on. **Desync reports**: a match whose games went out
of step is reported with what the game logged of it (1.13.1): Windows, the processor, the game's executable, the
match's lines from the link's `start` line, and the extracted disc's files (count, bytes, a hash of names and sizes
and, once hashed in the background, of their contents; `game-files-sc5.txt` beside the settings keeps the hashes).
**F3 reports** (`ReportSender`, 1.13.2): F3 in the game saves a capture under the session (`SC5_CAPTURE_DIR`); the
launcher sends its pictures, textures, draws and the end of the game's log to the server, which keeps them under
`/var/lib/sc5/reports/<day>/`. **How a game ended**: a game that ends during a match reports `left` with `exit`
and `exit_code`, from the last lines of its log.

**Updates** (`Updater`): the server names the latest release (`welcome.update`, an `update` push, the
`client_outdated` reply). A player package offers it in a window (UPDATE NOW, LATER, the notes and the release
page); UPDATE NOW downloads the zip into `update\` beside the launcher, checks its size and SHA-256 against the
server's (whose certificate is pinned; the download host is not trusted), unpacks it, checks its `VERSION.txt`
(`SC5-Online <version>, ...`) and swaps the release's files in once nothing uses the game (each old file renamed to
`<name>.sc5-old` and deleted at the next start); then the new launcher starts with `--updated <old version>`, and
when the release changed the game (`games\sc5\built.txt`, which `build-game.cmd` copies from `project\VERSION`,
names another release) it builds the game again on Build game before anything starts it (`GameProcess::needsRebuild`,
as MK9 Online's launcher). A source build only links to the release page.

### Without the game

Until `sc5.exe` speaks PROTOCOL.md section 3, point the game's executable at `launcher/tools/fake_game.py`
(Settings, *Game folder*; the launcher runs `.py` files with the configured Python, without a window). The stand-in
goes `booting` -> `ready`, takes `start`, reports `starting`, binds the start line's `local_port`, exchanges
heartbeats with `remote`, writes `in_match` and after `SC5_FAKE_SECONDS` (default 6) a result computed from the
seed, so both sides agree (Online Practice: `sparring_over`). In a lobby match it writes a spectator feed to the
start line's `feed` (READY, inputs, checks, the frame it left at, under the `SC5R` header); started to watch
(`watch=1`) it reads both players' feed and reports `in_match` and the result from it. Its `game.log` starts with
every `SC5_` variable it was given. `SC5_FAKE_RESULT=desync` makes it report a desync with a log like a real game's.

### Command line (development and automation)

| option | effect |
|---|---|
| `--config <ini>` | use another settings file (two launchers on one PC need two) |
| `--server host:port`, `--fingerprint <hex>` | override and store the server settings |
| `--user NAME [--register]` | sign in (or create) with the password from `SC5_DEV_PASSWORD` (`--password-env` picks another variable) |
| `--queue casual\|ranked --mode versus --character Siegfried` | search right after signing in |
| `--auto-accept N` | accept a found match after N seconds |
| `--cancel-search N` | with `--queue`: cancel the search N seconds after it starts, as CANCEL SEARCH does |
| `--rematches N`, `--rematch-choice rematch,select`, `--leave-series` | with `--queue casual`: choose a rematch after each match, N times, then leave or let it run out |
| `--solo` | with `--queue`: start single player first and search alongside it (standby) |
| `--practice on\|off`, `--search-again on\|off` | the queue pages' boxes, saved |
| `--lobby host\|join\|watch --lobby-name NAME [--lobby-code-file F] [--lobby-matches N]` | host a lobby and ready, join it and ready, or watch it as a spectator; the code through a file (Online Practice lobbies are never listed) |
| `--koth`, `--rate N` | a King of the Hill lobby; rate each match watched, 1 to 10 |
| `--leave-lobby`, `--leave-lobby-in-match S` | leave the lobby after its matches, or S seconds into the first |
| `--snap-dir <folder>` | save pictures of the window as the lobby changes, of the Create lobby window and of the leaderboard in both orders; with `--page`, a picture of that page |
| `--page ranked\|casual\|lobby\|single\|leaderboard\|build\|controls\|settings\|support` | start page |
| `--local-test` | start the two-instance loopback test |
| `--extract <image>` | open Build game and check and extract that image (the title update and DLC from the settings) |
| `--auto-update`, `--updated <version>` | install an offered update without asking (tests); started by an update |
| `--upnp-probe` | find the router and its public address, open nothing |

### End-to-end tests on one PC

`tools\online-e2e.ps1` runs this checkout's server on a throwaway database and certificate, two launchers on Qt's
offscreen platform (no window on the screen) and the stand-in games they start, and prints every status line, what
the launchers did and the database's records:

```
powershell -File tools\online-e2e.ps1 -Queue casual -Rematches 1      # match found, accept, match, result, rematch
powershell -File tools\online-e2e.ps1 -Queue ranked                   # a ranked set, Elo and titles
powershell -File tools\online-e2e.ps1 -Queue lobby -Matches 2 -Watch  # a lobby, two matches, a spectator
powershell -File tools\online-e2e.ps1 -Queue lobby -Mode sparring     # Online Practice
powershell -File tools\online-e2e.ps1 -Queue ranked -Draw -Matches 2  # drawn matches: nobody's point in the set
powershell -File tools\koth-e2e.ps1 -Matches 3 -Snaps                 # King of the Hill, Respect Points, leaderboard
```

The stand-in's results name the fighters as the real game does, by its own character ids with the costume after a
dot (`p1_char=3.0`), so a ranked set's winner keeps theirs in the next start line. `-Exe` runs another game (the real
`sc5.exe` speaks section 3 for versus since 2026-11-17: its `tools\linktest.py` drives two of them through the same
files), `-Visible` shows the launchers' windows, `-Snaps` saves pictures of them. The work folders
(`build\online-e2e`, `build\koth-e2e`) stay for a look and are replaced by the next run.

## Known limitations

- The game speaks all of section 3 (2026-11-17): versus, Online Practice (`sparring`), practice and watching (the
  spectator feed), the display and sound settings (`SC5_DISPLAY_FILE`) and `controls.toml` (docs\GAME.md, "The
  launcher's game"). The launcher's own end-to-end runs above still use `launcher/tools/fake_game.py`;
  `tools\linktest.py` drives two or three real games through the same files.
- Few matches have been played between two PCs yet (the first, 2026-11-17, on one home network, found the throws'
  heap pointer, GAME.md "Two PCs"); the other tests ran two or three games on one PC with lag added (up to 150 ms each
  way), with one game's heap moved to stand in for a second PC.
- Build game compiles the game only in a player package (its `games\sc5\build-game.cmd`, tested 2026-11-17 on a
  laid-out package; `tools\package.ps1`), not in a source build, where `sc5\build.cmd` does. Before the first build
  a package says `sc5.exe` is missing, as if an antivirus had taken it.
- Dampierre (downloadable) is picked in the game only, by players who both have him.
- A relayed or tunnelled match's next match is still a `park` and a new `start`, not a rematch from the result
  screen.
- Created characters, original ones and customized fighters, come from the offline save, read only
  (`SC5_CREATION_SAVE`, docs/PROTOCOL.md 3.3). One that the other PC's check would refuse is left out of the online
  select screen (a customized fighter is played in one of its fighter's own two costumes), and the main menu's
  Characters Created count stays 0 online.
- Results are self-reported: agreement counts, disagreement voids the match, a single report counts for nothing.
- One server process with in-memory queues and lobbies; a restart drops searches and matches in progress.
- The launcher is Windows only and not code-signed.
