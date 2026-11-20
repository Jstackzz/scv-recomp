# SCV Online: plan

Soulcalibur V running natively on Windows, built by each player from their own Xbox 360 disc, with a
launcher that has MK3D Online's features (all but the jukebox, for now) and rollback netplay. Same shape as
MK3D Online (`source/repos/mk3d-rollback`, the launcher and server) and MK9 Online
(`source/repos/mk9-rollback`, the ReXGlue game side), themed for Soulcalibur.

Nothing from the game is in this repository, ever: no code, no data, no generated C++.

## Target

| | |
|---|---|
| Disc | Soulcalibur V (World), Xbox 360, Redump SHA-1 `43dbb40b9ef4e5a651623ada40ed4eb2066017d5` |
| Title | `4E4D083D`, media `165FAD4F`, disc `default.xex` version 0.0.0.2, SHA-1 `dfe4d5e1e9198bc9df6db46ddf2bdf2a5cbf3f80` |
| Title update | TU1: LIVE package `TU_174Q21T_0000008000000.0000000000082`, `default.xexp` (delta 0.0.0.2 -> 0.0.1.2, SHA-1 `47a09662fd3fe376d283208fcd8a31eee819c736`) and `patch.cpk` |

Details and how they were checked: [TARGET.md](TARGET.md).

## How the update is applied

The ReXGlue runtime loads `game:\default.xex` and, if a sibling `default.xexp` exists, applies it
(`src/system/user_module.cpp`, as Xenia does). Codegen loads the executable through the same runtime
(`src/codegen/codegen_context.cpp`), so with `default.xexp` beside `default.xex` the generated code *and* the
data the game runs on are both the updated 0.0.1.2 image. The update's other file, `patch.cpk`, goes in the
folder mounted as `update:`. The builder checks the disc and the update by hash before it starts and refuses
anything else (another region, print or update), as the Tekken Tag 2 installer does.

DLC: free content (the Cepheus compatibility packs that show other players' DLC parts) on for everyone; paid
DLC only from the player's own Xbox 360 DLC packages. Never unlocked for everyone.

## Pieces

| Folder | What | Template |
|---|---|---|
| `sc5/` | ReXGlue project: manifest, codegen config, `sc5.exe` app, the game-side code (frame hook, input, netplay, snapshots, determinism, sound boundary, autopilot to the select screen, launcher link, names, practice, spectator feed, crash/hang reports, captures, display) | `mk9-rollback/mk9` |
| `launcher/` | Qt 6 launcher, MK3D Online's feature set without the jukebox, Soulcalibur theme, Build game for an Xbox 360 image + title update (+ the player's DLC) | `mk3d-rollback/launcher` (HEAD) |
| `server/` | matchmaking server, game id `sc5`, its own instance (MK3D's server has one version gate and update file for every launcher) | `mk3d-rollback/server` |
| `tools/` | disc/STFS/XEX tools (`tools/py/x360.py`), packaging, test scripts | both |

Launcher <-> game: MK3D's file protocol (PROTOCOL.md section 3: `cmd.txt` start/park/quit, `status.txt`
booting/ready/starting/in_match/result/desync/disconnected/error, a feed port for spectators) with an `SC5_`
prefix.

## Graphics settings and online play

Two players' games compare their fight state, so a setting may change only how a frame looks, never what the
game computes:
- Safe (launcher Display page): internal resolution (scale below or above 720p), anti-aliasing, texture
  filtering, V-sync, window mode, frame-rate cap for menus, brightness. They live in the GPU emulation, which
  the game does not read back.
- Not offered: anything the game itself decides (its effect, shadow or particle counts) unless it is shown that
  the fight never reads it. MK9 found GPU answers that game logic read (occlusion queries) desynced matches;
  SCV gets the same fixed answers on every PC from the start.

## Milestones

1. **Recompile and boot**: codegen of the patched image, build, `sc5.exe` reaches the main menu. *Done
   2026-11-16 (docs\GAME.md, "What it took to run").*
2. **Play offline at full speed**: Versus fights at a steady 60 fps on a mid-range PC; graphics settings. *Versus
   plays whole matches (rounds, KO replays) at a steady 60 fps on this PC (RTX 5080), 2026-11-16; the graphics
   settings 2026-11-17 (the launcher's Display page). Left: hitches of 70-210 ms while screens load, a mid-range PC.*
3. **Determinism gate**: two instances fed the same recorded inputs end a fight with identical state hashes. *Done
   2026-11-17 (docs\GAME.md, "Determinism").*
4. **Rollback on one PC**: two instances, GGPO-style session at the frame hook, forced-rollback stress. *Done
   2026-11-17: every frame put back 4 frames and run again, the fight the same byte for byte; two games with up to
   150 ms each way in sync, a spectator too (docs\GAME.md, "Rollback", "Netplay").*
5. **Launcher**: MK3D feature set against `fake_game.py`, then the real game (hold point, start line, park). *Done
   2026-11-17 (docs\LAUNCHER.md; docs\GAME.md, "The launcher's game").*
6. **Server + two-PC tests**: own instance (needs the owner's go-ahead to deploy), real matches. *The server runs on
   the Oracle Cloud machine and answers from outside (TCP 7797, UDP 7798), 2026-11-17. Left: matches between two PCs
   over the internet.*
7. **Player package**: zip with the launcher, the prebuilt runtime and toolchain; the player's PC compiles
   only the game's code. *Done 2026-11-17 (`tools\package.ps1`; docs\GAME.md, "What a player's PC needs to build
   the game").*

## Toolchain (this PC)

VS 2026 Build Tools (MSVC 14.50), clang from `mk9-rollback/external/llvm`, CMake + Ninja from VS, ReXGlue SDK
v0.10.0 with MK9's patch (`mk9-rollback/dist/sdk`, later its own copy under `external/`), Qt 6.12.0
(`mk3d-rollback/external/qt`), Python 3.13. Game files: `game/` (git-ignored).
