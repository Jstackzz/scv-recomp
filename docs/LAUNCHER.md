# SCV Online: launcher and server

The launcher (`launcher/`, Qt 6 Widgets, C++20) and the lobby/matchmaking server (`server/`, Python asyncio, standard
library only) of SCV Online, Soulcalibur V (Xbox 360) on Windows. Both are ported from MK3D Online
(`mk3d-rollback`, 1.0.10) and keep every one of its features but the jukebox: accounts and TLS pinning, Ranked and
Casual queues with MATCH FOUND, series and rematches, ranked sets and rank badges, lobbies with King of the Hill,
Respect Points and spectators, the leaderboard, the updater, crash, desync and F3 reports. From SCV 0.1.8 the player
does all of that in the game's own menus (PROTOCOL.md 3.8), and the launcher is small: Play (the account and PLAY),
Build game, Settings and Support, with everything online running in the background for the game. The wire formats and
the launcher <-> game contract are in [PROTOCOL.md](PROTOCOL.md); this page is about building, running, testing and
hosting.

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
  src/                   MainWindow and its pages (PlayPage, ToolPages: Build game and Settings, KeyboardBox,
                         SupportPage), MenuBridge (the game's online menus: ui.json and ask.txt), TestDriver (the test
                         switches), ServerClient (TLS), UdpRendezvous, TunnelBridge, GameProcess, MatchController,
                         FeedLink (spectators), ReportSender, Controls, X360 (disc, packages, XEX), GamepadNav, Updater
  tools/fake_game.py     the stand-in for sc5.exe: speaks PROTOCOL.md section 3 without the game (and plays the
                         player in its menus from a script of asks)
  tools/make_art.py      draws the emblem, icons and rank badges (Pillow)
  tools/aqt_split_layout.py  aqtinstall shim for Qt 6.11+, to install Qt into external\qt (MK3D Online's)
  build.cmd              configure + build + windeployqt into build\launcher
tools/online-e2e.ps1     one PC: the server, two launchers (and a spectator), their games: queue, ranked, lobby, asks
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

The suite (147 tests) starts a real TLS server on ephemeral ports with a throwaway certificate and database and covers
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
`/var/lib/sc5/update.json` (version, url, sha256, size, notes, page, and build: the zip's `VERSION.txt` stamp, which
`New-PlayerPackage` prints, so a test build of the same number is offered the release; PROTOCOL.md 1.3), and raise
`min-client` with the installer's second argument when every player must take it.

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
`dlc\` (the player's own DLC), `user\` (the player's saves, which the game PLAY starts plays on, online too) and
`cache\` (shaders). The repository is found by walking up from the exe to a folder with `tools\py\x360.py` and
`launcher\CMakeLists.txt` (`tools/repo_root` overrides it); a package is a `games\` folder beside the exe. Every game
the launcher starts gets a session folder `sessions\sc5-<yyyyMMdd-hhmmss>-<instance>` (the newest 30 kept) with
`cmd.txt`, `status.txt`, `ui.json`, `ask.txt.taken`, `game.log`, the netplay logs and F3 captures.

### The pages

From SCV 0.1.8 the player does everything online inside the game's own menus (PROTOCOL.md 3.8): Ranked, Casual,
lobbies, Online Practice, watching, the leaderboard and the preferred side are the game's, beside its single player,
Creation and Training. The launcher is small: it builds the game, starts it, signs the player in, and keeps doing all
the online work in the background for the game. Its look is large, plain, high-contrast text (12 pt at the least, the
Play page's lines 16 pt and PLAY itself 36 pt) and few things on a page.

1. **Play** (`PlayPage`, the start page): the account (SIGN IN and CREATE ACCOUNT, or the signed-in name with the rank's
   badge, its title and Elo, and the record: ranked sets and casual matches won and lost; *Sign out*; CONNECT AGAIN
   when the server cannot be reached), one big **PLAY** button, and three lines in plain words: how the game is doing
   (not running, starting, running, in an online match), how the connection is doing (signed in, players online and in
   matches, or offline and why), and what is going on online (the search and its time, the match, the series' score,
   the lobby). PLAY starts the player's own game (`MatchController::startPlayerGame`): `SC5_NET=1` and `SC5_MENUS=1`
   on the player's own save (`<game folder>\user`) with a session folder of its own (PROTOCOL.md 3.1 and 3.8); with the
   game running the button reads SHOW THE GAME and brings its window to the front. When the game cannot start (not
   built yet, the wrong title update, a release that changed it) a large notice over the page says why, with an OPEN
   BUILD GAME button. An update the server offers shows here as a card with its button (below, *Updates*). A launcher
   that never signed in on this PC opens the account window by itself the first time it is connected, on *create
   account*; "remember me" keeps a 30-day login token.
2. **Build game** (`BuildPage`, `X360.cpp`, PROTOCOL.md 3.7): (1) the disc image (`.iso`, XGD2/XGD3 or XISO),
   (2) title update 3 (`TU_174Q21T_0000008000000.00000000000O2`, the game's last update, docs/TARGET.md), (3)
   optionally the player's own DLC packages, then *CHECK AND BUILD*. The launcher reads the disc's file system and
   the Xbox Live packages itself (no Python): the disc's `default.xex` must be the World release's (SHA-1
   `dfe4d5e1...`, version 0.0.0.2), the update must be a `LIVE` title update of title `4E4D083D` whose
   `default.xexp` (SHA-1 `e8f8eb26...`) patches 0.0.0.2 into 0.0.3.2 (title update 1, `47a09662...` into 0.0.1.2,
   which the game was built on until 0.1.5, is named and refused: a large window says it is title update 1, that SCV
   Online now needs title update 3 and that file's name, and its *Choose title update 3 now* button opens the file
   picker and builds once a file is chosen; while the update in place is not title update 3, a large notice over the
   page says the same, and neither the compile step nor the game starts), a DLC
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
3. **Settings**: **Server** (address `host:port` and the fingerprint the server's host gives out, *Connect*; with an
   empty fingerprint the launcher shows the presented one and asks whether to trust it), **Online** (region, max
   ping, input delay (*Auto (from the ping)*, the default since 0.1.4: the server picks the match's delay from the
   ping measured at its start, 1 frame under 67 ms and a frame more every 33 ms above, 6 at most, the same in both
   games; or 0-4 frames, played when larger than the other player's), the game UDP port (7001), strict ranked, the match
   found chime (heard over the game when it shows MATCH FOUND), UPnP, *Hide my IP address from opponents* and *Even
   when the ping through the server is high*),
   **Display**: the window (windowed, borderless, fullscreen), its resolution, the internal resolution (1280x720 as
   on the console, 2x, 3x or 4x), anti-aliasing (off, FXAA, stronger FXAA), texture filtering (the game's own or 2x
   to 16x anisotropic), v-sync (on; on with a G-SYNC or FreeSync monitor; off), brightness and frame data in
   practice (a frame meter as Street Fighter 6's over Training and Online Practice, F7 in the game hides it;
   GAME.md). Each changes only how a frame looks, never what the game computes, so two players may set them
   differently in one match; they reach the game as `SC5_*` variables and `display.txt`, which a running game follows
   (PROTOCOL.md 3.1). **Keyboard** (`KeyboardBox`, for a player without a controller): which keys press each button
   of the Xbox 360 controller, two at most (click one and press the new key, Esc cancels, *No key* clears it), and
   whether the keyboard plays at all, saved at each change to `controls.toml` (`SC5_CONTROLS`), which a running game
   reads again within a second. **Game folder**: folder, executable and build script. **Tools and development** (a
   source build): Python, the repository, the local test's ports. The mouse wheel scrolls the page; a list or number
   box changes with it only once it was clicked.
4. **Support** (`SupportPage`): where to follow ImTheBlindGuy: YouTube (`youtube.com/@lmTheBIindGuy`) and Twitch
   (`twitch.tv/imtheblindguy`), each with a button that opens it in the browser.

**What went with 0.1.8.** The Ranked, Casual, Lobby, Single player, Leaderboard and Controls pages, the MATCH FOUND,
rematch and Create lobby windows, and the second game a search or a lobby used to start (hidden, on a fresh save),
practice while a search waits and single player set aside for a match (standby): there is one game now, the player's,
and its menus do all of that. The Settings page's sound levels went too (the game's own Sound options set them; the
launcher passes no `SC5_VOLUME_` variable, so the game plays at its own levels), and so did the Controls page's *In a
fight* button combinations (the game's own Options set what the buttons do; the launcher takes an older version's
`[fight]` out of `controls.toml`) and its PC controller section (its `[buttons]` stay in the file as they were).
The preferred side is chosen in the game (its ask `side`; the launcher keeps it as `display/preferred_side` and passes
it on as before). A settings file of 0.1.7 or older works as it is: the keys nothing reads any more (`display/volume_*`,
`online/practice_while_waiting`, `online/search_again`, `online/pick_in_game`, `online/koth`, the queue pages'
characters under `ui/`) are taken out at the first start (`cfg::dropRemovedSettings`, logged), and a start page of
before (`ui/page=ranked`, say) opens Play.

**The game's online menus** (`MenuBridge`, PROTOCOL.md 3.8). While the player's game runs, the launcher writes
`ui.json` into its session folder whenever anything in it changes (within 50 ms) and once a second: the account, the
server's counts and notice, the preferred side, the search (`queue`: searching with its time and the raised ping
limit, MATCH FOUND with the seconds left to accept, accepted, connecting, starting), the series against the present
opponent (a ranked set's score to 3, a casual series', a lobby's wins), the open lobbies, the lobby the player is in
with its members and one sentence of what happens next (who must get ready, the countdown, who cannot play whom by
their versions, a private lobby's password for its host), the leaderboard, one message (a match's result, a set won,
an ask refused and why) and the update offered. It takes `ask.txt` every 100 ms and does what each line asks through
the code the pages' buttons called before: `MatchController::findMatch`, `cancelSearch`, `accept`, `decline` and
`seriesReady`, and the lobby, ready, leave and leaderboard messages to the server (with a search running cancelled
first, an older server's `busy` answered with a leave and the join again, and the version check before Ready). Every
ask is safe in any state: one that does not fit is left undone with a message saying why. A match found plays the
chime and brings the game's window to the front; the match starts in the same game with `start` and ends with `park`
(3.3), a series' next match from the result screen with `rematch`, and a lobby's matches to watch play in it too.
When something online needs the game and it is not running (a lobby match that comes, a search asked for without it),
the launcher says so and starts nothing. When the game closes or crashes, the launcher leaves the search, the set or
series and the lobby the player was in, and reports a match it was in as left. The launcher never closes the game by
itself: a game that does not come back to its online menu after a match is parked again and the player told to close
it and press PLAY if it stays stuck. Closing the launcher asks first while the game runs, and closes the game with it.

**The controller in the launcher** (`GamepadNav`, SDL3): only while a launcher window is in front, since the game
reads the controllers in the background too. The d-pad or left stick moves the focus (gold) to the nearest control,
A clicks, B is Esc in a dialog, LB / RB step the pages, START presses PLAY on the Play page. While a match is
connecting, played, reported or watched the controller does nothing in the launcher. `SC5_PAD_ANY_WINDOW=1` takes any active launcher window as in front (tests
on Qt's offscreen platform with an SDL virtual gamepad).

**Crash reports**: a game that ends with an exception's exit code, or whose log ends with the game's crash line, is
reported to the server (PROTOCOL.md 1.13) with what was going on. **Desync reports**: a match whose games went out
of step is reported with what the game logged of it (1.13.1): Windows, the processor, the game's executable, the
match's lines from the link's `start` line (its setup, the last statistics, the last 30 lines before the failed
check and every `sc5 desync:` line the game writes after it), and the extracted disc's files (count, bytes, a hash of names and sizes
and, once hashed in the background, of their contents; `game-files-sc5.txt` beside the settings keeps the hashes).
**F3 reports** (`ReportSender`, 1.13.2): F3 in the game saves a capture under the session (`SC5_CAPTURE_DIR`); the
launcher sends its pictures, textures, draws and the end of the game's log to the server, which keeps them under
`/var/lib/sc5/reports/<day>/`. **How a game ended**: a game that ends during a match reports `left` with `exit`
and `exit_code`, from the last lines of its log.

**Updates** (`Updater`): a player package says which build it is (its `VERSION.txt` stamp, `hello`'s `build`),
so a test build that carries a release's number is offered that release when the update file names the release's
build (PROTOCOL.md 1.3). The server names the latest release (`welcome.update`, an `update` push, the
`client_outdated` reply). The Play page shows it as a card with its button, and `ui.json`'s `update` tells the game,
which says so in its menus; while the game runs nothing comes over it, and the offer's window (UPDATE NOW, LATER, the
notes and the release page) opens by itself once the game is closed, or at once from the card. UPDATE NOW downloads the
zip into `update\` beside the launcher, checks its size and SHA-256 against the
server's (whose certificate is pinned; the download host is not trusted), unpacks it, checks its `VERSION.txt`
(`SC5-Online <version>, ...`) and swaps the release's files in once the game is closed (each old file renamed to
`<name>.sc5-old` and deleted at the next start); then the new launcher starts with `--updated <old version>`, and
when the release changed the game (`games\sc5\built.txt`, which `build-game.cmd` copies from `project\VERSION`,
names another release) it builds the game again on Build game before anything starts it (`GameProcess::needsRebuild`,
as MK9 Online's launcher; `GameProcess::start` refuses such a game until then). When the title update in place is not
the one the new release is built on (an install of 0.1.5 or older, built on title update 1), it opens Build game,
says in its banner that SCV Online now needs title update 3, and the compile step asks for that file instead of
building from the old one (PROTOCOL.md 3.7). A source build only links to the release page.

### Without the game

Until `sc5.exe` speaks PROTOCOL.md section 3, point the game's executable at `launcher/tools/fake_game.py`
(Settings, *Game folder*; the launcher runs `.py` files with the configured Python, without a window). The stand-in
goes `booting` -> `ready`, takes `start`, reports `starting`, binds the start line's `local_port`, exchanges
heartbeats with `remote`, writes `in_match` and after `SC5_FAKE_SECONDS` (default 6) a result computed from the
seed, so both sides agree (Online Practice: `sparring_over`). In a lobby match it writes a spectator feed to the
start line's `feed` (READY, inputs, checks, the frame it left at, under the `SC5R` header); started to watch
(`watch=1`) it reads both players' feed and reports `in_match` and the result from it. Its `game.log` starts with
every `SC5_` variable it was given. `SC5_FAKE_RESULT=desync` makes it report a desync with a log like a real game's.
`SC5_FAKE_ASKS=<script>` makes it the player in the game's online menus too: it reads `ui.json` and writes `ask.txt`
lines as the script says (`wait queue.state=found`, `ask accept`, `ask lobby_join code={lobbies.list[name=X].code}`;
the stand-in's docstring has the rest), and its `game.log` says `asks script: done` or where it failed.

### Command line (development and automation)

The switches that play a part (`--queue`, `--lobby`, `--auto-accept`, `--rematches` and the rest) stand in for the
player in the game's menus: the launcher starts its game first, as PLAY does, and once the game is up and the account
signed in they go through the same asks the game's menus send (`TestDriver`, `MenuBridge::ask`). The one thing the
game's menus do not ask for is the next match of a series: the game's result screen chooses it, its loser's REMATCH,
CHARACTER SELECT or LEAVE (PROTOCOL.md 3.3, *The loser's choice*: the game says `chose`, `MatchController::readyInPlace`
readies the series or the lobby with it, and the loser's launcher alone leaves a series). `--rematches` stands in for
it only where the game offers no choice (the stand-in's is `SC5_FAKE_CHOICE`).

| option | effect |
|---|---|
| `--config <ini>` | use another settings file (two launchers on one PC need two) |
| `--server host:port`, `--fingerprint <hex>` | override and store the server settings |
| `--user NAME [--register]` | sign in (or create) with the password from `SC5_DEV_PASSWORD` (`--password-env` picks another variable) |
| `--play` | start the game once the window is up, as PLAY does |
| `--queue casual\|ranked [--character Siegfried]` | start the game and ask for that search once signed in (the character: a roster id the search goes with; picked in the game without it) |
| `--auto-accept N` | answer a match found with `accept` after N seconds |
| `--cancel-search N` | with `--queue`: `cancel` N seconds after the search starts |
| `--rematches N`, `--rematch-choice rematch,select`, `--leave-series` | with `--queue casual`: choose a rematch after each match, N times, then `leave` or let it run out |
| `--lobby-own-first` | with `--lobby join`: a lobby of one's own first, then the named one joined (the server moves the player; an older server's busy gets a leave and the join again) |
| `--lobby host\|join\|watch --lobby-name NAME [--lobby-code-file F] [--lobby-matches N] [--mode sparring]` | `lobby_create` and ready, find it in `lobbies` (or by the code in F) with `lobby_join` and ready, or join it with `watch=1` |
| `--koth`, `--rate N` | a King of the Hill lobby; rate each match watched, 1 to 10 (straight to the server: the game's menus have no rating) |
| `--leave-lobby`, `--leave-lobby-in-match S` | `lobby_leave` after its matches, or S seconds into the first |
| `--page play\|build\|settings\|support [--snap-dir <folder>]` | start page; with `--snap-dir`, a picture of it |
| `--local-test` | start the two-instance loopback test (two games of their own, no server) |
| `--extract <image>` | open Build game and check and extract that image (the title update and DLC from the settings, `games/sc5/update_package` and `games/sc5/dlc`), then compile; what it did goes to `sc5-launcher.log` (`build sc5: ...`) |
| `--auto-update`, `--updated <version>` | install an offered update without asking (tests); started by an update |
| `--upnp-probe` | find the router and its public address, open nothing |

### End-to-end tests on one PC

`tools\online-e2e.ps1` runs this checkout's server on a throwaway database and certificate, two launchers on Qt's
offscreen platform (no window on the screen) and the stand-in games they start (one each, as PLAY), and prints every
status line, what the launchers did and the database's records. Its settings files turn UPnP off.

```
powershell -File tools\online-e2e.ps1 -Queue casual -Rematches 1      # match found, accept, match, result, rematch, leave
powershell -File tools\online-e2e.ps1 -Queue lobby -Matches 2 -Choices select,select  # the loser's CHARACTER SELECT
powershell -File tools\online-e2e.ps1 -Queue ranked                   # a ranked set, Elo and titles
powershell -File tools\online-e2e.ps1 -Queue lobby -Matches 2 -Watch  # a lobby, two matches, a spectator
powershell -File tools\online-e2e.ps1 -Queue lobby -Mode sparring     # Online Practice
powershell -File tools\online-e2e.ps1 -Queue ranked -Draw -Matches 2  # drawn matches: nobody's point in the set
powershell -File tools\online-e2e.ps1 -Queue lobby -Matches 2 -OwnLobbyFirst  # B moves from its own lobby
powershell -File tools\online-e2e.ps1 -Queue casual -DelayB 3        # input delays: Auto (default) or a number
powershell -File tools\online-e2e.ps1 -Queue asks                    # the stand-in games drive it by asks alone
powershell -File tools\koth-e2e.ps1 -Matches 3 -Snaps                 # King of the Hill, Respect Points, leaderboard
```

The stand-in games' losers choose on their result screens as the real game's do (`-Choices`, the stand-in's
`SC5_FAKE_CHOICE`, one per result in turn): by default a casual series' `-Rematches` rematches and then LEAVE, a ranked
set's CHARACTER SELECT (its winner keeping their fighter, `keep=` in the next line) until it is won, a lobby's REMATCH.

`-Queue asks` starts the launchers with `--play` only: each stand-in game runs a script of asks (`SC5_FAKE_ASKS`)
against its launcher's `ui.json`, as a player in the game's menus would: asks that do not fit (accept with no match,
a second search, Ready outside a lobby) and their messages, a casual match found, accepted and played, the series
left, the leaderboard, then a lobby A creates and B finds in the list and joins, both ready, its match, and both
leaving. It passes once both scripts say `asks script: done`.

The stand-in's results name the fighters as the real game does, by its own character ids with the costume after a
dot (`p1_char=3.0`), so a ranked set's winner keeps theirs in the next start line. `-Exe` runs another game (the real
`sc5.exe` speaks section 3 for versus since 2026-11-17: its `tools\linktest.py` drives two of them through the same
files), `-Visible` shows the launchers' windows, `-Snaps` saves pictures of them. `-Port` picks other ports (the
work folder is then `build\online-e2e-<port>`). The work folders (`build\online-e2e`, `build\koth-e2e`) stay for a
look and are replaced by the next run. `-Stuck` (games that never take `park`) no longer gets its games replaced: the
launcher never closes the player's game, so it parks it again and tells the player.

## Known limitations

- The game's online menus (3.8) have no asks yet for a lobby's chat, for changing places in a lobby (a player stepping
  aside to watch, a spectator taking a free place, King of the Hill's line), or for rating a King of the Hill win:
  the launcher's pages that had them are gone, so those wait for asks of their own. A spectator joins to watch
  (`lobby_join watch=1`) and a King of the Hill lobby still plays, its line and its king as before.
- A series' next match is chosen at the game's result screen (3.3, *The loser's choice*) only when the two games
  reached each other straight; through the relay or the tunnel the launchers go on by themselves with CHARACTER SELECT
  (the winner keeping their fighter), and a player leaves such a series from the game's online menu (`leave`).

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
- A spectator's view ends (the watching game says `disconnected` some 20 s later and parks) when the players' games
  went out of step and played the round again (docs\GAME.md, "The round again"): the spectators' feed ends there.
- A KO that only a wrong guess had and a rollback took back leaves its call on the screen ("PERFECT", "K.O.") over the
  round that goes on, until the round's real end; the round markers are right (docs\GAME.md, "The HUD's round
  markers").
- Created characters, original ones and customized fighters, come from the offline save, read only
  (`SC5_CREATION_SAVE`, docs/PROTOCOL.md 3.3). One that the other PC's check would refuse is left out of the online
  select screen (a customized fighter is played in one of its fighter's own two costumes), and the main menu's
  Characters Created count stays 0 online.
- Results are self-reported: agreement counts, disagreement voids the match, a single report counts for nothing.
- One server process with in-memory queues and lobbies; a restart drops searches and matches in progress.
- The launcher is Windows only and not code-signed.
