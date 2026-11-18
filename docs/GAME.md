# SC5 on PC: the game side

Soulcalibur V (Xbox 360, title 4E4D083D, `default.xex` 0.0.0.2 with title update 1, docs\TARGET.md) recompiled
to native x64 code with the ReXGlue SDK (docs\SDK.md). This page covers what the game executable (`sc5\`) adds on
top of the recompiled code, and what the game itself does that matters for it. Every address is for the TU1 image
(`orig\analysis\default_tu1.bin`, written by `tools\imagedump`).

## Build and run

```
sc5\codegen.cmd                  # the game's code -> sc5\generated\default (also run by the build when a config changes)
sc5\build.cmd release            # -> sc5\out\build\win-amd64-release\sc5.exe (+ the SDK's DLLs)
```

The game reads its disc files from `SC5_GAME_DIR` (default `<exe>\game\disc`: the extracted ISO with the title
update's `default.xexp` beside `default.xex`), the title update's other files (`patch.cpk`) from `SC5_UPDATE_DIR`
(`<game>\..\update`) and keeps saves in `SC5_USER_DIR` (`<exe>\user`). Always run it windowed while testing:
`--fullscreen=false --window_width=960 --window_height=540`. The game takes no mouse input and shows no mouse pointer
over its window (`sc5_app.cpp`, the SDK window's hidden cursor, as MK9 Online); over the title bar and outside the
window the pointer shows as usual.

## What a player's PC needs to build the game

Nothing but the package: nobody ships the game's code, each player's PC translates and compiles its own from the
player's disc and title update ([sc5/player/README.md](../sc5/player/README.md) says how, as MK9 Online does it). The
game is two parts: `sc5.exe`, everything of ours, built at release without the game's code (`tools\build-player-host.cmd`,
the CMake option `SC5_PLAYER_HOST`), and `sc5game.dll`, the translated code alone, built on the player's PC by
`games\sc5\build-game.cmd` (the launcher's Build game, docs\PROTOCOL.md 3.7): codegen of the player's `default.xex`
with `default.xexp` beside it, then the package's ninja, clang and lld. Host code takes the image from
`sc5::GameImage()` (`src/sc5/gameimage.h`), never from `PPCImageConfig` or `PPCFuncMappings`, which the ready-made
`sc5.exe` cannot link. `tools\package.ps1` (`New-PlayerPackage`) lays the package out:

| part | from |
|---|---|
| `SC5Launcher.exe` + Qt | `build\launcher` (`launcher\build.cmd`) |
| Microsoft's C++ runtime DLLs | Visual Studio's `VC\Redist`, beside the launcher and in `games\sc5\sdk\bin` |
| `toolchain\` | clang, lld-link and clang's headers from `SC5_LLVM` (as `tools\devenv.cmd`), ninja from Visual Studio's CMake |
| `games\sc5\host\sc5.exe` | `tools\build-player-host.cmd` -> `dist\host` |
| `games\sc5\sdk\` | `external\sdk`: the runtime, the codegen tool, the few SDK headers the game's code compiles with |
| `games\sc5\project\` | what codegen reads: `sc5\config`, the manifest pointing at `project\xex`, the release's `codegen.partition.json` |
| `games\sc5\player\` | `sc5\player`: stand-in headers, `game_stub.cpp`, import libraries, `game_link.rsp` |
| `games\sc5\build-game.cmd`, `build.ninja` | `tools\player\build-game.cmd`, written by `tools\player\make_build_ninja.py` |
| `LICENSES\` | the repository's `LICENSE`, the ReXGlue SDK's and SDL3's, `launcher\licenses` (Qt) |

Tested 2026-11-17 with a PATH of Windows only and no compiler environment, in a folder named `Jeux & Démos\SC5-Online (1)`
(`tools\player\test-clean-build.ps1`, `check_package.py`, `test-boot.ps1`): codegen 6 s, the compile 25-30 s on 16
threads, a run with nothing changed under a second; `sc5game.dll` (37 MB) takes every hook from `sc5.exe` (55 functions:
25 game functions, 22 midasm hooks, 7 Xam imports, the trap log), and the built game, AVX2 and SSE4 builds, reached the
main menu (`ready`) in 41 s.

## Source map (`sc5/src/sc5`)

| file | what |
|---|---|
| `frame.cpp` | the frame boundary (below): frame count, main thread, top/end hooks, the frame rate in the log every 600 frames |
| `input.cpp` | XamInputGetState / GetCapabilities(Ex) / GetKeystrokeEx answered from one source chain: override > source (test scripts, later netplay) > the PC's controllers |
| `testinput.cpp` | scripted pads for tests: `SC5_INPUT_P1..P4`, `SC5_PADS`, `SC5_NO_PHYSICAL` (testinput.h) |
| `system_ui.cpp` | system message boxes the game raises in the normal course of things, answered for the player (below) |
| `guards.cpp` | checks inserted into the game's code where it reads through an empty pointer (below) |
| `unlocks.cpp` | everything on the disc unlocked without touching the save (`SC5_UNLOCK_ALL=0` turns it off): below |
| `game.cpp` | what the game is doing: its sequence (screen), the battle's state, the fight clock; the random seed; test hashes (below) |
| `battle.cpp` | the battle saved and put back: the game's own snapshot plus the memory it leaves out (below, "Rollback") |
| `rng.cpp` | the game's random number generators; the visual systems' draws kept apart from the fight's (below) |
| `net/netplay.cpp` | online play for two games over UDP with rollback (below, "Netplay") |
| `sound.cpp` | a rollback's sounds reconciled with the first run's: none started twice, a wrong guess's stopped (below, "Netplay"); the launcher's music, effects and voices levels (below, "Sound levels") |
| `dlc.cpp` | the player's own downloadable content where the SDK finds it (below, "Unlocks and downloadable content") |
| `link.cpp` | the launcher's side of docs\PROTOCOL.md 3: cmd.txt, status.txt (below, "The launcher's game") |
| `autopilot.cpp` | the menus from boot to the hold point, to a VS fight with the match's picks or to Training, and back; the plain games' way to their select screen |
| `sparring.cpp` | Online Practice: the round's clock held, health back up, at the top of every frame in sync (below) |
| `controls.cpp` | the launcher's `controls.toml`: the keyboard as player 1, every controller's buttons, player 2's controller |
| `display.cpp` | the launcher's display and sound settings at the start and from `SC5_DISPLAY_FILE` as it changes |
| `rerun_profile.cpp` | tests (`SC5_TEST_RERUN_PROFILE`): the game's tasks timed one by one in frames run again (below, "Netplay") |
| `select.cpp` | the character select screen: cursors, picks, the 32 slots and their characters |
| `versus.cpp` | a VS fight's setup online: rounds to win, player 1's settings and random numbers, player 2 on the second pad |
| `net/synctest.cpp` | the rollback check on one PC (`SC5_SYNCTEST`, below) |
| `debug_watch.cpp` | diagnostics (temporary): hardware write watches, the whole-memory diff, memory peeks, the two-player test switch |
| `users.cpp` | a second signed-in local profile for the offline versus (`SC5_VERSUS=1`, tests `SC5_TEST_USER2=1`): VS Battle's Player VS Player |

## What it took to run

- **Black screen at boot**: the SDK cleared 32 bytes for `dcbz`, the console clears its 128-byte cache line; the game
  clears memory with `dcbz; addi rX,rX,128` loops and three quarters of every line kept old data (SDK patch, SDK.md).
- **Missed functions**: callbacks and thunks only reached through code pointers, `config\sc5_indirect_funcs.toml`
  (`tools\py\find_indirect_funcs.py`: vtables, callback tables and addresses built with lis/addi in code).
- **Address 0**: a parameter getter reads guest 0 on purpose; the zero page is mapped readable (`protect_zero` off).
- **Title screen waiting forever after START**: its user manager (`[0x82927A54]`, a record of 104 bytes per user
  from +0x10) watches each user's sign-in with a 60-frame countdown (+0x50) that must rest at 0 before the title
  goes on (`sub_8232FAD8` state 5). It compares the XUID it stored with XamUserGetSigninInfo's online (flag 1) and
  offline (flag 2) XUIDs; the SDK returned the same XUID for both, so a local profile looked like it had changed and
  the countdown restarted for ever. The SDK now returns no online XUID for a profile not signed in to LIVE.
- **The first START asks to create save data** (a system message box, "No save data found ... [Create Save Data]
  [Select Storage Device]", raised by `sub_824906D8(task, 2, 14, 18)` from 0x824911C4 in the save task): answered
  with Create Save Data (system_ui.cpp, by the call site, so in every language). The save lives in
  `<user>\B13EBABEBABEBABE\4E4D083D\00000001\SC5SaveData`.
- **Quit with a bad disc screen at the main menu**: the game looks up XamMediaVerification* by ordinal and checks
  that its disc reads back whole (`sub_823260B8`); the SDK's stubs returned garbage. Implemented as a check that
  found nothing (SDK patch).
- **Crash as a fight starts**: a battle object's state save reads its owner's field with no owner (guards.cpp).

## Unlocks and downloadable content

**The player's own packages** (dlc.cpp, 2026-11-17). Build game, or `python tools/py/x360.py dlc-install <package>
game/dlc`, lays a package out as `game\dlc\<package>\` with its files and `game\dlc\<package>.header` (its header).
At startup the game copies each into the saves where the SDK's content manager finds marketplace content
(`<saves>\0000000000000000\4E4D083D\00000002\<package>\`) and writes the SDK's record for it beside
(`...\Headers\00000002\<package>.header`: the content record, 0x148 bytes, the package's own display name, then a
license mask of every bit), and takes out what is not in `game\dlc` any more. Dampierre's package (LIVE,
"DLC01:DAMPIERRE", one 53-byte file, `index_01_X360.dat`) puts him on the select screen. Nothing else unlocks paid
content.

**A 360 DLC package is a key.** Everything it grants is on the disc; the package holds one index file,
`index_NN_X360.dat`, read by `sub_82388990` (header) and `sub_82388760` (entries), all big-endian:

    u32 version (1, never 0)   u32 ids (all entries')   u32 strings size, the strings (360: one zero byte)
    u16 slots   u16 entries    each entry: u16 group, u16 slot (< slots), u8, u16 n1 + n1 u16 (a set: its items'
    slots), u16 FFFF, u16 0 (or a kind and number for an unlock that is not a release id), u16 n2 + n2 u16 (release
    ids), u32 a string offset (0), u8 flag, u64 offer (0), u32 license bits (1: bit 0 of the package's license mask)

The DLC manager (table 0x8276F430: state at +8, at +40 an array of 30 package objects of 344 bytes, their count at
+44) names its objects DLC00 to DLC29 and takes an installed package into the object whose name its display name
contains (`wcsstr`): "DLC01:DAMPIERRE" is DLC01. An entry is granted when its license bits meet the package's license
mask (the object's +340; bits 0: always), and `sub_82385F48` (once the table's state is 7 to 11) finds a release id
among the granted entries' n2 ids.

**The player's PS3 DLC** (`tools\py\ps3dlc.py`, 2026-11-17). The PS3 game's packs are the same keys: a group's index
(`USRDIR/DLCnn/index_nn_PS3.edat`) has the same entries, with a PS3 tail (the content id's and the license file's
string offsets) and, in the free catalog packs of DLC06 to DLC13, a version-2 header (a leading zero word); a pack
bought adds `license_NN.edat`, which only the buyer's license (`exdata\<content id>.rap`) opens, and the index's entry
for the pack (a set, with its items' slots in n1) names that license file. The tool opens the player's licenses, takes
from each group's index the entries they grant (a set and its items, a music track), and writes one 360 package per
group, `game\dlc\PS3-DLCnn\index_nn_X360.dat` with `PS3-DLCnn.header` (display name "DLCnn:PS3 ..."); a group with a
360 package of the player's own is left to it. From the European Dampierre pack the tool writes the 360 Dampierre
index byte for byte. The catalogs are opened with the game's own key, which the PS3 game builds at run time through a
substitution table (`key[i] = table[source[i]]`, stored at +0x328 of its DRM object, both tables through its TOC);
`--eboot` reads it from the game's executable decrypted by RPCS3 (`rpcs3.exe --decrypt EBOOT.BIN`). With the
owner's 37 US and 41 European packs: 11 packages (DLC02 to DLC13 but 05, the US-only Dampierre pack whose license is
missing), 361 release ids; the game's own check (`SC5_DBG_DLC=1`, below) grants every one tried. Only DLC equipment for
created characters, music and Dampierre: nothing a versus pick on the select screen changes but Dampierre, whom an
online match allows only when both players have him.

The game keeps a release state for each of 7426 unlockable things (characters, stages, music, creation parts,
titles) in a table of 4 bits each at `[[0x8273EF10]] + 0x22C248` (just past the 2.2 MB save image in the save object),
and asks for one through `sub_8245BED0(table, id)` (41 callers; the select screen's grid is `sub_822F5E38`, its 32
slots a table at 0x82746018 of 72 bytes each, the release id a halfword at +0x24). The low 3 bits are the state, bit 3
marks it new:

| state | fresh save | meaning |
|---|---|---|
| 2 | 1389 ids | released |
| 0 | 1304 ids | locked, unlocked by playing (seven characters among them: Alpha Patroklos, Elysium, Edge Master, Pyrrha Omega and others) |
| 3 | 4380 ids (all marked new) | downloadable content: released only while an installed DLC package grants it |
| 4 | 353 ids | hidden (the answer is 0) |

Downloadable content: the DLC manager (object 0x8276F430, `sub_82387868`) enumerates the installed packages
(XContentCreateEnumerator, type 2), mounts each and binds an archive from it through CRI's file system; each package
grants ranges of release ids, which `sub_82385F48` checks for an entry in state 0, 1 or 3. Character 0xD (slot 21,
`ico_chr_02f`, almost certainly Dampierre: its voices are on the disc) is in state 3, and the grid gives it its own
lock mark.

`unlocks.cpp` answers released for states 0 and 1 and leaves 3 and 4 to the game, so everything on the disc is open
and paid DLC stays the player's own (docs\PLAN.md). The table is never written: the save keeps what was earned, and the
game's own unlocks (`sub_8210F008`, `sub_8245BF90`, which read the table directly) still record into it.

The game's "Practice with Frame Delay" adds 2 frames of input delay in every offline mode; Xenia's game patch "Reduce
input lag" (WAZAAAAA) turns it off at 0x8216E63C (`lwzx r8,r6,r5` -> `li r8,0`). To decide with netplay.

## The game's frame (frame.h)

`main` (sub_82189228) runs `while (sub_82163C00()) {}` after its setup (sub_82163490). sub_82163C00 is one frame:
it reads the pads (sub_82493BC8, XamInputGetState on the main thread), runs the sequences (sub_824943F0) and tasks
(sub_82494CA0), draws (sub_8249B8A8, sub_8249E548) and presents (sub_824945F8). Its call from main (0x82189264) is
the top of a frame: only main's own frame is on the guest stack there. The game runs at 60 frames a second.

**One simulation step a frame.** sub_824943F0 runs the game's step (sub_82493230: pads, sequences, the battle) once,
and in a fight (0x82771B78 set) once more for each whole step a float at 0x82927D48 says the game is behind (at most
2). The present (sub_824945F8) sets that float from the last 16 frame times it measured: below 1 at 60 frames a
second, 1 or 2 after a late frame. So the game caught up after a stall, and how a fight went depended on the clock:
in a test, four stalls of 20 to 300 ms moved the first KO from frame 7043 to 7116. frame.cpp sets the float to 0
before each frame's simulation, so a frame is one step whatever the clock does (a slow PC slows the game down
instead of skipping), and the same stalls now change nothing. A frame run again (no present) also used to take the
catching up meant for a later frame. `SC5_FRAME_CATCHUP=1` gives the game its own count back (tests).

## Sequences (the game's screens)

A sequence manager (sub_82328798) runs one sequence at a time; each has a creator (one function each) and a vtable
whose slot 1 is its update. The ids and creators:

| id | name | creator |
|---|---|---|
| 1 | Setup | 0x82328C48 |
| 2 | Attract (the opening movie) | 0x82328CE8 |
| 3 | Title ("Press START button") | 0x82328D88 |
| 4, 18 | AstralHome (the main menu) | 0x82328E70 |
| 6 | Entry | 0x82328F50 |
| 7 | Game | 0x82328FE8 |
| 8 | SignOut | 0x82329150 |
| 9, 10 | VSStandard, VSSpecial (VS Battle: select screens and the fight) | 0x823291F8 |
| 11 | Arcade | 0x82329298 |
| 12 | Creation | 0x82329060 |
| 13 | Option | 0x823290D8 |
| 14 | Training | 0x82329338 |
| 15 | LogoSequence | 0x823293B0 |
| 17 | Sample | 0x82329428 |
| 19 | WorldMap (Story) | 0x823294C0 |
| 20 | Online | 0x82329560 |
| 22 | QuickBattle | 0x82329600 |
| 23 | LegendaryOfSouls | 0x823296A0 |
| 25 | Invite | 0x82329740 |

The Title sequence (vtable 0x820CF010, update sub_8232FAD8) keeps its state at +0xD4: 4 waits for START, 5 waits
for the user manager to settle, 6-7 open the save, then AstralHome. It returns to Attract after 1800 frames idle.

Menu paths with a fresh save (the autopilot will follow them): Title, START; AstralHome opens on Story with a
first-visit window (A closes it); Offline Play is two up; in it VS Battle is one down from Quick Battle; then the
character select (each player picks with A, then a costume), the stage select (A on Random) and the fight.

## The battle (what rollback will build on)

- **State machine**: a struct at 0x82767D08 holds the current state (+0) and the pending one (+4) and embeds about
  17 states (+0x18 .. +0x1B8, each with its own vtable 0x820CEA0C ..). sub_82441628 runs it once a frame: a pending
  state is entered (slot 3 its id, slot 1 enter), then the current one's slot 2 (update) and slot 4 (what comes
  next). Seen in a VS fight: +0x18 before, +0xD0 / +0xF0 loading and intro, +0x24 the round call, +0x40 fighting,
  +0x58 KO, +0x128 between rounds.
- **Battle state snapshots**: the game saves its battle state into a ring of five 160 KB streams (vtable 0x820BA8B8:
  +4 written, +8 read, data from +12) about every 80 frames of a fight (sub_824419B0 -> sub_8211A8A8 ->
  save sub_82159DF0), and for the KO replay saves the present into a sixth, restores the oldest and plays on from
  there, and restores the present at the end (restore sub_8215A360). A short replay through the game's scripts
  (sub_82123140 ...) saves and restores a seventh. A snapshot is not a memory copy: each battle object writes and
  reads its own fields (slot 4 / slot 5), and pointers are written as (block, offset) into a list of the memory
  blocks the save registers (sub_82159D88), from both players' objects (0x82937070 and 0x82994BA0, 0x5DB30 bytes
  each) to the state machine's states; a save of a fight writes about 92 KB. Whether this can carry rollback (a snapshot every frame, restored on a misprediction) instead of copying guest memory as MK9 Online does
  is the first question of the netplay work.

## Determinism (what netplay builds on)

Two runs with the same seed and the same inputs (timed in fight frames) play the same fight: every battle event on
the same frame (fighting, KO, the KO replay, the second KO, between rounds), and the game's own battle snapshots
identical but for one frame stamp. What differs between runs, and why:

- **Random numbers**: the C library's `rand()` (state per guest thread, `_getptd()+0x14`; 115 call sites) and a
  Mersenne Twister, both seeded by `sub_824A35A8`: at boot from the date and time (`sub_82163490`), and again as a
  battle is set up from a value the game's own online code agrees on (`sub_823A0070`, `sub_823A22C8`: the seed at
  +0x19444). With different seeds the Random stage differs, and so does all that follows. `game::SetSeed` (and
  `SC5_TEST_SEED`) makes every seeding use one value: netplay will give it the seed both PCs agree on.
- **Frame counters**: the game keeps several (`0x82771B40`, `+4`, `+8`, `0x8280DE94`, `0x8281929C`, ...) that start
  during boot, so each runs ahead of the main loop's frames by however many frames the boot took (95 to 135 here).
  A battle object in physical memory (its serializer `sub_82469AA0`, the field at +0x1D6) stamps one into the
  snapshots. The fight does not show it; comparing snapshots across PCs must align or leave it out.
- **The heap's layout**: what loads on other threads allocates in another order, so the players' raw objects
  (0x82937070, 0x82994BA0) differ in pointers (a 512 KB shift in one run) and in a few floats of secondary
  animation. The game's snapshots write pointers as block and offset, and show none of it.

Test switches (game.cpp, testinput.cpp): `SC5_FIGHT_INPUT_P1..P4` (scripts by fight frame), `SC5_TEST_SEED=<n>`,
`SC5_TEST_HASH=1` (each fight frame's raw player hashes, and each battle snapshot's hash, into the log),
`SC5_TEST_DUMP=<fight frame>,...` with `SC5_TEST_DUMP_DIR` (the players' objects and every snapshot written raw).
A log file named on the command line is added to, not replaced: give each test run its own.

## Rollback (battle.cpp, net/synctest.cpp)

A frame of a fight is saved at its top with the game's own snapshot (sub_82159DF0, about 92 KB) and, with
`SC5_BATTLE_RAW=1`, the memory the snapshot leaves out, copied raw (`kRaw` in battle.cpp, about 800 KB). Put back,
the frames since are run again with the same inputs (frame::RunAgain: no drawing, no present). What had to be added
to the game's snapshot, each found with the diagnostics below:

- **The input pipeline.** sub_8216E2F8 puts each player's input into a delay ring of 61 slots, `delay` slots ahead
  (0x828FD130: per player a write count, the slots, a flag; the delay per player at 0x8275C15C, 2 offline): a frame
  run again pushed its input once more and the fight saw it a frame early. The task sub_8213DEC0 updates (a heap
  object, 0x401C0200) keeps each player's held buttons at +128 for the "newly pressed" bits. The game's 360 pad
  objects (0x828B3A40, 4 x 400 bytes) keep the last XInput packet number each saw and take a pad's state only when
  it changes: the input layer now keeps and puts back its own packet count with the key state (input::KeepKeys).
  The pad manager 0x82771080 (0xB40 bytes) and the 0x100 bytes before it.
- **Random numbers.** Besides the C library's rand (the main thread's `_getptd()+0x14`): a Park-Miller generator
  (sub_824A33D8 / sub_824A3248, state 0x829249F8), TT800 (sub_824A3030, 25 words at 0x82924A20, its position at
  +100) and a Mersenne Twister (`[0x82773FB0]`, 4992 bytes and its index). All are seeded by sub_824A35A8. The
  battle's effects, the stage and the AI draw from them too, dozens of times a frame.
- **The fighters' AI** (sub_822DC390): it runs every frame for both players, human or not (each has a virtual pad
  at 0x82A18518 + 152), with its state at 0x82A15980-0x82A19140.
- The players' objects whole (0x82937070 and 0x82994BA0), the battle's state machine, a few frame counters.

- **Random numbers apart for what rollback cannot put back** (rng.cpp). The effects, the stage's wind and two of the
  step's tasks keep their state in heap objects that come and go, so a frame run again can draw a different number of
  times there (counted per call site with `SC5_DBG_RNG_FROM/TO`: sub_8219D508 one draw more, sub_824A3458 two fewer),
  and every number the fight drew after that changed: in 4-frame rollbacks P1's idle timer (sub_821DF1C8, the 80
  bytes at the end of each player's object) drifted. Their updates (sub_82195D48, sub_821936C8, sub_82194210,
  sub_8219D508, sub_824A5ED0) now draw from a second set of the four generators' states, swapped in around them and
  never put back; the fight's set is what rollback keeps. Since then every call site the fight draws from draws the
  same number of times in a frame and its rerun. (This changes which numbers the fight gets compared with the
  original game, the same way on every PC; `SC5_RNG_SHARED=1` turns it off.)

The check (`SC5_SYNCTEST=<n>`, every fight frame put back n frames and run again; tools/synctest.sh runs a scripted
fight with and without it and prints both timelines) goes on from the frame as it ran again and compares the raw
memory and the game's snapshot; the snapshots write by write (each block matched by the guest address it came from),
each difference sorted into the players' poses, heap objects and the rest (the fight). Results with two human
players, 2026-11-17 (`SC5_TEST_P2_HUMAN=1`, first round, about 3,300 checks, the stage's gusts on):

- n = 1 and n = 4: every battle event on the same frame as without rollback (the round's KO at 9935), and no
  difference in the fight: the players' state, the battle's, the random numbers. What differs is cosmetic, from heap
  objects rollback cannot put back:
  - **Two leaf bones of each pose** (bytes +3923 to +4083 of 6208; the pose is in three buffers taken in turn, each
    player's +0x3190, +0xB190, +0x13190, and the snapshot keeps the current one), moved by the forces on cloth and
    hair: sub_821936C8 sums, for each player, a manager's list of forces (heap: the manager 336 bytes, sub_82193608;
    an animated wind and a force by the floor under the player, 400 bytes each, sub_82193AD8; gusts, 576 bytes,
    sub_82194210 at random intervals) into the player's +0x1CE30, which the pose's second stage (sub_822BDCE8) takes
    in. The forces keep oscillators, ages and lifetimes, and attacks create and free some of them: put back, they froze
    or crashed the game. Some 190 checks in 3,300 at n = 1 (while P1 attacks), most of them at n = 4.
  - **The weapon's trail** (a heap object of P1's, 416 bytes of sampled sword positions the snapshot carries for the KO
    replay): filled in a frame, empty in its rerun, and 14 bytes of counts the save writes from the stack beside it.
    Hits do not depend on it (every scripted hit lands on the same frame).
  - About 200 bytes a frame of the players' objects outside the snapshot (raw memory: the same secondary animation).
- Cost (release build): a 1-frame rollback about 3 ms, a 4-frame one about 8 ms, a save 0.2 ms; the game kept 60
  frames a second with a 4-frame rollback every frame.

## Netplay (net/netplay.cpp)

Adapted from MK9 Online's netplay (mk9/net/netplay.cpp), with the battle snapshots of battle.cpp in place of MK9's
copy-on-write memory. Packets (UDP, "SC5N"): HELLO, READY (seed, words, delay), INPUT (ack, now, seen, 24 frames of
inputs), CHECK (a frame's hash), BYE. Tests set it from the environment (`SC5_NET_BIND`, `SC5_NET_PEER`,
`SC5_NET_SIDE`, `SC5_NET_DELAY`, `SC5_NET_LOCKSTEP`, `SC5_NET_LOG`, a simulated network `SC5_NET_LAG` / `_JITTER` /
`_LOSS`); the launcher's matches from its start line (below, "The launcher's game"). Before the barrier the games
also pass each other the select screen's picks (PICK) and player 1's battle settings (SETUP).

- **The barrier** is the first frame of round 1's call (the fight loaded and its intro played: the intro goes as the
  characters' voice lines play, and from the second match on it ran on different frames on two PCs). The games meet
  there whatever time their menus, loading and intro took, and agree: player 1's seed (the game's seeding, sub_824A35A8, run again with it), player 1's
  counters that start at boot (0x82771B40-0x82771B5F, 0x8280DE94, 0x8281929C, 0x8281991C, 0x828E8790, 0x835B3EB4),
  the larger input delay, and the game's own input delay (0x8275C15C, 2 offline) set to 0: the netplay's stands in.
  The battle's second player is the second pad's (the VS menus would leave it to the CPU: versus.cpp's hook of
  sub_8243D430 while a match is on). The barrier also agrees the battle's state machine (0x82767D08, 0x200 bytes) and
  makes the fight's state alike: both games save the battle and hash each block of the fight in it, player 2's game
  sends its hashes (SUMS), player 1's game its bytes of every block that differs (FIX), written where they came from
  before the first frame (FIXOK). A first match from a fresh start and a rematch have had nothing to send so far; a
  match after a park had 4 blocks (21 KB): what the intro and a game's earlier matches left otherwise.
- **The round's call** (sub_82323DD0) asks whether a sound from before still plays: if so it stops it and its state
  keeps 1 at 0x82767D38, else 2. That follows the audio's timing, and after a first match the two games' round calls
  ran apart (the second match desynced at its first check); versus.cpp makes it 2 always (the sound is stopped all
  the same). VS Battle's own Rematch keeps the fight loaded and goes from the result to "between rounds", whose state
  counts on from the result screens; a rematch goes through Character Select instead (the same picks), loads, and meets
  at the intro.
- **Frames**: while a round is fought (battle state "fighting") they run ahead on a guess (the other's last input),
  8 frames at most, and a wrong guess puts the battle back and runs the frames since again within the frame. Outside
  it (the round's call, a KO, between rounds) both games run on neutral inputs without waiting (lockstep
  there made the intro wait at almost every frame with a round trip over the delay). START and BACK never leave a PC
  (no pausing a match).
- **Keeping the two even**: a game ahead by more than 1.25 frames (both ways, averaged over 8) holds a frame back:
  one vertical blank left out (the SDK's `IGraphicsSystem::HoldVblanks`, added for this): the screen shows a frame
  once more and the game runs one fewer. A sleep in the frame did nothing: the game's main thread runs a frame or two
  ahead of its GPU (it waits on a fence, sub_82543CB0) and made the time up with the frames after.
- **Checks**: every 30 frames of fighting both compare `battle::FightHash`: the snapshot's blocks of the fight, not the
  players' poses, heap objects, the stack, nor 13 words in each player's object and one at 0x829F552C that the
  snapshot carries as raw pointers into loaded data (found with `SC5_TEST_HEAP_SHIFT`, which moves one game's heap:
  equal on one PC, different on two), nor the float at 0x829F59D4 that sub_82210320 takes from the stage while the
  fight loads (-40.0 when the stage's data is not in yet: how far the loading had got; two games with -40.0 and -5.5
  there stayed alike in all else). Test dumps: `SC5_NET_DUMP_AT=<frames>` with `SC5_NET_DUMP_DIR` writes the snapshot
  and its blocks at those match frames (net<side>_m<match>at<frame>), which tools\py\cmpsnap.py compares block by
  block.
- **Sound** (sound.cpp): CRI AtomEx 1.18. The game gives a player its cue (sub_8256C4E8, criAtomExPlayer_SetCueId:
  r3 the player, r4 the cue sheet, r5 the cue) and starts it (sub_8256CDF8, criAtomExPlayer_Start; the error text
  "E2010021604" found it; the playback id back). It stops one sound with sub_8256C348 (criAtomExPlayback_Stop, the
  round's call uses it), a whole player with sub_8256C568 (criAtomExPlayer_Stop), and asks after one with sub_8256CD60
  (criAtomExPlayback_GetStatus). Two of its three ways to start a sound (sub_821D63B0, sub_821D6650: sound handles
  that keep the playback id at +80 and the player at +72) stop the handle's player first; the players come from a
  table by a slot the game deals out in turn (not part of what rollback puts back), and some sounds start when the one
  before has ended (CRI's status: the audio's own time). Each start is kept with its frame, player, cue and playback
  id, and a rollback reconciles them (sound::RollbackBegin/End, Timestone's sound plan adapted, below): a start in a
  frame run again that matches one of the first run's in the rollback's frames (the same cue, nearest frame first) is
  left out and the game gets that sound's playback id; one that matches none starts then, late; what the first run
  started that the frames run again did not is stopped at the end (a wrong guess's sound); a player stop in a frame
  run again spares what is still to be matched on that player and stops the rest one by one. Before (2026-11-17),
  starts were matched by player within one frame: under rollback most did not match (the player and the frame moved)
  and played twice (tools\synctest.sh 4, two human players: 1-3 dB louder in the fight's busy seconds than without
  rollback; now within a decibel, the KO on the same frame), a handle's stop silenced the first run's sound, and a
  wrong guess's sounds played on. What stays: a sound a wrong guess cut short does not come back.
  The log counts them every 100 rollbacks; `SC5_DBG_SOUND=1` logs the first 30 starts in frames run again that
  matched none, with the first run's starts near them.
- **Tests** (tools\nettest.sh: two games on this PC over 127.0.0.1, the scripted fight, player 1's script in game 1
  and player 2's in game 2, each with its own copy of the saves), 2026-11-17, delay 2: no added lag: in sync, no
  rollback; 50 ms each way: in sync, rollbacks of 1-2 frames (3-6 ms), 2 frames held back in all; 80 ms each way with
  10 ms of jitter: in sync, rollbacks of up to 5 frames (11 ms at worst), both games at 60 frames a second with no
  frame over 20 ms. Both games played round 1 to the same KO frame and went on into round 2. Further apart
  (tools\linktest.py `--lag`, 15 ms of jitter, 2% loss): 100 ms each way at delay 2 played smoothly; 150 ms each way
  at delay 2 rolled back the full 8 frames (about 16 ms each) and had to wait and drop frames now and then, so the
  launcher's own delay (the one-way trip in frames less one, 6 at most) is what kept such a match smooth. With the pose
  left out of frames run again (below): 150 ms each way at delay 2 with a spectator, 8-frame rollbacks of 8.4 ms on
  average and 11.5 at worst (13.2 and 18.2 before), every frame of the fight on time at 60 a second, the three games in
  sync to the same result.
- **What a rollback costs** (release build, the log every 600 frames while rolling): putting the battle back 0.46 ms,
  keeping a frame 0.12-0.13 ms, running a frame again 0.64 ms (1.43-1.75 ms before the pose was left out of it, below).
  Running the frames again is most of it: an 8-frame rollback is 6-8 ms on top of the frame (13-16 before). A frame is
  kept only where a rollback could go back to it: the other player's input for it was a guess, or it is a check frame
  (the check hashes the kept frame). Where both inputs were known the keep is left out, at the frame's top and in a
  rerun (`SC5_NET_KEEP_ALL=1` keeps every frame, for tests). This mostly saves time at short distances: far apart,
  nearly every frame is a guess when it runs.
- **Where a frame run again spends its time** (`SC5_TEST_RERUN_PROFILE=1`: the step, the pads and the tasks phase
  timed, and the game's tasks one by one: rerun_profile.cpp; logged every 2000 frames run again). The step
  (sub_82493230) walks the game's task tree (its manager at [0x835A3850], the first task at +12) and calls each task's
  methods through its vtable, which the recompiled code dispatches through the SDK's function table: the profile
  swaps each found method's entry for a timing wrapper of its own. A function called directly gets timed (and, with
  `SC5_TEST_RERUN_SKIP=<address>`, left out of frames run again) by a hook line (`SC5_RERUN_TIME`). In the
  forced-rollback fight, of 1.75 ms a frame run again: the fighters' update (sub_821246B8, vtable 0x820D89DC +4, once
  a fighter) 1.16 ms, of which their pose (sub_822BCF88) 1.09 ms. Most of the pose's main-thread time is handing its
  bones to worker threads, about 54 jobs a fighter (sub_822BEE50: a 16-slot queue behind a lock and two events); the
  cloth and hair (sub_822BDCE8) are 0.04 ms. Left out of every frame run again (frame.cpp), the fight stayed byte for
  byte the same but for the poses and a second buffer the pose fills for the drawing (each player's +0x1B310, three of
  0x800 bytes taken in turn; now counted as pose, not in the checks' hash), and the KO came on the same frame: a frame
  run again 0.64 ms, a 4-frame rollback 3.1 ms on average (7.4 before). The next tasks (sub_82123140, a fighter's,
  0.15 ms; sub_8213B3D8, 0.12 ms) change the fight when left out: they stay. What a frame shows after a rollback has
  its pose worked out afresh; the cloth and hair go on from where the rollback's first frame had them (a small jump
  in their swing at most). `SC5_RERUN_POSE=1` works the pose out in frames run again (tests).
- **Compared with GGPO**: the same core (a guess that repeats the last input, 8 frames at most, the time sync that
  holds the side ahead back, inputs sent with a window of earlier ones, spectators fed from a player's game), plus
  the fight checks every 30 frames and the sound reconciled after a rollback. GekkoNet (a C++ library in GGPO's line,
  BSD) has the same: prediction, delay, spectators, desync detection, limited saving (keeps fewer frames, rolls back
  further), a stress session like `SC5_SYNCTEST`.
- **Compared with Timestone** (github.com/tilderain/Timestone, the rollback mod for UMVC3 on PC, read 2026-11-17):
  the same model (the last input repeated, a delay and a rollback window, 16 frames of inputs a packet, a checksum in
  the input packets), on a game with no snapshot of its own: it saves the regions it found and rebuilds the game's
  objects (projectiles, effects) itself, and runs a frame again headless (only the update lanes that move the fight).
  Its time sync computes a wait it does not use (it waits only when the rollback window runs out); its check sums
  positions, life, actions and random numbers. Taken from it: its plan for sound (above), and its way of making
  rollback cheaper: profile a frame run again task by task and leave out only what is shown to do nothing for the
  fight (here the models' pose, above). Not taken: its snapshot reuse (off there: not faster and not safe), its ENet
  transport and GekkoNet, which it plans to move to.

## Sound levels (sound.cpp)

The game's sound is CRI AtomEx with one configuration (the ACF, registered from memory; `SC5_DBG_SOUND=1` logs its
@UTF tables): 23 categories in two groups, and a cue is in one category of each group. Group 0 is what the game's
Sound options set: id 0 SE, 1 SYSTEMSE (the menus' sounds), 2 VOICE (the fighters and the announcer), 3 BGM. Group 1
(BGM_battle, SE_hit_weapon, Voice_chara, Voice_narration, ...) stays at 1. sub_82338F38 applies the options (the
object at 0x8290DC84: +20 SE and SYSTEMSE, +28 VOICE, +16 BGM, levels 0 to 15 through sub_821D38B0) through
sub_821D4628 (r3 the category, f1 the volume), which clamps the volume to 0-1, scales it by the category's base
level (0x82902608: 0.85, 0.90, 0.90, 0.60) and calls criAtomExCategory_SetVolumeById (sub_8256B678; nothing else
does). The BGM level is also kept at 0x8274D5D0 for the movies (CRI Mana, sub_825A2838) and a BGM player.

The launcher's music, effects and voices levels scale what the game hands sub_8256B678 (music BGM, effects SE and
SYSTEMSE, voices VOICE) and the movies' volume (music); above 100% is louder than the game's loudest. A change applies
at the top of the next frame on the main thread (frame::Post): each category set again at the volume the game asked
for last. Measured at the main menu (its music alone, `SC5_TEST_AUDIO_RMS=1` logs the mix's loudness each second):
BGM muted is silence, 50% is 6 dB down, 150% 3.5 dB up; muting each category in turn (`SC5_TEST_SOUND_CYCLE=1`) left
the music alone except BGM.

## The launcher's game (link.cpp, autopilot.cpp, select.cpp, versus.cpp)

With `SC5_NET=1` (docs\PROTOCOL.md 3) the game boots to its hold point, the main menu, says `ready`, and takes
`start`, `park`, `rematch` and `quit` from cmd.txt; `starting`, `in_match`, `result ...`, `desync`, `disconnected`
and `error` go to status.txt. `SC5_LOG` names the game's log, `SC5_CACHE_DIR` the shader cache. Modes: `versus`,
`sparring` (Online Practice), `practice` (offline, Training), and `watch=1` (a spectator, through the launcher's feed);
without `SC5_NET`, `SC5_TRAINING=1` and `SC5_VERSUS=1` make a plain game for practice or the offline versus (below).

**The menus** (an empty save, as every online session has): Title, START (the logos and the opening movie skip on
START too); AstralHome opens with a first-visit window that A closes (15 s in), the cursor on Story: the hold point.
AstralHome's creator (sub_82328E70) makes a 1088-byte object; its menu (at +8, vtable 0x820BF434) keeps the depth at
+0x88 (0 the top level, 1 Offline Play's submenu), the top level's cursor at +0x94 (Offline Play 0, Story 2) and the
submenu's at +0x98 (Quick Battle 2, VS Battle 3); UP lowers them. Back from VS Battle the game opens the submenu again,
on VS Battle, so the autopilot goes by these cursors, not by a fixed path: Offline Play, A; VS Battle, A; VS Battle's
settings menu (Start Match, Player "Player VS CPU", Battle Count 3, Round Time 60, CPU Strength, Default), A on Start
Match; the select screen; the stage select; loading; the fight. After the last round the winner's pose, the Results
screen (A: Next), then a menu: Rematch ("fight again with the same character and settings": no loading, no intro; not used online),
Character Select, Return to Astral Home (it asks again, Yes / No, the cursor on No). START in a fight pauses it: Return
to Game, Move List, Options, Character Select, Return to Astral Home (the same question). `park` leaves a fight through
that menu, the result through the result's, the select screens with B.

**The select screen** (select.cpp). VS Battle's is set up by sub_822FBDF8 (vtable 0x820CBD50; sub_822F72C8 and
sub_82301500 set up the other modes'). Its object keeps each side's step at +0x9C + 4 side (0 choosing a character,
1 its costume, 2 and 3 done) and cursor at +0xAC + 12 side (x 0-7, y 0-3); sub_822FE558 moves a cursor (wrapping
both ways; the empty slots 19 and 20 make one crest in the third row that moves skip) and on A picks the slot y*8+x
(sub_823000B0; Random, slot 31, sub_823004B0) unless +1600 + 4 slot says it is locked. The costume panel's label
goes through sub_822FFF98(side, n, count) every frame it is up. "Player VS CPU": the first pad picks player 1's
character and costume, then player 2's. The 32 slots (the table at 0x82746018, 72 bytes each: +0 the game's
character id, +0x24 the release id, +0x26 the icon), row by row, as the screen names them:

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 0 | Patroklos (18) | Z.W.E.I. (14) | Leixia (22) | Natsu (20) | Ezio Auditore (31) | Raphael (9) | Viola (15) | Pyrrha (16) |
| 1 | Siegfried (3) | Hilde (11) | Xiba (21) | Ivy (4) | Cervantes (8) | Aeon (23) | Tira (10) | Nightmare (6) |
| 2 | Mitsurugi (0) | Maxi (1) | Yoshimitsu (24) | (empty) | (empty) | Dampierre (13) | Voldo (2) | Astaroth (7) |
| 3 | Creation | Alpha Patroklos (19) | Elysium (25) | Edge Master (5) | Algol (12) | Kilik (26) | Pyrrha Omega (17) | Random |

Devil Jin is not on it (in SC5 a style for created characters only), though data\sc5.json lists him.

**Picks.** A side the start line names (a roster id, or `<character id>.<costume>` from a result line) the autopilot
picks on both PCs: the cursor one press toward the slot at a time, A, the costume panel to the costume (in a mirror
match the second side cannot have the first side's: after three presses that change nothing it takes what the panel
shows), A. A side named `-` its player picks on their own PC with their own controller (only moves and A get
through; A only on a slot both games can load, so Dampierre only when both players have his package) while the
other game follows: their cursor as it moves, then their character and costume (PICK packets, repeated).
`SC5_TEST_LOCAL_PICK=<slot>[.<costume>]` stands in for the player in tests.

**The stage** must be the stage select's own choice in both games: with Random each game draws its own (seeding all
four generators in the frame of the press did not make the draws alike), and player 2's game taking player 1's stage
id at the battle setup was not enough (the fighters started elsewhere: the stage select sets up more than the id the
battle setup copies). The autopilot waits 5 s on the stage select (a press before it takes them was lost in one game
and not the other), then RIGHT 1 + seed % 8 times from Random, A.

**The battle setup** (versus.cpp, sub_8243D2A0 copying the VS settings record into the battle setup; the record:
per side 88 bytes: +0 1 a player / 2 the CPU, +4 the input, +8 the rounds to win (Battle Count), +12 and +16 health
240.0; then +176 the round time, +208 the stage). Online, player 1's game sets the rounds to win from the start line
(best of N: (N+1)/2) and sends its stage, round time, rounds and its values of the generators the setup does not
seed (Park-Miller and TT800, 0x829249F0-0x82924A87) and of the counters that started at boot; player 2's game waits
for them and takes them (SETUP). Both then seed the C library's rand and the Mersenne Twister with the match's seed.

**The result.** A match is over when a KO goes on to neither its replay nor "between rounds" (battle state 0xBC,
then 0x160). The rounds each side won: +0x75C in each player's object (+0x784 the same). The result line's
`p1_char`/`p2_char` are the character ids above with the costume index after a dot; a drawn match (0-0 on time)
gives `winner=0`, which the server does not take yet.

**Tests** (tools\linktest.py: two games on this PC through cmd.txt and status.txt, each with its own session folder
and an empty save, the scripted fight), 2026-11-17: boot to `ready` in 40 s; start to `in_match` in 31-38 s; picks
by roster id and on the select screen (`-`, Xiba in costume 2 and Voldo; a Pyrrha mirror: costumes 1 and 2) in sync
to the result, also with 60 ms each way (rollbacks of at most 2 frames, 4.5 ms on average, 60 frames a second, none
over 20 ms); Rematch in sync; park from the result back to `ready` in 20 s, from a fight in 7 s; a second match after
a park; quit. Elysium against Dampierre, and Xiba against Voldo with 60 ms each way and picks on the select screen: the first
match, a rematch and the next match after a park all in sync.

**The second player.** The VS menus ignore the second pad: the select (sub_82164720, at P1's first pick) makes the
other side the CPU (VS settings: type 2 at +88/+92 of the select's record; the battle's player then has input mode 2,
`+628`, taking the AI's virtual pad, and pad index 0, `+432`), signed-in second user or not. versus.cpp makes the
fight's second player the second pad's after the battle setup (mode 1, pad 1) in every online match, and in tests with
`SC5_TEST_P2_HUMAN=1`.

**Training (practice)** is the Offline Play submenu's fifth item (Arcade, Legendary Souls, Quick Battle, VS Battle,
Training: the submenu's cursor 4; it opens on Quick Battle, 2). It goes straight to the select screen (the same one,
sub_822FBDF8, every character open; the first pad picks the player's fighter, then the dummy), its stage select, the
round's call (no loading state between) and the fight with an endless clock. A fresh save shows its help windows
first ("Training", then the control pages), which A closes. START opens Training Options: Return to Game, Move List,
Switch Mode/State, System Settings, Reset Position, 2P Settings, Instant Replay, Display Settings, Cancel Changes,
Options, Switch Ring (greyed), Character Select, Return to Astral Home; the menu wraps, so UP from the top is the last
item, which asks again (Yes / No, on No). Display Settings: Life Bar, Mode/State, Advice Window, Damage, Display
Attack Properties, 2P Status, Display Control Help, Command Input History: no frame data. `start mode=practice`
(the start line's p1 and p2: the player's fighter and the dummy) goes there and hands the controllers to the player
once the fight is up (`in_match match=practice`); `park` closes any help window (A), then START, UP, A, UP, A; the
player leaving for the main menu in the game is the hold point again (`ready`).

**The plain games** (sc5_app.cpp, autopilot::InitPlain): `SC5_TRAINING=1` takes a game from boot to Training's select
screen and `SC5_VERSUS=1` to VS Battle's with "Player VS Player", and the autopilot stops there: the players pick.
VS Battle's Player setting (Player VS CPU, CPU VS Player, CPU VS CPU, Player VS Player: RIGHT three times) offers
Player VS Player only with a second user signed in, which users.cpp then signs in (a local profile, user 1); the
select screen asks the second player to press START on their controller. With the player's own save (its
`SC5SaveData\savedata.bin` there at start) the autopilot sets off at once; with a fresh one it waits for the main
menu's first-visit window and closes it first. Should it get lost on the way, it stops and the game is the player's.

**Online Practice** (`mode=sparring`, sparring.cpp): VS Battle has no endless clock (Round Time 30, 45 or 60), so at
the top of every frame of the match in sync, reruns included, the round's clock (0x8293619C: +0 the round's time, +0xC
the seconds left, +0x10 the frames left of the second) is held where it starts, and a fighter's health (player object
+0x2268C, full at +0x22688) is full again once it stayed the same for 90 frames, and at once below 40% (no single hit
takes more). What the rules remember is kept per frame, so a rerun starts from the frame before's. A ring out still
ends a round: the match plays to 5 rounds won. BACK goes over the wire (neither game's pad reads see it) and ends the
match in sync for both (`sparring_over`), and so does the other game's BYE. Two games with the scripted fight:
health refilled on the same frames in both, BACK at fight frame 1500 ended it for both in the same second, back at
`ready` 7 s after `park`.

**A spectator** (`watch=1 feed=...`, netplay.cpp's Watch*): the game goes to VS Battle's select screen as a player's
does (its autopilot's side is neither player's: both follow the feed's picks, or the start line's), takes player 1's
battle setup from the feed at its own (versus.cpp's WaitSetup), meets both players' READY at the round's call and
takes player 1's counters, battle state machine and whole fight there (the players' block sync gives player 2's game
only the blocks that differ; a spectator cannot ask), then runs every frame on both players' inputs from the feed:
never a guess, the neutral frames where the players had them, its own checks against theirs. Far behind, it runs up
to 120 frames a frame unseen (frame::RunAgain) until within 30 frames. Three games (tools\linktest.py --watch, a relay
in place of the launchers' feed): the spectator's frame log was byte for byte the players', its 221 checks agreed with
theirs, the same result line, also with 60 ms each way between the players (22 rollbacks) and picks on the select
screen; the feed was 254 KB.

**The match's stats**: `stats-<match>.json` in the session folder every 10 s and at the match's end (docs\PROTOCOL.md
3.4), from net::MatchStatsJson.

**Controls and display** (controls.cpp, display.cpp; docs\PROTOCOL.md 3.1): the keyboard and the controllers' button
map from `controls.toml`, the controllers assigned to players by the SDK's DeviceAssignment interface (player 2's by
name); the display settings as the GPU plugin's options through their `REX_` variables (the plugin registers them
after the app's own setup), brightness through the GPU's gamma ramp (reached through the command processor's
protected members), the master volume in the frames XAudioSubmitRenderDriverFrame hands the SDK. Brightness 180% and
60% changed a frame's mean luminance from 66 to 80 and 43, live.

**Diagnostics** (debug_watch.cpp, temporary): `SC5_DBG_WATCH=<hex address>[:len],...` (up to 4) with
`SC5_DBG_WATCH_FROM/TO` (fight frames; `SC5_DBG_WATCH_ABS=1`: absolute frames) logs every main-thread write there with
the guest function and its callers (the CPU's debug registers); `SC5_DBG_WIDEDIFF=<fight frame>,...` with
`SC5_DBG_WIDEDIFF_OUT` writes what differs in guest memory after the check's rollback and run;
`SC5_DBG_POS=<offset>,...` names the guest address a snapshot's byte came from; `SC5_DBG_PEEK=<frame>:<hex
address>:<bytes>,...` logs memory; `SC5_DBG_SPAN=<a>:<b>,...` writes every byte of guest memory that differs between the
ends of frames a and b (absolute; `SC5_DBG_WIDEDIFF_OUT`), `SC5_DBG_MEMDUMP=<frame>:<hex address>:<hex size>,...` and
`SC5_DBG_SELDUMP=<frame>,...` (the select screen's object) write memory into `SC5_TEST_DUMP_DIR`; `SC5_TEST_STALL=<fight
frame>:<ms>,...` holds a frame's end; `SC5_DBG_RNG_FROM/TO=<fight frames>` counts each generator's draws by call site in
frames and in their reruns (and the callers of a few sites); `SC5_DBG_FORCES_FROM/TO` (battle.cpp) logs the forces'
contents as they are summed; `SC5_NO_GUSTS=1` stops the stage's gusts; `SC5_DBG_DLC=1` logs the DLC manager's
packages and each index it read, and with `SC5_DBG_DLC_IDS=<hex>,...` asks the game's own check about those release
ids the first time the game asks about one (the select screen asks about Dampierre). The copy-on-write design of MK9 Online
(mk9/net/rollback.cpp: every page the simulation writes kept in an undo log, the heap's allocator with it) would also
put back the cosmetic objects above; nothing in the fight needs it so far.

