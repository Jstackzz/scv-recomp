# SCV Online

Soulcalibur V (Xbox 360) running natively on Windows PCs, with online play and rollback netcode: Ranked and
Casual matchmaking, lobbies with spectators and King of the Hill, Online Practice, single player, a leaderboard, and
controls you remap in the launcher.

The game's code is not in this repository and never will be. As in MK9 Online, every player builds their own copy on
their own PC from **their own disc**: the launcher reads your Xbox 360 disc image and title update, translates the
game's PowerPC code to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiles it with
clang. Nothing from the game (code, data or generated C++) is ever committed here.

## What you need

- **Soulcalibur V, Xbox 360**, as a disc image (`.iso`; Redump SHA-1 `43dbb40b9ef4e5a651623ada40ed4eb2066017d5`).
  Every Xbox 360 copy, from any region, is this same World disc. A modified copy is refused, since both players of a
  match must run the same code.
- **Title update 1**: the Xbox Live package `TU_174Q21T_0000008000000.0000000000082` (from your console).
- **Your own DLC**, if you have it: your Xbox 360 DLC packages. Paid DLC is only ever yours: nothing here unlocks it
  for anyone else. If you bought the DLC on PS3, `tools/py/ps3dlc.py` turns your own packs and licenses into Xbox 360
  packages, which you add on Build game with any 360 ones (docs/GAME.md, "Unlocks and downloadable content"). For the
  costume packs (Active Wear, Bridal, Dark Warrior, Maid, Skeleton, Tekken and the rest) it also needs their free
  "Compatibility Pack" downloads, whose models and textures it converts to the Xbox 360's formats on your PC (about a
  minute; the packages then take up to 120 MB each).
- Windows 10 or 11, 64-bit.

[docs/TARGET.md](docs/TARGET.md) lists every hash the launcher checks.

## Playing

1. Download `SC5-Online-<version>-windows.zip` from [Releases](../../releases) and unpack it somewhere you can write
   to (Documents, say). It has everything: the launcher, the ReXGlue runtime and the compiler that builds the game.
   Nothing else gets installed.
2. Start `SC5Launcher.exe` and open **Build game**: pick your disc image, the title update and (optionally) your DLC
   packages, then **CHECK AND BUILD**. The launcher checks each one, extracts the disc and compiles the game's code
   for your PC. That happens once and takes a few minutes.
3. Sign in (top right) and play: **Ranked** or **Casual**, **Lobby** (1, 2 or 3 rounds to win, King of the Hill, Online
   Practice, watching), or **Single player** (the game's own modes, Training with every character, and two-player
   versus on one PC).

The launcher updates itself from this repository's releases; when an update changes the game, it builds the game
again by itself before you play.

## What works

- **Rollback netcode** built on the game's own battle snapshot: a late input is guessed, and when the guess was
  wrong the game puts the fight back and runs the frames again unseen. Tested on one PC with lag added: every frame
  put back 4 frames and run again with the fight the same byte for byte, and matches at 100 and 150 ms each way in
  sync at a steady 60 frames a second, a spectator too. Sounds stay as you heard them through a rollback: none
  start twice and a wrong guess's are stopped. How it works and how it was tested: [docs/GAME.md](docs/GAME.md#rollback-battlecpp-netsynctestcpp).
- **A match that goes out of step is not lost**: the two games check each other every half second. If a check ever
  fails, both go back to the start of the round being fought, make it the same again, and you fight that round again
  (the screen says OUT OF SYNC, THIS ROUND STARTS AGAIN); the match and its score go on. Each game's log then names
  exactly what differed, and the launcher sends it to the server, so the cause can be found and fixed.
- **Every mode online**: versus (Ranked and Casual, first to 3 rounds as on the console, and lobbies), Online
  Practice (no timer, health refills), spectators, King of the Hill with Respect Points, and Training offline while
  you wait for a match.
- **Everything on the disc unlocked** in every mode, single player's PLAY included (the hidden characters among
  them), without touching your save. Dampierre and the DLC parts come only from your own packages; an online match
  allows Dampierre only when both players have him.
- **No mouse pointer over the game**: it takes no mouse input. Over its title bar and outside it the pointer shows
  as usual.
- **Display settings that cannot desync a match**: window mode and size, internal resolution (720p as on the console
  up to 4x), anti-aliasing, texture filtering, V-sync, brightness. Each changes only how a frame looks, never what
  the game computes, so two players may pick differently. Music, effects and voices levels apply at once.
- **Rematches in the game**: after a match the game stays at its result screen. Casual offers REMATCH (the game's
  own Rematch: the same characters on the same stage, straight back into the fight) and CHARACTER SELECT (both pick
  again); a ranked set goes on by itself, its last match's winner keeping their fighter while the loser picks again.
  The next match starts from there, without closing the game. In a lobby the last match's loser chooses REMATCH or
  CHARACTER SELECT themselves, with their own controller, on the game's result screen (player 1 after a draw), and
  the other game follows.
- **The way into a fight and the KO replays play**, as on the console: the VS screen (both fighters on their cards),
  the stage's fly-over, the fighters' intros and the replay of a KO. Either player skips them with START (a replay
  with A too), and both games and the spectators skip them together.
- **The players pick the stage** on the game's own stage select, with their own controller: player 1 for the first
  match, then the loser of each match, while the other game follows. Random draws the same stage in both games, and
  no pick within a minute counts as Random. REMATCH keeps the stage.
- **Created characters online**: a player's own created characters (made in Creation in the offline game) are on the
  select screen online: original characters under the Creation slot, customized fighters (a regular fighter re-dressed
  in Creation) among their fighter's costumes. The other player's game gets the character's record and checks every
  part of it against its own game (parts, models, stickers, colours, weapon, limits) before anything loads it; one
  with downloadable parts only plays when both players have the same ones, and then fights wearing them. A ranked
  winner keeps theirs. The picture taken in Creation stays on its own PC: on the other, the VS screen shows SCV
  Online's own card for that character.
- **Your own button settings everywhere**: the launcher's Controls page sets what each button presses in a fight, any
  combination of A, B, K and G on any button (the game's own settings offer 13 of the 16 and nothing on the stick
  clicks), in every mode. Online the other player's game sees your presses as you set them, whatever theirs are.
- **The launcher**: accounts, MATCH FOUND with the opponent's connection, ranked sets with 16 Soulcalibur rank
  titles, a leaderboard, controls for keyboard and any controller, self-updates, and crash, desync and F3 (picture)
  reports to the server. Players who cannot reach each other play through the server's relay.

## Not done yet

- Few matches have been played between two PCs yet (the first ones found the throws' desync, fixed in 0.1.2; a first
  to 30 on 0.1.3 found the stages' effects and the match after a park, fixed in 0.1.4); most tests ran on one PC with
  lag added, one game's memory moved to stand in for a second PC.
- A spectator's view of a match ends when its players' games play a round again after going out of step.
- Screens can hitch for 70-210 ms while they load. Only tested on a fast PC so far.
- More in [docs/LAUNCHER.md](docs/LAUNCHER.md#known-limitations).

## Building from source

Windows with Visual Studio 2026 Build Tools (MSVC, CMake, Ninja), LLVM clang (`SC5_LLVM`, or `external\llvm`),
Python 3.11+, and Qt 6.12.0 (MSVC 2022 64-bit) in `external\qt` for the launcher.

```
rem 1. The ReXGlue SDK v0.10.0 with this project's patch (docs/SDK.md)
git clone https://github.com/rexglue/rexglue-sdk external\rexglue-sdk
git -C external\rexglue-sdk checkout v0.10.0
git -C external\rexglue-sdk submodule update --init
git -C external\rexglue-sdk apply ..\..\patches\rexglue-sdk-v0.10.0.patch
tools\build-sdk.cmd

rem 2. Your game files: the extracted disc in game\disc with the update's default.xexp beside default.xex,
rem    and patch.cpk in game\update (the launcher's Build game lays them out too)
sc5\codegen.cmd
sc5\build.cmd release

rem 3. The launcher
launcher\build.cmd

rem 4. A player package (dist\SC5-Online): sc5.exe without the game's code, the runtime and the toolchain
tools\build-player-host.cmd
powershell -Command ". tools\package.ps1; New-PlayerPackage"
```

The server is Python with the standard library only: `python -m server.makecert`, then
`python -m server --port 7777 --udp-port 7778` ([docs/LAUNCHER.md](docs/LAUNCHER.md#server) has the rest, tests and
hosting).

## Documentation

| | |
|---|---|
| [docs/GAME.md](docs/GAME.md) | the game side: what it took to run, unlocks and DLC, the frame, determinism, rollback, netplay, sound, the launcher link |
| [docs/LAUNCHER.md](docs/LAUNCHER.md) | the launcher and the server: building, running, testing, hosting |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the server's protocol, the UDP formats, and the launcher <-> game contract |
| [docs/SDK.md](docs/SDK.md) | the ReXGlue SDK and this project's changes to it |
| [docs/TARGET.md](docs/TARGET.md) | the exact disc and title update |
| [docs/PLAN.md](docs/PLAN.md) | the plan and its milestones |
| [sc5/player/README.md](sc5/player/README.md) | how a player's PC builds the game |

## Thanks

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (Tom Clay) and the [Xenia](https://xenia.jp) project it
builds on; MK3D Online and MK9 Online, whose launcher, server and game side this project started from; GGPO's and
Timestone's ideas about rollback; ps3recomp's PPU disassembler (MIT); Qt, SDL3, LLVM and Ninja.

Soulcalibur is a trademark of Bandai Namco Entertainment Inc. This project is not affiliated with or endorsed by
Bandai Namco or Microsoft. You need your own copy of the game. Project code: MIT ([LICENSE](LICENSE)).
