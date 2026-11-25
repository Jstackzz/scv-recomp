# SCV on PC: the game side

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
| `dlc.cpp` | the player's own downloadable content where the SDK finds it; costume packs' parts in fights (below, "Unlocks and downloadable content") |
| `link.cpp` | the launcher's side of docs\PROTOCOL.md 3: cmd.txt, status.txt (below, "The launcher's game") |
| `autopilot.cpp` | the menus from boot to the hold point, to a VS fight with the match's picks or to Training, and back; the plain games' way to their select screen |
| `sparring.cpp` | Online Practice: the round's clock held, health back up, at the top of every frame in sync (below) |
| `controls.cpp` | the launcher's `controls.toml`: the keyboard as player 1, every controller's buttons, player 2's controller, the buttons in a fight (`[fight]`, below "Controls in a fight") |
| `display.cpp` | the launcher's display and sound settings at the start and from `SC5_DISPLAY_FILE` as it changes |
| `framemeter.cpp` | frame data in practice: a frame meter as Street Fighter 6's over Training and Online Practice (below) |
| `result_overlay.cpp` | the panel over a lobby match's result screen where its loser chooses the next match (below, "The loser's choice") |
| `rerun_profile.cpp` | tests (`SC5_TEST_RERUN_PROFILE`): the game's tasks timed one by one in frames run again (below, "Netplay") |
| `select.cpp` | the character select screen: cursors, picks, the 32 slots and their characters |
| `creation.cpp` | created characters online: the player's own brought into the online game's save, this PC's tables read, the other PC's record checked and placed with SCV Online's own VS card (below, "Created characters online") |
| `creation_check.cpp` | the check of a created character from another PC, field by field (plain C++; `sc5\tests\creation_check_test.cpp`) |
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
- **Title screen waiting forever after START**: its user manager (`[0x8293B5E8]`, a record of 104 bytes per user
  from +0x10) watches each user's sign-in with a 60-frame countdown (+0x50) that must rest at 0 before the title
  goes on (`sub_8233C920` state 5). It compares the XUID it stored with XamUserGetSigninInfo's online (flag 1) and
  offline (flag 2) XUIDs; the SDK returned the same XUID for both, so a local profile looked like it had changed and
  the countdown restarted for ever. The SDK now returns no online XUID for a profile not signed in to LIVE.
- **The first START asks to create save data** (a system message box, "No save data found ... [Create Save Data]
  [Select Storage Device]", raised by `sub_8249FA48(task, 2, 14, 18)` from 0x824A0550 in the save task
  `sub_824A0390`; title update 1 had it at 0x824911C4): answered
  with Create Save Data (system_ui.cpp, by the call site, so in every language). The save lives in
  `<user>\B13EBABEBABEBABE\4E4D083D\00000001\SC5SaveData`.
- **Quit with a bad disc screen at the main menu**: the game looks up XamMediaVerification* by ordinal and checks
  that its disc reads back whole (`sub_82332A68`); the SDK's stubs returned garbage. Implemented as a check that
  found nothing (SDK patch).
- **Crash as a fight starts**: a battle object's state save reads its owner's field with no owner (guards.cpp).
- **Black eyes**: every fighter's eyes were black in close-ups, with only the highlights showing. The game finishes
  its costume textures' small mips on the CPU from images the GPU drew, and the GPU's resolves never reached the
  CPU's memory. The SDK now reads those images back while the textures are made (SDK patch, SDK.md).
- **Over-exposed scenes**: everything was brighter than on the console (paler skin, Mitsurugi's hair silver-white
  instead of dark with grey streaks). The game's auto exposure reads the scene's brightness, which the GPU shrinks
  to one small tile every frame, on the CPU (`sub_8234BF20`), and that resolve never reached the CPU's memory either.
  The SDK now reads small resolves made every frame back a frame late, without waiting for the GPU (SDK patch,
  SDK.md). Only drawing reads the exposure: nothing the netplay compares depends on it.
- **Algol's face black** (0.1.2 testers): on a PC whose shader cache was cold, the GPU dropped the first costume
  textures' draws while their shaders were compiling, and those textures kept whatever their memory held: Algol's
  face black, or blocks of noise. The SDK now makes draws into the targets textures are made in wait for their
  shaders (SDK patch, SDK.md).
- **Specks on white costumes** (0.1.2 testers): black and blue specks on white clothes (a white cat suit, Ezio's
  robe). The game writes its compressed textures' blocks as 16-bit floats, and the Xbox 360's 16-bit float holds
  values a PC's can't (no infinity or NaN), which white's colour needs. The SDK now writes them as the Xbox 360 does,
  on both render target paths (SDK patch, SDK.md).
- **Creation's pictures black** (0.1.2 testers): the character's picture on Creation Home and Shoot Picture's
  thumbnail panel stayed black. The game draws the character once and makes the picture from that on the CPU; the
  SDK now reads such a picture back to the CPU when it is taken (never in fights) (SDK patch, SDK.md).

## Unlocks and downloadable content

**The player's own packages** (dlc.cpp, 2026-11-17). Build game, or `python tools/py/x360.py dlc-install <package>
game/dlc`, lays a package out as `game\dlc\<package>\` with its files and `game\dlc\<package>.header` (its header).
At startup the game puts each into the saves where the SDK's content manager finds marketplace content
(`<saves>\0000000000000000\4E4D083D\00000002\<package>\`) and writes the SDK's record for it beside
(`...\Headers\00000002\<package>.header`: the content record, 0x148 bytes, the package's own display name, then a
license mask of every bit), and takes out what is not in `game\dlc` any more. A package's files go there as hard
links to the ones in `game\dlc` (2026-11-20: a costume pack's data is up to 120 MB, and every online game starts from
fresh saves in its session folder, the launcher keeping 30 of those), copies only when the saves are on another
drive (the log says so); a file already there stays when it is that file or a copy of it as it is (size and time),
and one the package no longer has goes. Dampierre's package (LIVE, "DLC01:DAMPIERRE", one 53-byte file,
`index_01_X360.dat`) puts him on the select screen. Nothing else unlocks paid content.

**A 360 DLC package is a key, and data.** The package holds one index file, `index_NN_X360.dat`, read by
`sub_82398158` (header) and `sub_82397F28` (entries), all big-endian:

    u32 version (1, never 0)   u32 ids (all entries')   u32 strings size, the strings (a zero byte first)
    u16 slots   u16 entries    each entry: u16 group, u16 slot (< slots), u8, u16 n1 + n1 u16 (a set: its items'
    slots), u16 archive, u16 file (FFFF, 0: none; else a file of the disc that the entry's file replaces), u16 n2 +
    n2 u16 (release ids), u32 the offset of the name of the entry's file in the package (0: none), u8 flag, u64 offer
    (0), u32 license bits (1: bit 0 of the package's license mask)

The DLC manager (table 0x82782F30, vtable 0x820D2394: state at +8, at +40 an array of 30 package objects of 344 bytes,
their count at +44) names its objects DLC00 to DLC29 and takes an installed package into the object whose name its
display name contains (`wcsstr`): "DLC01:DAMPIERRE" is DLC01. It mounts the first package of each object as `DLCnn:`
(any other of the same group only for its license mask, as `licence_drive`), reads `DLCnn:index_nn_X360.dat`, and for
each entry with a file name takes the size of `DLCnn:<name>` (sub_82364460) into the entry (+0x14, 48-byte entries
from the object's +0x1C, by slot) and marks it (+0xD bit 0x80); an empty name gives the harmless `NtCreateFile FAILED:
path='DLCnn:\'` lines of the log. An entry is granted (bit 0x40) when its license bits meet the package's license mask
(the object's +340; bits 0: always), and `sub_82394D40` (once the table's state is 7 to 11) finds a release id among
the granted entries' n2 ids. The files: a granted entry with an archive below 15 replaces that file of that archive
(`sub_823864C8`, slot 9: a std::map per archive at the table's +0x70; a unique insert, objects in order, so **the
lowest group's** copy of a file wins); every file request of the game goes through `sub_823627B0`, which sends a
replaced one to archive 14 with the entry's handle (group << 16 | slot), whose size (`sub_82394F80`) and path
(`sub_82386080`, slot 10 `sub_823966E0`: `DLCnn:<name>`) come from the entry; the file is read whole, as it is (no
CRI compression). A created character's downloadable models, ModelData 0x7Fggss00, load the same way: group gg's
entry ss. The archives: 0 cpudata, 1 cdata, 2 stage, 3 motion, 4 weapon, 5 creation, 6 human, 7 voice, 8 dna, 9
text, 10 demo, 11 patch (title update 1's `patch.cpk`), 14 the DLC (the table at 0x82753438, 24 bytes each).

**The player's PS3 DLC** (`tools\py\ps3dlc.py`, 2026-11-17). The PS3 game's packs are the same keys: a group's index
(`USRDIR/DLCnn/index_nn_PS3.edat`) has the same entries, with a PS3 tail (the content id's and the license file's
string offsets) and, in the free catalog packs of DLC06 to DLC13, a version-2 header (a leading zero word); a pack
bought adds `license_NN.edat`, which only the buyer's license (`exdata\<content id>.rap`) opens, and the index's entry
for the pack (a set, with its items' slots in n1) names that license file. The tool opens the player's licenses, takes
from each group's index the entries they grant (a set and its items, a music track), and writes one Xbox 360 package
per group, `PS3-DLCnn`, laid out as the game's own are (an STFS LIVE package: the header with the display name
"DLCnn:PS3 ...", one hash table, the file table, one file `index_nn_X360.dat`; its hashes filled in, no signature).
The player adds them on Build game with their 360 packages (or `x360.py dlc-install`), which installs them as it does
those; a group that a 360 package of the player's in the tool's output folder has is left to it. From the European
Dampierre pack the tool writes the 360 Dampierre index byte for byte. The catalogs are opened with the game's own key, which the PS3 game builds at run time through a
substitution table (`key[i] = table[source[i]]`, stored at +0x328 of its DRM object, both tables through its TOC);
`--eboot` reads it from the game's executable decrypted by RPCS3 (`rpcs3.exe --decrypt EBOOT.BIN`). With the
owner's 37 US and 41 European packs: 11 packages (DLC02 to DLC13 but 05, the US-only Dampierre pack whose license is
missing), 361 release ids; the game's own check (`SC5_DBG_DLC=1`, below) grants every one tried. Build game of the
published 0.1.1 takes them with the owner's 360 Dampierre package as it takes that one (a fresh unzip of the release:
12 packages installed, the same index files as before, the game at its main menu in 41 s with all 12 listed). Only DLC equipment for
created characters, music and Dampierre: nothing a versus pick on the select screen changes but Dampierre, whom an
online match allows only when both players have him.

**The costume packs' data** (2026-11-20). DLC02 to DLC04's parts are on the 360 disc; DLC06 to DLC13's (some 28
packs: Active Wear, Ancient Armor, Bridal, Classic Costumes, Dark Warrior, Hairstyles, Headgears, Maid, Modern, School
Uniforms, Sexy, Skeleton, Tekken, Valkyrie...) are not. On PS3 they come in the group's free catalog pack ("Soul
Calibur 5 - Cepheus Compatibility Pack 1" to 8, `EP0700-BLES01250_00-DLCSC5CATALOG001` to 008) as
`USRDIR/DLCnn/dat.cpk` (12 to 67 MB): an ID-only CRI archive (an ITOC), CRILAYLA-compressed files, **file id = index
slot**: slot 0 a small table, slot 1 the group's icons, each costume part (a bundle: NMD model, NTP3 textures, NDP3
meshes), and 13 entries that replace files of the disc, the same in every group: creation 1 (the parts tables,
whose "scrambled text" lists every parts set), cdata 381 to 384 (the parts' icon atlases), patch 8 to 15 (their names,
8 languages). Each group's tables list its own parts and every lower group's (DLC13's are the biggest), and the game
keeps the lowest group's: with every package carrying them, only DLC06's parts showed (105 models); with them in
DLC13's alone, all eight groups' (644 models, 323 downloadable parts sets). So the tool now also reads the catalog
packs' `dat.cpk`, adds to each costume group's package the group's own entries without a license (slots 0 and 1) and,
in the highest group it writes, the 13 replacing ones; converts each chosen entry's file to the 360's formats
(below), names it in the index (`NNN.dat`, the slot) and puts it in the package. Packages up to 120 MB need STFS
hash tables of more than one level (the tool writes up to three, the file table over several blocks, the hashes as a
console's); the launcher and `x360.py` read them (a test package of 150 MB and 71 files read back byte for byte). The
CRILAYLA decompression is plain Python (about 10 MB/s): with the owner's packs, 590 MB in 68 s.

**The parts' names** (dlc.cpp, 2026-11-24). The game's texts are tables of strings by number: `sub_82348660` (r3 the
table, r4 its data) puts a table in a std::map at [0x8293B4D4] and unscrambles the data in place (`sub_82349C28`: the
11 bytes before an FFFFFFFF end mark hold the key, a 16-bit word rotated 3 and xored with 0xEB7D; each byte xored with
the key's low byte, the key then rotated right 1, times 5, plus 1; a sum of 15 x each byte from 0xA21F must give the
key back; the same scheme on both platforms), `sub_82347670` (r3 the table, r4 the string, 1 on) gives a string, an
empty one for a table it does not have. A table: u32 0, u32 count, count offsets, the strings (UTF-8). Title update 1
registers 0 to 53 (the 54 sections of text 424, at boot) and 54 (patch 0 + the language: its own additions,
`sub_8215CCC8`). Creation names a part the DLC adds from **table 55**, which only a later title update registers:
patch 8 + the language (patch 8 to 15, 8 languages, English first). Title update 1's own patch 8 to 15 are empty
(23 bytes, no strings); the owner's later 360 title update (`TU_174Q21T_0000008000000.00000000000O2`, xexp 0.0.0.2 ->
0.0.3.2, patch.cpk 268 KB) has them with 421 strings, some still placeholders (`arm_201`), as the PS3 1.03 update's
patch.cpk does; the PS3 catalog pack DLC13's copy names every part of the 8 costume groups, and ends with each pack's
description ("Maid Costumes 1 contains ... (usable by female characters only)"). With `SC5_DBG_TEXT=1` the game asked
for table 55 strings 114, 115, 119, 129 at Creation's head parts (Tennis Cap, Student's Cap, Dark Warrior Helm,
Corinthian Helm). So the game now registers table 55 itself, the first time it asks for it without having it: the
installed packages' copy of patch 8 + the language (the lowest group's, as the game keeps for any file the packages
replace; the language is the patch file the game read for table 54, `sub_82363FC8`), copied as it is into guest
memory and handed to `sub_82348660`. Display only: nothing a match checks changes. Seen: "Corinthian Helm", "Bronze
Armor" in Creation's lists and on the parts worn (2026-11-24). Without costume packages nothing asks for table 55.

**Where the DLC is in the game** (the owner's 38 packs in 12 packages, 2026-11-24, `SC5_DBG_DLC=1` with
`SC5_DBG_RELEASE_OUT`): every one of the packages' 348 release ids is in state 3 (downloadable) in a fresh save's
table, granted by its package and released (answer 2); the other 4032 ids in state 3 are content none of the
owner's packs has. Dampierre: the select screen (third row, a "!" on him) and Creation's styles. The 26 tracks
(Soul Blade to Soulcalibur IV, Broken Destiny): the BGM list of VS Battle's stage select (Y and X there are the
character's and the stage's own music), each with its game's logo. The costume parts: Creation, a new character,
Equipment, each kind of equipment at the end of its grid (a "!" until seen); none is in Change All. Many packs are for
one sex only, which the game decides (the packs' own descriptions, table 55's strings 392 to 420): Maid Costumes 1,
Active Wear 1, Modern Costumes 1, Valkyrie Armor 1, Sexy Costumes 1, Tekken Costumes 2, Classic Costumes 1 female
only; Dark Warrior Armor 1 male only; the SCV Character Equipment, Hairstyles, Bridal, School Uniforms, Sexy Costumes
2 packs split by sex; Classic Headgears' Clown Mask only for the regular character Voldo.

**PS3 data on the 360.** The PS3 and 360 discs have the same files by id (CREATION.CPK and creation.cpk: 1850), so each
rule was read off the pairs and checked on all of them (creation, cdata, text, weapon): the PS3 file converted must be
the 360's byte for byte. A bundle (u32 count, count + 1 offsets) converts part by part, sizes unchanged. **Textures**
(Namco NUT, NTP3 -> NTXR): the magic; per texture the pixel format, 0, 1, 2 (DXT1, 3, 5) kept with every 16-bit word
of the data swapped, 14 -> 19 and 17 -> 20 (32-bit, cube maps too) with the data as it is; headers and tails the same;
no tiling. **Meshes** (Namco NUD, NDP3 -> NDXR): the magic; each material's alpha test (+0x0E, GL's 0x0200 + n -> n)
and cull mode (+0x12: GL_BACK 0x0405 -> 2, GL_FRONT 0x0404 -> 6); each material texture's sampler (+0x0C, +0x0D wrap
1 -> 0, 2 -> 1, 3 -> 2; +0x0E 3 -> 2; +0x0F magnification 1 -> 0, 2 -> 1; +0x10 GCM minification with its mip mode ->
the 360's minification and mip filter, 1 -> 0 2, 2 -> 1 2, 4 -> 1 0, 6 -> 1 1; +0x14 LOD bias 00001F00 -> -1.0f); u8
vertex colours byte-reversed (skinned: at the vertex clump's start; else after the position and normals, 20 or 36);
the rest the same. **Models** (NMD, both versions: the 360 disc itself has 1354 `NMD\x03`), tables and text: the same.
Creation: 1830 of 1850 files byte for byte; 16 models of two old material types (0x00, 0x08) differ only in sampler
byte +0x0E (4 on the 360 where the rule gives 2, which the PS3 file does not tell); 4 (579, 611, 1242, 1287) are other
revisions on the 360 (material values retuned, vertices exported again, a texture replaced, the model's stamp). The
DLC uses textures 0, 1, 2, 17, materials 0x28 and 0x24, vertex formats the discs have, and the tool refuses anything
else rather than guess. cdata and text also differ by platform (the UI's tables and shaders, "PlayStation" wording),
none of which the DLC replaces but the icon atlases, which convert byte for byte.

**Downloadable parts in a fight.** Creation marks a record whose parts sets use a 0x7F model (record byte 15,
`isDLCModelUsedFlag`, `sub_82231620`); `sub_82221978` copies the mark into the creation manager's entry (+0x34D8),
and a fight's setup of the character (`sub_82148400`, 28 callers) then swaps it for a template of its style in plain
clothes (`sub_8229EF18` mode 1: the style, body, colours, names and weapon kept). The game does that for every such
character, the data installed or not, offline and online (seen: the costume on the select screen's card, plain
clothes in the fight). A midasm hook (`Sc5DlcFighterModels`, dlc.cpp) takes the mark as clear when every 0x7F model
the record wears at any break level has its file on this PC (the manager's entry, bit 0x80): the character fights as
created. Online both games decide alike, since such a character is only picked when the two games' downloadable parts
are the same (creation.cpp's fingerprint): a created character in Dark Warrior and Skeleton parts fought in sync in
two games, its costume on both screens (2026-11-20).

The game keeps a release state for each of 7426 unlockable things (characters, stages, music, creation parts,
titles) in a table of 4 bits each at `[[0x8274EF10]] + 0x22C248` (just past the 2.2 MB save image in the save object),
and asks for one through `sub_8246B318(table, id)` (41 callers; the select screen's grid is `sub_82302A70`, its 32
slots a table at 0x82756020 of 72 bytes each, the release id a halfword at +0x24). The low 3 bits are the state, bit 3
marks it new:

| state | fresh save | meaning |
|---|---|---|
| 2 | 1389 ids | released |
| 0 | 1304 ids | locked, unlocked by playing (seven characters among them: Alpha Patroklos, Elysium, Edge Master, Pyrrha Omega and others) |
| 3 | 4380 ids (all marked new) | downloadable content: released only while an installed DLC package grants it |
| 4 | 353 ids | hidden (the answer is 0) |

Downloadable content: the DLC manager (object 0x82782F30, `sub_82396BC8`) enumerates the installed packages
(XContentCreateEnumerator, type 2), mounts each and binds an archive from it through CRI's file system; each package
grants ranges of release ids, which `sub_82394D40` checks for an entry in state 0, 1 or 3. Character 0xD (slot 21,
`ico_chr_02f`, almost certainly Dampierre: its voices are on the disc) is in state 3, and the grid gives it its own
lock mark.

`unlocks.cpp` answers released for states 0 and 1 and leaves 3 and 4 to the game, so everything on the disc is open
and paid DLC stays the player's own (docs\PLAN.md). The table is never written: the save keeps what was earned, and the
game's own unlocks (`sub_8210F218`, `sub_8246B3D8`, which read the table directly) still record into it.

The game's "Practice with Frame Delay" adds 2 frames of input delay in every offline mode; Xenia's game patch "Reduce
input lag" (WAZAAAAA) turns it off at 0x8216EB54 (`lwzx r8,r6,r5` -> `li r8,0`). To decide with netplay.

## The game's frame (frame.h)

`main` (sub_82189A00) runs `while (sub_82164070()) {}` after its setup (sub_82163900). sub_82164070 is one frame:
it reads the pads (sub_824A2F58, XamInputGetState on the main thread), runs the sequences (sub_824A3780) and tasks
(sub_824A4030), draws (sub_824AAC38, sub_824AD398) and presents (sub_824A3988). Its call from main (0x82189A3C) is
the top of a frame: only main's own frame is on the guest stack there. The game runs at 60 frames a second.

**One simulation step a frame.** sub_824A3780 runs the game's step (sub_824A25C0: pads, sequences, the battle) once,
and in a fight (0x82785678 set) once more for each whole step a float at 0x8293B794 says the game is behind (at most
2). The present (sub_824A3988) sets that float from the last 16 frame times it measured: below 1 at 60 frames a
second, 1 or 2 after a late frame. So the game caught up after a stall, and how a fight went depended on the clock:
in a test, four stalls of 20 to 300 ms moved the first KO from frame 7043 to 7116. frame.cpp sets the float to 0
before each frame's simulation, so a frame is one step whatever the clock does (a slow PC slows the game down
instead of skipping), and the same stalls now change nothing. A frame run again (no present) also used to take the
catching up meant for a later frame. `SC5_FRAME_CATCHUP=1` gives the game its own count back (tests).

## Sequences (the game's screens)

A sequence manager (sub_82335390) runs one sequence at a time; each has a creator (one function each) and a vtable
whose slot 1 is its update. The ids and creators:

| id | name | creator |
|---|---|---|
| 1 | Setup | 0x82335840 |
| 2 | Attract (the opening movie) | 0x823358E0 |
| 3 | Title ("Press START button") | 0x82335980 |
| 4, 18 | AstralHome (the main menu) | 0x82335A68 |
| 6 | Entry | 0x82335B48 |
| 7 | Game | 0x82335BE0 |
| 8 | SignOut | 0x82335D48 |
| 9, 10 | VSStandard, VSSpecial (VS Battle: select screens and the fight) | 0x82335DF0 |
| 11 | Arcade | 0x82335E90 |
| 12 | Creation | 0x82335C58 |
| 13 | Option | 0x82335CD0 |
| 14 | Training | 0x82335F30 |
| 15 | LogoSequence | 0x82335FA8 |
| 17 | Sample | 0x82336020 |
| 19 | WorldMap (Story) | 0x823360B8 |
| 20 | Online | 0x82336158 |
| 22 | QuickBattle | 0x823361F8 |
| 23 | LegendaryOfSouls | 0x82336298 |
| 25 | Invite | 0x82336338 |

The Title sequence (vtable 0x820CF930, update sub_8233C920) keeps its state at +0xD4: 4 waits for START, 5 waits
for the user manager to settle, 6-7 open the save, then AstralHome. It returns to Attract after 1800 frames idle.

Menu paths with a fresh save (the autopilot follows them): Title, START; AstralHome shows title update 3's notice of
its changes, then the first-visit window (A closes each), then opens on Story; Offline Play is two up; in it VS Battle
is one down from Quick Battle; then the
character select (each player picks with A, then a costume), the stage select (A on Random) and the fight.

## The battle (what rollback will build on)

- **State machine**: a struct at 0x8277B808 holds the current state (+0) and the pending one (+4) and embeds about
  17 states (+0x18 .. +0x1B8, each with its own vtable 0x820CF314 ..). sub_82450BD0 runs it once a frame: a pending
  state is entered (slot 3 its id, slot 1 enter), then the current one's slot 2 (update) and slot 4 (what comes
  next). Seen in a VS fight: +0x18 before, +0xD0 / +0xF0 loading and intro, +0x24 the round call, +0x40 fighting,
  +0x58 KO, +0x128 between rounds.
- **Battle state snapshots**: the game saves its battle state into a ring of five 160 KB streams (vtable 0x820BA908:
  +4 written, +8 read, data from +12) about every 80 frames of a fight (sub_82450F58 -> sub_8211A9E8 ->
  save sub_8215A298), and for the KO replay saves the present into a sixth, restores the oldest and plays on from
  there, and restores the present at the end (restore sub_8215A808). A short replay through the game's scripts
  (sub_82123400 ...) saves and restores a seventh. A snapshot is not a memory copy: each battle object writes and
  reads its own fields (slot 4 / slot 5), and pointers are written as (block, offset) into a list of the memory
  blocks the save registers (sub_8215A230), from both players' objects (0x8294AC10 and 0x829A8740, 0x5DB30 bytes
  each) to the state machine's states; a save of a fight writes about 92 KB. Whether this can carry rollback (a snapshot every frame, restored on a misprediction) instead of copying guest memory as MK9 Online does
  is the first question of the netplay work.

## Determinism (what netplay builds on)

Two runs with the same seed and the same inputs (timed in fight frames) play the same fight: every battle event on
the same frame (fighting, KO, the KO replay, the second KO, between rounds), and the game's own battle snapshots
identical but for one frame stamp. What differs between runs, and why:

- **Random numbers**: the C library's `rand()` (state per guest thread, `_getptd()+0x14`; 115 call sites) and a
  Mersenne Twister, both seeded by `sub_824B2EA0`: at boot from the date and time (`sub_82163900`), and again as a
  battle is set up from a value the game's own online code agrees on (`sub_823AFD00`, `sub_823B1FB0`: the seed at
  +0x19444). With different seeds the Random stage differs, and so does all that follows. `game::SetSeed` (and
  `SC5_TEST_SEED`) makes every seeding use one value: netplay will give it the seed both PCs agree on.
- **Frame counters**: the game keeps several (`0x82785640`, `+4`, `+8`, `0x82821A14`, `0x8282CE1C`, ...) that start
  during boot, so each runs ahead of the main loop's frames by however many frames the boot took (95 to 135 here).
  A battle object in physical memory (its serializer `sub_82478EF8`, the field at +0x1D6) stamps one into the
  snapshots. The fight does not show it; comparing snapshots across PCs must align or leave it out. Some count frames
  shown, not frames run: `0x82785644` (the present adds one when the GPU took the frame; the battle reads it, below,
  "The barrier") and the GPU's own `0x82821A14`, `0x8282CE1C`, `0x8282D49C` (below, "Rollback").
- **The heap's layout**: what loads on other threads allocates in another order, so the players' raw objects
  (0x8294AC10, 0x829A8740) differ in pointers (a 512 KB shift in one run) and in a few floats of secondary
  animation. The game's snapshots write pointers as block and offset, and show none of it.

Test switches (game.cpp, testinput.cpp): `SC5_FIGHT_INPUT_P1..P4` (scripts by fight frame), `SC5_TEST_SEED=<n>`,
`SC5_TEST_HASH=1` (each fight frame's raw player hashes, and each battle snapshot's hash, into the log),
`SC5_TEST_DUMP=<fight frame>,...` with `SC5_TEST_DUMP_DIR` (the players' objects and every snapshot written raw).
A log file named on the command line is added to, not replaced: give each test run its own.

## Rollback (battle.cpp, net/synctest.cpp)

A frame of a fight is saved at its top with the game's own snapshot (sub_8215A298, about 92 KB) and, with
`SC5_BATTLE_RAW=1`, the memory the snapshot leaves out, copied raw (`kRaw` in battle.cpp, about 800 KB). Put back,
the frames since are run again with the same inputs (frame::RunAgain: no drawing, no present). (The frame's own
drawing is left out; phase 0x400 still runs when sub_824AD5B0 calls it: a task's request of the drawing,
sub_824ADEB8, waits there in a loop of phase 0x400 and a short sleep until the drawing has taken it, and left out it
was never taken: Elysium against Edge Master with 40 ms each way waited for good at fight frame 160, 2026-11-18.)
What had to be added to the game's snapshot, each found with the diagnostics below:

- **The input pipeline.** sub_8216E810 puts each player's input into a delay ring of 61 slots, `delay` slots ahead
  (0x82910CB0: per player a write count, the slots, a flag; the delay per player at 0x8276FC4C, 2 offline): a frame
  run again pushed its input once more and the fight saw it a frame early. The task sub_8213E538 updates (a heap
  object, 0x401C0200) keeps each player's held buttons at +128 for the "newly pressed" bits. The game's 360 pad
  objects (0x828C75C0, 4 x 400 bytes) keep the last XInput packet number each saw and take a pad's state only when
  it changes: the input layer now keeps and puts back its own packet count with the key state (input::KeepKeys).
  The pad manager 0x82784B80 (0xB40 bytes) and the 0x100 bytes before it.
- **Random numbers.** Besides the C library's rand (the main thread's `_getptd()+0x14`): a Park-Miller generator
  (sub_824B2CD0 / sub_824B2B40, state 0x8293857C), TT800 (sub_824B2928, 25 words at 0x82938580, its position at
  +100) and a Mersenne Twister (`[0x82787AB0]`, 4992 bytes and its index). All are seeded by sub_824B2EA0. The
  battle's effects, the stage and the AI draw from them too, dozens of times a frame.
- **The fighters' AI** (sub_822E8FA0): it runs every frame for both players, human or not (each has a virtual pad
  at 0x82A2C0B8 + 152), with its state at 0x82A29520-0x82A2CC90.
- The players' objects whole (0x8294AC10 and 0x829A8740), the battle's state machine, a few frame counters.
- **The KO replay's manager** (sub_8211A9E8, once a frame from the battle's task: a heap object, `[task + 128] + 4`,
  0x41780004 so far). While a round is fought it saves the battle into the next of five streams (0x2800C bytes each
  from +0x2CC; a sixth holds the present during a replay) whenever its own frame counter (+0xF0318) is 84 past the
  last save's stamp (five stamps at +0x2B4, the next stream at +0x2B0); after a KO it plays one back. Nothing put
  its fields back, so every frame run again counted twice and its saves came earlier in a game that had rolled back
  more: two games saved at fight frames 2213 and 2218 and replayed the KO from there, and the check during the
  replay parted them (render scale 1 against 2, no lag; in a launcher test, one game replayed a KO for 3 frames and
  the other not). Its 0x2CC bytes before the streams, each stream's 12-byte header and the 0x7C after them are kept
  with each frame (DynamicRegions); a frame run again where the first run saved saves the same stream again.
- **The battle's cameras.** Which side of the screen each player is on turns the stick's left and right into back
  and forward: each player's +0xC4 (sub_8216E810: 1 for the player sub_8216C890 names), from the sign of a cross
  product of the two fighters' positions and the camera's (sub_8216B850: the view at +16 of the cameras' director,
  its x and z through vtable slots +80 and +88), or player 1 while the camera with the greatest blend weight
  (sub_824B3F58: eight at +608, the weight at +156) is in its modes 1 and 3. The camera follows the fighters
  smoothly, and nothing put it back: a frame run again moved it on once more, so after their rollbacks two games'
  cameras stood a little apart, and as the fighters crossed over the games turned them on different frames and read
  the same stick as other directions (Ezio against Viola with 40 ms each way: P1's inputs at +0x240C apart from frame
  3480; a KO's replay is another matter, below, "A KO's replay is not fought"). The
  director is the battle scene's task from +224 (`[0x8293B74C]`, 0x540 bytes to the task's end: positions, targets,
  blends; pointers only to itself, its cameras and what the fight loaded), its cameras two arrays in physical memory,
  8 of 0x320 bytes (`[+0x2B4]`) and 9 of 0x280 (`[+0x384]`), whose pointers are to static objects (a camera's target,
  a player's object): kept with each frame.
- **The battle's save slots.** A move's script can save the whole battle and a few frames on go back to it (the
  move-script interpreter sub_8212A0D8: command 9002 saves into one of four slots, sub_82488778; 9003 puts a slot
  back, sub_824888A0, keeping a few of the present's fields, each player's +468, +1588, +4188 and one of +1788's;
  9004 empties them all). Each slot is a stream like the KO replay's, at 0x49CA0 + slot * 0x2800C in the buffer of the
  battle's setup (`[0x8277B81C] + 140`); per slot a count at 0x8274C21C (-1 empty, 0 saved, one more each time it is
  put back), a word at 0x8293B770 (0 saved, 1 put back), and the KO replay manager's copy of the counts at 0x82910C04.
  Nothing put them back: a frame run again after its first run had gone back to a slot found the count and word as
  that run had left them, and the move's script went on without going back. With the same inputs one game went back
  7 frames (saved at 6015, put back at 6022) and the other, which had run 6022 on a guess first, did not (Xiba against
  Leixia with 40 ms each way: P1's counters 7 apart from frame 6023, the check at 6030 apart). The counts and words
  are kept with each frame, and so is each slot that holds a save.
- **The HUD's round markers** (hud.cpp). A round's end (sub_8213E7F8, from the "fighting" state's update) gives the
  winner the round through sub_82147418: the player's rounds won (+0x1108, a halfword, and the other count at +0x75C)
  and the HUD's own count of them (a heap object, `[0x82A06B3C]` while its +48 is `[0x82A06B38]`: +0x94 player 1,
  +0x98 player 2) each go up by one, and the marker's light starts (sub_8217D818, effect 1966 / 1967). The HUD draws
  five markers a side from its own count every frame (sub_82181D60) and takes the players' counts only as a round
  starts (sub_82329248). Nothing put the HUD's count back: a rollback over a KO's frame counted the round once in the
  first run and again in the rerun, and a KO only a wrong guess had (the real input took it back) stayed counted, so a
  player was shown with a round or more they did not have until the next round's call, and on the last round to the
  result: four markers lit in a first to 3, the two PCs apart, the match ending by the real count (the streamed first
  to 30 of 2026-11-20). Siegfried pushing Mitsurugi out, 60 ms each way with 25 ms of jitter and 3% loss, delay 1,
  player 2 tapping guard every 2 frames (2026-11-24): every round's KO was counted twice in one game or both, two KOs
  at 3376 and 3516 came only on a guess, and one game showed five markers in round 3 with one round won. The two words
  are kept with each frame (`SC5_HUD_KEEP=0` leaves them out, tests); the log says `sc5 hud:` where a frame shown has
  other markers than rounds won, and where a KO shown was taken back. Still shown from a KO taken back: its call (a
  "PERFECT" over a round that went on) and the marker's light.

What must not be put back:

- **The game's allocator.** The AI's region once ran to 0x82A2CCF0, over the first 0x60 bytes of the game's main
  allocator (a TLSF pool of 3 MB: its control block at 0x82A2CC90, sub_82389F18 / sub_82389F30 / sub_82389F40
  allocate, free and resize in it): the free lists of small blocks went back to the frame kept, a block taken since
  was handed out again, and a later allocation inside a restore wrote through a "next free" read from a block in use
  (Aeon Calcos against Siegfried with 40 ms each way: "write of guest 0x3ED82A23" from sub_82389800).
- **The GPU's frame counters**, though they too start at boot: 0x8282CE1C (the low word of the frame sync's count of
  frames presented, 0x8282CD80 + 152, sub_82552A28), 0x82821A14 (sub_82560AD8) and 0x8282D49C (sub_82553960). Only
  the present moves them (sub_824A3988, never in a frame run again), so each rollback set them back by its frames,
  and after a few hundred frames run again the game's wait for its GPU (sub_82552A28, at a KO's replay or a match's
  loading) waited for a frame it had already presented: the main thread blocked there for good. Nothing in the fight
  reads them; nor does the barrier agree them any more, nor the battle setup (versus.cpp).

- **Random numbers apart for what rollback cannot put back** (rng.cpp). The effects, the stage's wind and two of the
  step's tasks keep their state in heap objects that come and go, so a frame run again can draw a different number of
  times there (counted per call site with `SC5_DBG_RNG_FROM/TO`: sub_8219DD98 one draw more, sub_824B2D50 two fewer),
  and every number the fight drew after that changed: in 4-frame rollbacks P1's idle timer (sub_821DFA48, the 80
  bytes at the end of each player's object) drifted. Their updates (sub_8219B548, sub_82193EB0, sub_821949F8,
  sub_8219DD98, sub_824B58E8) now draw from a second set of the four generators' states, swapped in around them and
  never put back; the fight's set is what rollback keeps. Since then every call site the fight draws from draws the
  same number of times in a frame and its rerun. (This changes which numbers the fight gets compared with the
  original game, the same way on every PC; `SC5_RNG_SHARED=1` turns it off.) Two more, 2026-11-18 (battle.cpp,
  found by counting each call site's draws in both games' last run of every frame, Aeon Calcos against Siegfried with
  40 ms each way, which parted at frame 240 on a Park-Miller draw for P1's +0x5DAF0): the forces' own updates (slot 1
  of each force's class: sub_82191B40 for the wind and the force by the floor, sub_821923C8 and sub_821939E8, the
  gusts' sub_821931D8) add a random wobble to their vectors (sub_821916A0, TT800: three draws a force a frame, 27 in
  one game and 32 to 37 in the other, as each kept its own forces after its rollbacks); and an emitter of effects
  (sub_82382328: positions and directions at random for an effect id from the table at 0x82776F08) fires when a
  cooldown in its own object allows (sub_82385C50): 17 Park-Miller draws at frame 219 in one game, none in the other.
  And the drawing (phases 0x200 and 0x400) and the present (frame.cpp): a frame's first run draws and its rerun does
  not, so drawing from the fight's generators left them wherever the frame's last run had: a game's TT800 and
  Mersenne Twister went by which of its frames had been run again (Elysium against Edge Master with 40 ms each way:
  TT800 a draw apart from frame 1189, the Twister two from 434). Nothing in a round drew from them where it showed,
  but the picks between rounds did (below, "The fight's random numbers seeded again").
- **The pose in frames run again** (frame.cpp). Left out of them since the 0.1.1 work (two thirds of a frame run
  again), it reached the fight after all: each frame builds the skeleton (sub_822D0670 from the fighter's task
  sub_82123400, then sub_822D4538 from sub_8213B858) from the pose before it, which the pose's last stage changes
  (a bone scaled, sub_822C44C8), and sub_82146688 takes an angle of bone 2 into the fight (+0x3070; its sectors at
  +0x3066 / +0x3068 are read by the move scripts' interpreter, sub_8215D210). After a frame run again without its
  pose, that bone came out a bit or two apart, by how far each game had rolled back (Aeon Calcos against Siegfried,
  40 ms each way: P1+0x3078 at frame 210; with Siegfried against Siegfried nothing showed). It is worked out again;
  `SC5_RERUN_POSE=0` leaves it out (tests).

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
    hair: sub_82193EB0 sums, for each player, a manager's list of forces (heap: the manager 336 bytes, sub_82193DF0;
    an animated wind and a force by the floor under the player, 400 bytes each, sub_821942C0; gusts, 576 bytes,
    sub_821949F8 at random intervals) into the player's +0x1CE30, which the pose's second stage (sub_822CA898) takes
    in. The forces keep oscillators, ages and lifetimes, and attacks create and free some of them: put back, they froze
    or crashed the game. Some 190 checks in 3,300 at n = 1 (while P1 attacks), most of them at n = 4.
  - **The weapon's trail** (a heap object of P1's, 416 bytes of sampled sword positions the snapshot carries for the KO
    replay): filled in a frame, empty in its rerun, and 14 bytes of counts the save writes from the stack beside it.
    Hits do not depend on it (every scripted hit lands on the same frame).
  - About 200 bytes a frame of the players' objects outside the snapshot (raw memory: the same secondary animation).
- Cost (release build): a 1-frame rollback about 3 ms, a 4-frame one about 8 ms, a save 0.2 ms; the game kept 60
  frames a second with a 4-frame rollback every frame.
- 2026-11-18, n = 1: with two human players (`SC5_TEST_P2_HUMAN=1`, 240 s) both runs again keep every battle event on
  the same frame from the fight's start (the time-out at +3599, round 2's call at +3842), and no check finds the fight
  apart. With the script's own setup (player 2 the CPU's) they do not: the run with every frame run again fights the
  round otherwise (its KO at +1371 against +1559, the same in every run of each), though there too no frame's rerun
  differs from its first run in the fight. Not the drawing left out (`SC5_REPLAY_DRAW=1`: the same +1371) nor the
  vertical blanks' counter put back; the CPU's AI takes in something else, not found yet (no match online has a CPU
  player).

## Netplay (net/netplay.cpp)

Adapted from MK9 Online's netplay (mk9/net/netplay.cpp), with the battle snapshots of battle.cpp in place of MK9's
copy-on-write memory. Packets (UDP, "SC5N"): HELLO, READY (seed, words, delay), INPUT (ack, now, seen, 24 frames of
inputs), CHECK (a frame's hash), BYE, and since 0.1.4 RSKIP (a KO's replay cut short, below), DSUMS and DBYTES (what a
failed check says, below), AGAIN (the round again after one, below) and CALL (a round's call made alike, below). Tests
set it from the
environment (`SC5_NET_BIND`, `SC5_NET_PEER`, `SC5_NET_SIDE`, `SC5_NET_DELAY`, `SC5_NET_LOCKSTEP`, `SC5_NET_LOG`, a
simulated network `SC5_NET_LAG` / `_JITTER` /
`_LOSS`); the launcher's matches from its start line (below, "The launcher's game"). Before the barrier the games
also pass each other the select screen's picks (PICK), the stage select's (STAGE), player 1's battle settings
(SETUP), a player's skip of the way into the fight (SKIP) and, on a lobby match's result screen, its loser's choice of
the next match (CHOICE).

- **The barrier** is the first frame of round 1's call (the fight loaded and its intro played: the intro goes as the
  characters' voice lines play, and from the second match on it ran on different frames on two PCs). The games meet
  there whatever time their menus, loading and intro took, and agree: player 1's seed (the game's seeding, sub_824B2EA0,
  run again with it), the counters that start at boot (0x82785640-0x82785658, 0x828FC310, 0x835C79B4: the larger of the
  two games' values, and never below a game's own: 0x82785640 and 0x828FC310 count on while a game waits there, kept by
  other threads, and part the two games anyway; 0x8278565C, packed bytes, player 1's; not the GPU's frame counters,
  above, "Rollback"), the larger input delay, and the game's own input delay (0x8276FC4C, 2 offline) set to 0: the
  netplay's stands in. Until 0.1.3 player 2's game took player 1's counters, which set them back whenever it reached the
  barrier first (its loading or intro the faster), and a counter going back blacked out its scene. Two blends of the
  post-effects run every frame as step tasks (sub_82353358 -> sub_823534F0, the effects' parameters; sub_82355F78 ->
  sub_82356060, the colour grading) and weigh each effect by `elapsed = [0x82785648] - its start` against its fade-in:
  an effect started before the barrier with a fade-in of 0 gave `elapsed / fade-in` = -inf, NaN in the blended
  parameters (the post-effect manager [0x82A2C3E4] +256), the colour grading and the colour matrix of the final
  composite (sub_82355350): every scene pixel black, the HUD drawn on top, until the KO or the next round's call set the
  effect again. No desync (the fight never reads them). The exposure session found it (2026-11-19): 7 of 7 matches set
  back by 2 frames or more went black, none of 15 others. frame.cpp also guards both blends (an effect whose start is
  still to come weighs nothing yet), since a rollback sets the counters back as well (kRaw) and so does player 2's game
  taking player 1's setup at the stage select (versus.cpp). Algol against Pyrrha Omega, 40 ms each way, player 2's game
  on this PC's AMD iGPU (2026-11-19): player 2's scene black all round 1 before (0x82785648 set back 17 frames); lit all
  round with the counters' change alone, with the guard alone (it caught a fade-in-0 effect 8 frames ahead in each blend
  at the barrier), and with both. One of the counters, 0x82785644, counts frames shown (the present adds one when the
  GPU took the frame), and the battle throttles with it (sub_821561B0, sub_821143A8, sub_821CB220: "4 or more since the
  stamp", the stamp kept in its objects); a frame run again shows nothing and a busy GPU skips a count, so in a match it
  is the match's frame from its agreed value, set in every frame run (netplay.cpp `SetFrameInputs`).
  The battle's second player is the second pad's (the VS menus would leave it to the CPU: versus.cpp's hook of
  sub_8244C838 while a match is on). The barrier also agrees the battle's state machine (0x8277B808, 0x200 bytes) but
  for its word at +0x14, the address of a heap object (the menus' object the fight was set up from): player 1's left
  player 2's game pointing at whatever its own heap held there, and with the two heaps apart (two PCs) player 2's KO
  replays stopped playing, which parted the games at every KO (player 2's log names the agreed words it took that
  differed). Nor +0x10, which goes with it: that object counts as the fight's setup only while its +48 is the state
  machine's +0x10 (sub_82488778 and the KO replay's start test it); in a rematch the two games had numbered it apart
  (0x569 and 0x54B), player 2's game took player 1's and played no KO replay, and reached the result 397 frames before
  the other (Ezio against Viola with 40 ms each way, render scale 2 on game 2, 2026-11-18). Nor +0x138 and +0x13C
  (below, "Checks"). It also makes the fight's state alike: both games save the battle and hash each block of the fight in it, player 2's game
  sends its hashes (SUMS), player 1's game its bytes of every block that differs (FIX), written where they came from
  before the first frame (FIXOK). A first match from a fresh start and a rematch have had nothing to send so far; a
  match after a park had 4 blocks (21 KB): what the intro and a game's earlier matches left otherwise; VS Battle's own
  Rematch with one game 20 s longer on its result screen than the other, 9 blocks (25 KB).
- **The round's call** (sub_82330618) asks whether a sound from before still plays: if so it stops it and its state
  keeps 1 at 0x8277B838, else 2. That follows the audio's timing, and after a first match the two games' round calls
  ran apart (the second match desynced at its first check); versus.cpp makes it 2 always (the sound is stopped all
  the same). **A rematch** with the same two fighters on the same stage (`rematch` with `stage=keep`: both players
  chose REMATCH) is VS Battle's own Rematch, as on the console: the fight stays loaded, the result screen's menu goes
  to "between rounds" and round 1's call, and the games meet there as after a loading, however long each stayed on
  its result screen (the barrier agrees the state machine's counts and makes the fight's blocks alike); about 16 s
  from the end of a match to the next fight, half the way through Character Select. Ezio against Viola with 40 ms
  each way, best of 3, 2026-11-19: rematches with both lines at once, with one 8 s after the other and with one 20 s
  after the other, every match in step (the same rounds on the same frames, no check apart). **The menu on the same
  frame** (0.1.4, netplay GOAT packets, autopilot.cpp's Again): what the result screen shows goes on frame by frame
  (the winner's pose, the loser's), and the game's own Rematch starts the next round from it: two games that took the
  result screen's menu a frame apart, counted from the match's end, parted at the rematch's first check (Siegfried
  against Mitsurugi, the scripted fight, 40 ms each way: one game's snapshot had a block of player 2's object at
  +0x1C360 that the other's had not; 2 of 3 such rematches parted, 2026-11-20). Until 0.1.3 the menu was taken 12 s
  after the match's end, or as the launcher's `rematch` came when that was later, which two PCs get a few frames apart
  (a casual rematch chosen in the launcher's window after 12 s). Now each game tells the other how many frames after
  the match's end it had the `rematch`, and both take the menu at the larger of the two plus 2 s (12 s at least),
  counted from the match's end: rematches in step with the lines 0.4 s apart (Next at 1,115 frames in both games, the
  commands at 971 and 995). Any other
  rematch goes through Character Select, loads, and meets at the intro: per side the same fighter, the one the
  launcher names, or `-` for that side's player to pick again; then the stage select (below, "The stage"). The
  launcher sends it for a queue series' next match while both games wait at their result screens (the netplay keeps
  the two games in touch there with its hellos: 20 s without a word from the other ends it).
- **Frames**: while a round is fought (battle state "fighting") they run ahead on a guess (the other's last input),
  8 frames at most, and a wrong guess puts the battle back and runs the frames since again within the frame. Outside
  it (the round's call, a KO and its replay, between rounds) both games run on neutral inputs without waiting
  (lockstep there made the intro wait at almost every frame with a round trip over the delay). START and BACK never
  leave a PC (no pausing a match).
- **A KO's replay is not fought** (netplay.cpp `RoundFought`, battle::InKoReplay: the replay's manager in its state
  2, though the state machine says "fighting"). Fought, with rollbacks in it, the two games' replays came apart
  (2026-11-18): a player's press ends a replay early (its manager also waits on camera slot 0's blend weight), and
  where the frame that ended it ran again after a rollback, the cameras came out of it otherwise than from its first
  run (part of what they take in is worked out in the drawing, which a frame run again leaves out: with
  `SC5_REPLAY_DRAW=1` they came out alike). The next round's cameras stood apart, and the round went its own way after
  a crossing over (Cervantes against Yoshimitsu with 40 ms each way, from frame 5790); and in a replay each player's
  +0xC4 (the side of the screen, sub_8216C890: player 1 while the camera with the greatest weight is in its modes 1
  and 3) differed in its one check (Aeon Calcos against Siegfried, frame 2730). At the replay's end the game puts the
  present back.
- **A player cuts a KO's replay short** (netplay.cpp `ReplaySkipAsk`, RSKIP packets): the replay's manager
  (sub_8211A9E8) ends a replay when its player's pad has START or A newly pressed (the pad manager's +4: 0x8000, or
  0x80000000 that A sets). Online the replay runs on neutral inputs, never fought nor rolled back, so the press must
  come on one frame in both games: a player's own START or A while a replay plays asks the other game for a frame 30
  ahead (RSKIP, asked); the other game agrees while that frame is still to come there and nothing earlier was agreed
  (a lower frame asked by both at once wins), and refuses otherwise; the asking game waits at that frame for the answer
  should it not have come. At the top of the agreed frame both games press A on both pads for 4 frames if the replay
  is still going (it starts and ends on the same frame in both), and the replay ends there in both. The players' games
  write the agreed frame into the spectator feed (type 12), and a spectator's game presses there too. Tests:
  `SC5_TEST_SKIP_REPLAY=<n>` asks n frames into each replay. 2026-11-23, two games 40 ms apart, game 2's heap 2 MB
  higher, both players mashing: game 2 cut both KO replays short 15 frames in (match frames 1717 and 3387 agreed),
  both games pressed there and went between rounds on the same frame, every one of 86 check frames alike; in a
  heapsweep pair the mashing players' own A presses cut the replays short too.
- **The way into a fight plays** (autopilot.cpp): the VS screen while the fight loads, the stage's fly-over and the
  fighters' intros, as on the console, unless a player presses START on their own controller (the autopilot reads it:
  the game never sees a player's pad before the match runs in sync). That game sends SKIP for the coming match and
  the spectators' feed says so (type 12); both games then skip as the console takes START (the VS screen once the
  fight has loaded), and a spectator's game follows the feed. The two games need not skip on the same frame: they meet
  at round 1's call whatever time their way there took, and the barrier makes what the intro left alike. Tests:
  `SC5_TEST_SKIP_INTRO=1`, this game's player skips at once.
- **The fight's random numbers seeded again** (netplay.cpp `ReseedAtStateChange`) wherever the battle's state changes
  outside a round's fighting (the KO, the KO's replay and its end, between rounds, the round's call, the round's first
  frame fought), at the frame's top, from the match's seed and the frame, as the barrier seeds them (sub_824B2EA0); a
  spectator does the same. The two games reach each of these on the same frame with the same fight, while within a
  round their generators come apart though the fight stays alike: some draws from the C library's rand, TT800 and the
  Mersenne Twister come in a frame's first run and not in its rerun, or the other way round (who draws them is not
  known yet), and some between rounds come in one game only; the check does not hash the generators. Between rounds
  the game picks from them: Elysium's and Edge Master's style for the next round (each player's +0x1B8 and +0x17AC),
  which the two games picked apart (0x23 and 0x34), loaded for 19 and 14 frames and started round 3 on frames 5 apart
  (Elysium against Edge Master with 40 ms each way: TT800 a draw apart from frame 1189, the Twister two from 434,
  2026-11-18).
- **Keeping the two even**: a game ahead by more than 1.25 frames (both ways, averaged over 8) holds a frame back:
  one vertical blank left out (the SDK's `IGraphicsSystem::HoldVblanks`, added for this): the screen shows a frame
  once more and the game runs one fewer. A sleep in the frame did nothing: the game's main thread runs a frame or two
  ahead of its GPU (it waits on a fence, sub_82553960) and made the time up with the frames after.
- **Checks**: every 30 frames of fighting both compare `battle::FightHash`: the snapshot's blocks of the fight, not the
  players' poses, heap objects, the stack, nor 14 words in each player's object, two at 0x82A090C8 and the state
  machine's at 0x8277B81C and 0x8277B944 that the snapshot carries as raw pointers (found with `SC5_TEST_HEAP_SHIFT`,
  which moves one game's heap: equal on one PC, different on two; below, "Two PCs"), nor the count at 0x8277B940 that
  comes with the second (sub_8232C120 stores the pair between rounds, which sub_821490E0 works out from a player's
  pose: 1142 and 1126 in two games alike in all else, Kilik against Ezio with 40 ms each way; neither is agreed at the
  barrier), nor the float at 0x82A09574 that sub_82210948 takes from the stage while the fight loads (-40.0 when the
  stage's data is not in yet: how far the loading had got; two games with -40.0 and -5.5 there stayed alike in all
  else), nor the two words at 0x82A09170 that sub_8220F998 sets between rounds to a flag and a number that differs
  from game to game and run to run in the same way (Elysium against Edge Master as their next styles loaded: 1468
  against 1477; the fights alike in all else and the next round on the same frame), nor (0.1.4, below, "Every
  stage") the same two words of that list's other 15 entries, the two heap addresses at 0x82A08248 and the word at +16
  of each of the five effect slots at 0x82A06ED0. Test dumps: `SC5_NET_DUMP_AT=<frames>` with `SC5_NET_DUMP_DIR` writes the snapshot
  and its blocks at the top of those match frames as the two games end up with them (net<side>_m<match>at<frame>,
  and the raw memory kept beside it as `.raw.bin` / `.raw.blocks`): each run of such a frame keeps its top, and it is
  written once that frame's input and every one before it are known, so a wrong guess shows in neither game's files.
  tools\py\cmpsnap.py compares the snapshots block by block.
- **Two PCs** (2026-11-17, the first matches between two PCs, on one home network, 3 ms): two of three desynced, each
  a few frames after a throw landed (its LT shortcut), with no rollback at all. A throw stores the address of a heap
  object at +0x5BE90 of the thrower's object, which the check counted: each PC's heap lies elsewhere, so every landed
  throw "parted" two games that went on alike. Found by playing the match's own inputs again on one PC, two games
  through the link (the net log as fight scripts: testinput's `LX=`/`LY=`/`RX=`/`RY=`/`LT=`/`RT=` tokens carry the
  analog values), with game 2's heap moved (`linktest.py --heap-shift2`): the desync came at the same frame with the
  same hash as on the player's PC, and that word alone differed, by the shift. Then every character in 14 pairs,
  both mashing (walks, throws, quarter circles, Critical Edge motions, guards) for 7,000 frames with game 2's heap
  2 MB higher, each check's snapshot dumped (`SC5_NET_DUMP_DIR`, `SC5_NET_DESYNC_CONTINUE=1`) and compared: that found
  0x82A090C8 (Ezio's) and the state machine's +0x14 (the KO replays, above); with all three left out the matches stay
  alike to their end (`tools/heapsweep.sh`; `tools/py/netlog2script.py` turns a match's net log into fight scripts,
  `mash.py` makes varied ones, `snapsweep.py` compares the dumps). With 40 ms each way (rollbacks every few frames,
  2026-11-18) the same sweep found what rollback did not keep (above, "Rollback"): the pose left out of frames run
  again (Aeon Calcos against Siegfried, P1+0x3078 at frame 210), visual systems drawing from the fight's random
  numbers (the same pair at frame 240), the KO replay's frame counter, the cameras (Ezio against Viola at 3480), a
  KO's replay fought with rollbacks in it (Cervantes against Yoshimitsu, 5790; above, "A KO's replay is not fought"),
  and two things put back that must not be: the allocator's lists (a crash inside a restore) and the GPU's frame
  counters (a freeze at a KO's replay). The words that differed for one check at 0 ms in two pairs were stamps of
  the count of frames shown, which a busy GPU and frames run again moved apart (above, "The barrier": the battle's
  throttles at +0x22E9C and on, `sub_821561B0` keeps one at its +156); the floats at +0x22D60 and +0x231B8 and player
  2's +0x23B20 and +0x5CC08 are fight state, timers the fighters' tasks write (sub_8237D268 / sub_8237F128,
  sub_82143FD8 / sub_821441E8), and stay in the check. Then Xiba against Leixia parted at frame 6030 of round 3: the
  per-frame dumps (`SC5_NET_DUMP_AT`, the frames' final runs) were alike to the top of 6022 and 7 frames apart after
  it, one game having gone back to a move's save slot and the other not (above, "The battle's save slots"). The
  sweep of all 14 pairs then found three more: Elysium against Edge Master waited for good at fight frame 160 (a
  task's request of the drawing in a frame run again, above, "Rollback") and then picked the two mimics' next styles
  apart (above, "The fight's random numbers seeded again"), and Kilik against Ezio's count at the state machine's
  +0x138 (above, "Checks"). With all that (2026-11-18, three pairs at a time): all 14 pairs kept in step to the same
  result with 40 ms each way (172 to 385 checks each) and with none (167 to 394), no word apart.
  Render scale 1 against 2 with 40 ms each way, two rounds and a rematch (tools\linktest.py `--env2
  SC5_RENDER_SCALE=2 --rematch`): both games played every KO's replay and started each round on the same frame, in
  both matches. Graphics settings apart (`--render_target_path_d3d12=rov` against RTV, `--resolution_scale=2` against
  1, each side, and the two together; Mitsurugi against a standing Hilde, one and three rounds, none and 40 ms each
  way): 13 matches in step to the same result, where on the build of 2026-11-17 4 matches of 16 with settings apart had
  parted at frame 1350.
- **Every stage** (2026-11-24, after a first to 30 between two players on 2026-11-20 in which 11 matches parted in
  the middle of a round, the players said at ring outs and broken walls, on stages 2A, 2B, 12, 20, 1A, 16, 22, 19 and
  14). The sweeps before had mashed on whatever stage the seed drew, and a stage has effects of its own. A match on
  each stage place with player 1 pushing player 2 out of the ring (`tools/stagehunt.sh`, `HUNT_SCRIPT=ringout`:
  tools/py/ringout.py; 60 ms each way, 25 ms of jitter, 3% loss, game 2's heap 2 MB higher) parted on 9 of the 13
  places tried: stages 21 and 22 within ten seconds (frames 480 and 570), 19, 24, 16, 13, 27, 11 and 18 later. Each
  time the first words apart were a player's +0x5DAE0 and +0x5DAF0, a timer of the fighter that takes a Park-Miller
  number when a move's script says (sub_821DFB98, sub_821DE908, sub_821DFA48), and then the fight itself (stage 22:
  player 1's health 160 against 68 at frame 5640). `SC5_DBG_DRAWS=<file>` (rng.cpp) writes every Park-Miller number
  the fight draws, by match frame, as the frame's last run drew it, with who drew; `tools/py/drawscmp.py` puts two
  games' files side by side: on stage 21, at frame 475 the caller at 0x82382918 drew 6 numbers in one game and 9 in
  the other, and the fighter's draw of frame 476 came out apart. That caller is sub_823827C0, an emitter of effects
  that the stage's update fires (sub_82211F78, sub_82383C38): its particles live in heap objects, which a rollback
  does not put back, so the game that went back drew another number of times. Two changes (battle.cpp,
  net/netplay.cpp):
  - **The effects' emitters and the stage's particles draw from the visual states** (sc5/rng.h), as the wind and the
    first emitter found did: every function of the emitters' module that draws itself (15 from sub_82382590 to
    sub_823887E8) and the particles' update (sub_824BD678, 60 to 70 draws a frame on the stage with water).
  - **The fight's random numbers are seeded at the top of every run of every frame** from the match's seed and the
    frame's number (netplay.cpp `SeedFrame`: a frame's first run, each rerun, a spectator's). What a frame draws then
    depends on nothing an earlier frame drew, in either game: a system of that kind nobody has met yet can no longer
    part two games for the rest of a match (within a frame the fight's draws must still not come after such a
    system's). `SC5_NET_NO_FRAME_SEED` (tests) seeds as before, where the battle's state changes only. The seeding
    leaves its mark in what the check hashes, so a spectator's game checks a frame before it seeds it, as the
    players' games keep a frame before they seed it (seeding first, it was apart at its first check, frame 120; with
    the order right, 341 checks alike to the same result as the players', linktest.py `--watch`). These seedings are
    not logged (60 lines a second otherwise).
  With both, stage 21's match: every one of 148 checks alike and all 6,688 frames' draws alike in the two games.
  `SC5_NET_CHECK_RESTORE=1` (tests) saves the battle again right after a rollback put a frame back and logs every
  word of the fight the game's restore left otherwise (none on stage 21: the game's restore sets that timer up anew,
  sub_82149198, and the raw memory put back after it has the kept value).
  The same match on all 27 stage places with those two changes (`tools/stagesweep.sh`): 20 in step to the same
  result with every check alike and every frame's draws alike; the other 7 showed four more things, none of them the
  random numbers:
  - **Two heap addresses in the check** (stage 2A from frame 8,550 with the rounds at 3-1, stage 18 from 6,510 at
    2-0): the words at 0x82A08248 and 0x82A0824C, which the stage sets late in a match, 0x41483010 in one game and
    0x41683010 in the game whose heap lies 2 MB higher, to the match's end. Two PCs never have the same there, so any
    match that came so far on those stages failed its next check with no rollback at all: stage 2A lost three of the
    11 matches of 2026-11-20. Left out of the check, as the other raw pointers are (battle.cpp `KindOf`).
  - **The stage's list** (stages 27 and 11): the 16 entries of 64 bytes at 0x82A09170 (sub_8220F998 adds one a kind
    of thing the stage does) each begin with a flag and a number that differ from game to game and run to run; the
    first entry's were left out of the check on 2026-11-18, and a fight with more of the stage's doings fills the
    next (829 against 944 from frame 810). All 16 entries' first 8 bytes are left out now.
  - **An effect slot's number** (stages 1C and 26): five slots of 176 bytes at 0x82A06ED0, taken in turn; a move's
    script makes one with a word of its fighter's (sub_82185230, sub_82184DC8), kept at the slot's +16: 1 in one game
    and 2 in the other from frame 355 to the match's end, and 2 in both in the next run of the same match. Nothing
    reads it in the second after but the snapshot's own save and restore (`SC5_DBG_WATCH_READS`). Left out.
  - **A round's call not alike** (stage 22, round 3): below, "The check at a round's call".
  With all of it, the same match on the 27 places as two players' games run it (`HUNT_PLAY=1`: a failed check plays
  the round again or makes a round's call alike): every match to the same result in both games; 25 with every check
  alike and nothing to do, 2 with one round's call made alike (stage 23 at frame 1,068, stage 22 at 5,512); no round
  played again. And the 14 pairs of characters of the heap sweep, both mashing for 9,000 frames, the same way
  (`tools/pairsweep.sh`): all 14 to the same result in both games with every check alike, no call made alike and no
  round played again.
- **The check at a round's call** (netplay.cpp `CallCheck`, `CallSync`; 0.1.4). Outside a round's fighting the two
  games run on neutral inputs and compare nothing, and the game itself is not the same there on two PCs: after a KO
  a fighter's script waits on its voice (CRI's playback status, the audio's own time: sound.cpp), so the winner's line
  and pose can start a few frames apart. Stage 22, the ring out of round 2 with rollbacks at the KO: player 1's
  +0x2325C and +0x29D8 apart 4 frames after the KO (`SC5_NET_CHECK_ALWAYS=<n>`, tests: a check every n frames there
  too), and at round 3's first check its timers at +0x231B8 160 against 68 and its place a hair apart; with a
  hardware watch slowing both games it did not happen. Until now the round's first check took that for a desync. So
  the first frame of every round's call after the first is checked like a fought frame's, and when that check fails
  the two games say what differs (as above), agree a frame (CALL packets: the later of the two games' present ones,
  the game behind runs on to it) and make the fight's blocks alike at its top as at the barrier, before the round is
  fought. Nothing is put back, nothing is fought again and the players are told nothing: a pause of a round trip or
  two during the round's call. Stage 22's match then: the call of frame 5,512 apart, 5 blocks (24 KB) made alike at
  frame 5,561 in 0.2 s, round 3 fought from 5,631 with every check alike, 3-0 in both games. Should the call be over
  first (a long round trip), the round's first check finds what is left and the round begins again.
  `SC5_NET_NO_CALL_CHECK` (tests): no check there.
- **What a desync says** (netplay.cpp `Evidence`, `Detail`; 0.1.4). A check is one hash, and a failed one said only
  that the fights differed; the launcher's report of it was 36 lines, mostly statistics. Each check frame's snapshot
  is now kept for 8 checks. When a check fails, the two games pass each other a hash of every 256 bytes of that
  frame's snapshot as the check hashes it (DSUMS, each page by its guest address since 0.1.5), then their
  bytes of the first 16 pages that differ (DBYTES), and each game's log names up to 48 words with both values (lines
  with `sc5 desync:`, which the launcher's report carries whole, PROTOCOL.md 1.13.1): the match frame, the battle's
  state, the rounds won, both players' health, the rounds fought so far with each one's end and health (a ring out:
  neither at 0), which places differ, then `P1+0x2268C: 43393333 here, 43383333 there (as floats 185.2 and 184.2)`.
  It takes about a tenth of a second, a second and a half at most. `SC5_TEST_DESYNC_AT=<match frame>[:<side>]`
  (tests) takes a point of player 1's health in one game at that frame: a desync made to order.
- **The round again** (netplay.cpp `RoundAgain`; 0.1.4). A failed check no longer ends the match. The battle is kept
  as each round's fighting begins (`KeepRoundStart`: a snapshot of the round's first fought frame). Once the two
  games have said what differs, both stop (AGAIN packets name the round by its first frame and the frame number each
  would go on from), each puts its own snapshot of the round's start back with the game's own restore (as a rollback
  does, only further: from the middle of the round, or from the KO it just went to, the state machine back at
  "fighting"), the two make the fight's blocks alike as at the barrier (SUMS, FIX, FIXOK: player 2's game takes
  player 1's bytes of every block that differs, at a moment with no throw or projectile under way), the random
  numbers are seeded again, and the frames go on from the larger of the two games' next free numbers, so nothing
  sent for the frames left behind is mistaken for the new ones. The players see the round start again with full
  health and its time back, and for five seconds a notice in large type (result_overlay.cpp: "OUT OF SYNC / THIS
  ROUND STARTS AGAIN"); about 150 ms on one PC. Three times a match at most; after that, in Online Practice, or when
  it cannot be done (the other game does not come along in 6 s, the two are in different rounds, the game refuses
  the snapshot) the match ends as a desync as before. A game whose own check did not come to be compared (a lost
  CHECK) goes along when the other asks, from its wait for inputs too. The game's status says it (`in_match
  match=<id> again=<times> again_frame=<the failed check>`) and the launcher sends the game's log of it to the
  server as a desync's. The spectators' feed ends at the first round played again (their games cannot follow it
  yet). Tests, two games with 40 ms each way: a desync made at frame 900 of round 1 (the check of 930 failed, the
  round began again at 1,025 with 240 health each, the match went on to the same result in both games with every
  later check alike); one 30 frames before a ring out, the games already at the KO when they went back (KO to
  fighting, the round fought again to the same ring out, the match to 3-0 in both); and through two launchers and a
  local server (tools\online-e2e.ps1 with a real game: both launchers' reports paired by the server, the match played
  on to its result). `SC5_NET_NO_ROUND_AGAIN` (tests): a failed check ends the match.
- **A match after a park** (0.1.4): the barrier's block sync (MeetSync) is kept by the match's number, and a start
  after a park counts from 1 again: the last session's "FIX built, FIXOK came, applied" of its match 1 let both games
  through the new barrier at once with nothing made alike, and the match parted at its first check (frame 120: 7 of
  7 matches started after a desync on 2026-11-20, each time until both players closed their games). `Open` now
  starts it empty. linktest.py `--park-after 25 --again`: frame 120 apart before, 4 blocks (1,816 bytes) taken and
  the match in step to its result after.
- **Player 2's pose in the check** (0.1.5; battle.cpp `KindOf`). The first day of 0.1.4 (2026-11-24: two pairs of
  players, 29 matches finished, 7 ended as desyncs, 170 reports) every match went back to its round's start at its
  first check (frame 120), often again at 210 and 300, some three times and then ended; rounds' calls were made alike
  at almost every round; one match failed in the middle of a round. Health was the same in both games every time, and
  the barrier's sync kept finding "1 blocks (2048 bytes)" apart. The game puts each player's pose buffers on round
  addresses (the pose's three, 6,208 bytes, on a multiple of 0x100; the three the pose fills for the drawing, 0x800
  bytes, on one of 0x80), so they lie at +0x3190 and +0x1B310 of player 1's object and at +0x3160 and +0x1B360 of
  player 2's (the snapshot keeps the current one of each: 0x8294DDA0 / 0x82955DA0 / 0x8295DDA0 and 0x82965F20 /
  0x82966720 / 0x82966F20 for player 1, 0x829AB8A0 / 0x829B38A0 / 0x829BB8A0 and 0x829C3AA0 / 0x829C42A0 /
  0x829C4AA0 for player 2, `SC5_NET_DUMP_AT` of six frames running). The check left out player 1's offsets in both
  objects: the first 0x30 bytes of player 2's pose and the last 0x50 of its third buffer for the drawing were hashed
  as the fight. Two games on one PC have those bytes alike, which is why every sweep here passed; two PCs do not for a
  second or so after an intro, a winner's pose or a hitch (cloth and secondary bones, worked out by worker threads),
  and 0.1.4 plays the intros that 0.1.3 skipped on one frame in both games. Each player's buffers are now left out by
  their own addresses. `SC5_TEST_POSE_NOISE=<side>` (tests) writes the frame's number into the end of player 2's three
  buffers for the drawing in that player's game, every frame: on 0.1.4's check the first check failed (frame 120,
  P2+0x1CB50 apart), now a whole match's checks are alike, and the 14 pairs of `tools/pairsweep.sh` with that noise on.
  The reports of that day said only that the snapshots were "laid out apart": the detail's layout hash took the
  blocks' addresses from 0x80000000 on, and a pose's block is whichever buffer is current in that game. The detail
  now goes by guest address (DSUMS: address and hash of every page of the static memory in the check; a page only one
  game has differs too), whatever each game's snapshot holds beside them.
- **Rollback over a throw** (battle.cpp `Restore`): the game's own restore makes a throw's heap object again and
  sets its owner's +0x5BE90 to it; the raw copy of the players' objects put back after it held the address from when
  the frame was kept, freed since, and a call through it crashed the game (Aeon Calcos's back throw: "invalid call ...
  0x00000032"). Where the game's restore set that word, its value stays.
- **The way into a fight** (autopilot.cpp): the VS screen (both fighters on their cards while the fight loads) takes
  START as on the console once the fight has loaded (A, B, X, Y do nothing there), and so do the stage's fly-over and
  the fighters' intros. Until 0.1.3 the autopilot skipped them always; now they play unless a player skips them
  (below, "The way into a fight plays"), and then the autopilot presses START on both pads every third of a second
  while they are up (the VS screen 359 frames to 101, the intros 484 to about 40: from the start line to `in_match`
  31-38 s played, 31 s skipped). In the
  one launcher test with the intros skipped (2026-11-17), the two games took a KO's replay differently (one went to a
  3-frame replay, the other straight between rounds) and the rematch after it parted: not the skip, nor the display
  settings as such (render scale 2 there), but the KO replay's frame counter, which rollback did not put back (above,
  "Rollback"): the game with more frames run again saved its replay points earlier. (A 1- or 2-frame "replay" after
  a KO is the game's own: a save just before the KO, the same with no START at all.) Render scale 1 against 2 since:
  the same replays, in sync to the result (tools\linktest.py `--env2 SC5_RENDER_SCALE=2`).
- **Sound** (sound.cpp): CRI AtomEx 1.18. The game gives a player its cue (sub_8257C298, criAtomExPlayer_SetCueId:
  r3 the player, r4 the cue sheet, r5 the cue) and starts it (sub_8257CBA8, criAtomExPlayer_Start; the error text
  "E2010021604" found it; the playback id back). It stops one sound with sub_8257C0F8 (criAtomExPlayback_Stop, the
  round's call uses it), a whole player with sub_8257C318 (criAtomExPlayer_Stop), and asks after one with sub_8257CB10
  (criAtomExPlayback_GetStatus). Two of its three ways to start a sound (sub_821D6BB8, sub_821D6E58: sound handles
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
  launcher's own delay (the one-way trip in frames less one, 6 at most) is what kept such a match smooth. While the
  pose was left out of frames run again (0.1.1; it is worked out again since, below): 150 ms each way at delay 2 with a
  spectator, 8-frame rollbacks of 8.4 ms on average and 11.5 at worst (13.2 and 18.2 with it), every frame of the fight
  on time at 60 a second, the three games in sync to the same result.
- **What a rollback costs** (release build, the log every 600 frames while rolling): putting the battle back 0.46 ms,
  keeping a frame 0.12-0.13 ms, running a frame again 1.43-1.75 ms with its pose (0.64 ms while the pose was left out
  of it, below). Running the frames again is most of it: an 8-frame rollback is 13-16 ms on top of the frame. (Six
  games at once on one PC, 40 ms each way: 2.0 ms a frame run again with the pose, 1.15 without, 59.2-59.4 frames a
  second.) The launcher's delay (the one-way trip in frames less one) keeps most rollbacks to 1-3 frames. A frame is
  kept only where a rollback could go back to it: the other player's input for it was a guess, or it is a check frame
  (the check hashes the kept frame). Where both inputs were known the keep is left out, at the frame's top and in a
  rerun (`SC5_NET_KEEP_ALL=1` keeps every frame, for tests). This mostly saves time at short distances: far apart,
  nearly every frame is a guess when it runs.
- **Where a frame run again spends its time** (`SC5_TEST_RERUN_PROFILE=1`: the step, the pads and the tasks phase
  timed, and the game's tasks one by one: rerun_profile.cpp; logged every 2000 frames run again). The step
  (sub_824A25C0) walks the game's task tree (its manager at [0x835A3850], the first task at +12) and calls each task's
  methods through its vtable, which the recompiled code dispatches through the SDK's function table: the profile
  swaps each found method's entry for a timing wrapper of its own. A function called directly gets timed (and, with
  `SC5_TEST_RERUN_SKIP=<address>`, left out of frames run again) by a hook line (`SC5_RERUN_TIME`). In the
  forced-rollback fight, of 1.75 ms a frame run again: the fighters' update (sub_82124978, vtable 0x820D9394 +4, once
  a fighter) 1.16 ms, of which their pose (sub_822C9B38) 1.09 ms. Most of the pose's main-thread time is handing its
  bones to worker threads, about 54 jobs a fighter (sub_822CBA00: a 16-slot queue behind a lock and two events); the
  cloth and hair (sub_822CA898) are 0.04 ms. Left out of every frame run again (frame.cpp), the fight stayed byte for
  byte the same but for the poses and a second buffer the pose fills for the drawing (each player's +0x1B310, three of
  0x800 bytes taken in turn; now counted as pose, not in the checks' hash), and the KO came on the same frame: a frame
  run again 0.64 ms, a 4-frame rollback 3.1 ms on average (7.4 before). The next tasks (sub_82123400, a fighter's,
  0.15 ms; sub_8213B858, 0.12 ms) change the fight when left out: they stay. That held for Siegfried against
  Siegfried; with Aeon Calcos the next frame's skeleton took the missing pose into the fight (above, "Rollback": bone
  2's angle, P1+0x3078), so the pose is worked out in frames run again once more (2026-11-18). `SC5_RERUN_POSE=0`
  leaves it out (tests: what it costs).
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

## Created characters online (creation.cpp, creation_check.cpp)

docs\PROTOCOL.md 3.3, *Created characters*, has what the players see; this is what the game does.

**The save.** `savedata.bin` (0x22C200 bytes, plain) keeps 50 creation slots from 0x516C, 29,832 bytes apart: a
132-byte header, the record (TK_CreationCharacterDNA_st, 13,340 bytes, its schema sub_82226450) at +132, what Shoot
Picture was set to (104 bytes) at +13,472 and the picture it took at +13,576 (below, "The VS screen's card"). A
created character's header ends with +112 a number, +116 1 (a customized
fighter) or +120 1 (an original character), +124 and +128 0x01000000 (the screens test +128). In memory the save
object ([0x8274EF10]) keeps the image from +0x48 and the release table right after it (+0x22C248, "Unlocks"). Creation
and the select screen read the slots there as they go.

**The record**, by the game's schema (field, offset, size): versions 11.0 at 0 and 2; select no 4, variation 5, model
6, base type 7 (0 male, 1 female for an original character, 5 a customized fighter), facial 8, character type 9 (32 an
original character, listed under the Creation slot; a fighter's id: one of that fighter's costumes), voice 10, pitch
11, formant 12, flanger 13, eyebrow 14, DLC parts flag 15, style (RyuuhaType) 16, attack rate 17; the parts sets worn
20 (20 x u32, -1 none: `enCreationPartsSetID`); the body 100 (16 floats: shoulder breadth, five bone scales, the
blending rate and nine rates; then muscle type and level, matrix scale, figure mode); colours 168 (120 x
TK_CreationColorData_st: palette, a flag byte, row, column, saturation); break levels 1128 (4 x 288: 34 parts of
ModelData u32 and slot u8, then oppai swing, foot sound, hit material, equip weight); equipped category diff 2280;
parts sounds 2288 (20 x 4); parts coordinates 2368 (19 x 96: valid, parts id, parent bone, order, 7 vectors); weapon
4192 (700: style, variation, additional effect, 4 colours at +8, 4 stickers at +40, trace colours +408, effect +624);
update time 4892; break effects 4920 (3 x 16 x 12); stickers 5496 (84 x 92, CCreationBakingInfo: illustration id, map,
bake type, colour categories, guide region and id, OLT key +14, OLT index +16, OLT relation +17, slot, mirror, ten
floats of placement, 4 colours at +60); height 13224; facial motion name 13225 (5); a short name 13230 (15, empty in
Creation's); the name 13245 and as typed 13290 (45 each, UTF-8: the game turns `%`, `#`, `$` into their full-width
forms, sub_82349AE0); checksum 13336 (sub_822470B0: each word turned left by an amount the word before sets, XORed).
The game's repair pass (sub_82222B28: r3 the record, r4 a byte whose 0x10 makes it check the checksum and refuse a
wrong one) clamps the attack rate (80-200), pitch and formant (-50 to 50), flanger (0-2) and six body sizes (shoulders
0.08-0.15, upper 0.9-1.2, lower 0.9-1.1, face 0.9-1.1, hand 0.7-1.1, body 0.8-1.2). A ModelData is four bytes: parts
set type, table index, variation, segment. The 360 game did not check records from other consoles (sub_8229EF18 only
replaced one with a template of the same style when its player chose not to see created characters). The check runs
this pass too, on a copy in a buffer of the SDK's own heap (a save slot would not do: the creation manager copies
slots as it goes), after its own checks, and refuses a record the pass refuses or would change; its own checks hold
the same limits, so the pass is a second opinion.

**What the game has.** At the select screen, its object's [+1264] is the creation context (sub_8223CFE8,
sub_8223D0D0): +12 the parts sets of each base type (6 x {type, count, entries of 2,496 bytes}: the id at +0, the
category +4, the release +16, the members +20, a count and 14 of 9 models each; character exclusive +2452, forbidden
+2460), from its builder [+28] (+14788), whose +14860/+14864 are the weapons (count, entries of 160 bytes: style,
variation, release +12). In the test PC's game: 645 male and 677 female parts sets, 2,897 models, 203 weapons (189
released) in 29 styles, the palette (sub_8222D210's [0x8277B418]: +8 the last row, +10 the last column) 65 x 33.
Creation's illustrations (stickers: CreationIllustrationInfo, 204 bytes) are loaded in Creation mode only (the baking
guide manager, sub_8222C618, at a Creation object's +15048, its table at +140: count +52, entries +56): 446, each with
its own texture (OLT index, relation, key, which a record's sticker repeats) and release; the check carries them
(creation_stickers.inc, tools\py\creation_stickers.py from a dump of that table) and asks the game which of their
releases this PC has. sub_8246B318 answers whether a release id is released (unlocks.cpp opens everything but
downloadable content, state 3, and hidden entries). A parts set's models in the record must be among its base type's
(the body's 0x020000xx included).

**A model's segment.** Of a ModelData's four bytes the game loads the file the first three name (sub_822A4530, a model loader's
virtual function, its neighbour at 0x820C83E8 sub_822A46B0 giving the archive, 5 or 14: the model table at
[0x82A26918 + 80], a block per parts set type, its row the table index and its column the variation, an unknown column
taken as 0; 0x7F a downloadable model's package group and entry); the fourth is the
segment of that file a part shows, which the fight (sub_82147F48) and Creation (sub_821D0E28) turn on per break level,
and a negative one they never show. The parts tables list a file's segments one by one (the innerwear inner_006_f,
parts set 01050006: 1B080000 to 1B080003 in slots 6 to 9) and never segment 0xFF, yet Creation writes it for a piece
it keeps worn but shows nowhere: a player's new female characters were held out online for "model 1B0800FF is not on
this PC" (2026-11-20, 0.1.3; offline he saw and played them), every new one he made and a copy of his working one with
only its height changed, his own PC refusing what its own Creation had made. So a model passes when the tables list
it, or when it is one of their files with segment 0xFF (`KnownModel`: only that form, only a file this PC's tables have
for that character and base type; another negative segment or an unknown segment stays refused). Such a character
fights as Creation shows it: two games with a female character wearing 1B0800FF from break level 1 on, picked through
the pad, fought in sync (2026-11-24). The same goes for downloadable parts (UsesDlc takes any segment of a downloadable
file). The fight's list of break level 0's models (sub_82245CD0, filled by both) has room for 20, a model once however
many of its segments are worn, with its count right after it: a record whose level 0 wears more than 20 would write
over it, so the check refuses that too (Creation's own view would break on such a record first).

**Held out, said plainly.** A player's own character that fails the check is not only a log line (`slot N (name) stays
out online: <why>`): at the online select screen the game draws, with the result overlay's drawer (result_overlay.cpp,
large and high-contrast), "N OF YOUR CREATED CHARACTERS CANNOT BE PICKED ONLINE", each one's name and slot and why in
plain words (creation_check.h's PlainWhy: "it wears a part this PC's game does not have", "its saved data is
damaged"...), for the screen's first 20 s and whenever the player's cursor is on the Creation slot or on a fighter one
of them customizes; and the status line says how many (`starting match=<id> held_out=N`, PROTOCOL.md 3.4), which the
launcher's footer and log show as they show every state.

**A customized fighter's costumes.** A customized fighter (a regular fighter re-dressed in Creation: the fighter's own
character type, base type 5) wears that fighter's own costumes, which the select screen does not read. Its builder
keeps the creation data's archive ([+14868]'s object, its data at +12: a count of files, then their offsets;
sub_8223CE00 and sub_8223CE90 give a base type's data and its size, base types 0 to 4 files 0 to 4 and 5 file 11,
scrambled text that only the game's reader parses) and reads only base types 0 and 1 from it (sub_8223E1F0); Creation
reads 5 as well (sub_8224B3C8). So once at the select screen creation.cpp reads base type 5 with the game's own loader
(sub_8221E848: r3 a table {type, count, entries}, r4 the base type, r5 and r6 the data and its size) into a table in
the SDK's heap; the loader takes the entries from the game's heap (294 KB), which go back to it at once
(sub_8267A180). The test PC's game has 118 parts sets there, none downloadable: 89 of category 19 (+4), each a
fighter's whole costume (id 0x0013vvcc: cc the fighter's costume code, vv the costume; one model, parts set type 0, in
slot 31), and the 29 of category 21 that every base type's table has, the fighters' hair (0x020200cc, models
0x01nn0000 in slot 15). Each fighter's costume code is in the game's character table (sub_8235E5D0: 33 entries of 36
bytes from 0x82753828, the character type at +0 and the code at +4; Patroklos, 18, has 0x35, as his models 0x0035vv00,
his facial motion r035 and sub_8229EF18's templates carry). Only a fighter's first two costumes (vv 0 and 1, the
select screen's own two) load in a fight: with the third (vv 4, Patroklos's 0x00130435 and model 0x00350300) both
games gave up loading the fight after 40 s, so vv 4 and 5 are left out. A customized fighter must have base type 5,
wear one of its own two costumes in every break level (without one the game does not list it), wear nothing else but
the hair, and have a voice the game knows (sub_8235EE20 keeps 1-8, 11-23, 26, 34-36, 40-41, 47-66, 96-111 and 255 and
turns any other into 257, with which a customized fighter's fight never loads; an original character takes it).
Another fighter's costume or a creation body's parts are refused though the game copes with them (it loaded
Mitsurugi's costume on Patroklos), and so is another base type. `SC5_TEST_KNOWN_OUT=<file>` writes what the check
read, as text: sc5\tests\data\known.txt is the test PC's (2026-11-20: 1,322 parts sets and 2,897 models for original
characters, 28 fighters' own costumes), which creation_check_test checks the game's records against.

**What breaks the game.** Records with one area garbled and their checksum made right (tools\creationfuzz.py), loaded
in two games with the check off (`SC5_TEST_CREATION_UNCHECKED=1`), 2026-11-19: unknown models (ModelData) hang the
game for good at the costume panel; unknown stickers (ids, textures) keep the fight from loading (the stage select
waits forever); a weapon garbled whole crashes the game as the fight is set up (it reads the weapon's file through a
bad pointer, sub_82149AA0); garbage body sizes and rates (huge, NaN) hang both games as the fight starts. The check
refuses all of these. The game itself copes with the rest, each fought in sync: unknown parts sets (ignored), a weapon
variation its style lacks, the weapon's effect and trail bytes, a sticker's other bytes and huge or NaN placement,
parts coordinates (sub_82243AF8 skips a part not worn or a bone not allowed and clamps the offsets), colours
(sub_8224B8E8 and sub_8222D210 check the palette and the place), the small ids (select number, model, facial motion,
voice, eyebrow, height), break effects, parts sounds, the break levels' tails, the body's muscle and figure bytes,
names (shown as they are); a character type or style the game lacks is not listed at all. The check holds most of
these to Creation's limits too. The customized Patroklos (`--record patroklos`, 2026-11-20) did the same in every area
(unknown models and stickers stall the game, a weapon garbled whole crashes it, garbage body sizes hang it, a garbage
type is not listed, the rest fought in sync), and more: an unknown voice (the header's small ids, then the voice
alone) and his third costume made both games give up loading the fight, and with no parts at all he is not listed; his
second costume, any voice the game knows (none, a created character's, Mitsurugi's), another fighter's hair, another
fighter's costume and a creation body's base type fought in sync. The check refuses the first three, and of the rest
another fighter's costume and the other base type.

**The select screen's list.** A on the Creation slot or a fighter asks the screen's provider, the creation manager
([object + 1256], vtable 0x820BFC54), for that character's entries (sub_82222208, up to 50) and refuses an empty list
(sub_8230C828). The manager keeps its own copy of each slot (50 x 13,544 bytes from +4: the record, then what it works
out from it: the parts it loads at +13340 with their count at +13476, which its lists take as "used", the type +13480,
the DLC flag +13528), filled from the save as a VS Battle's menus come. A record placed during the select screen needs
its entry filled too: the game's own sub_82221978 (r3: a word holding the entry's address, r4: the record), called
from the frame's top on the main thread; an earlier match's entry is dropped (+13476 = 0) when its slot is cleared.
The costume panel lists a fighter's standard costumes first, then its created ones in slot order; the other PC's
record goes into slot 49 (player 1's) or 50 (player 2's), so each game knows where it is. The player's own records
that fail the check are taken out at the select screen, once its manager is there: the slot's header cleared and the
entry emptied. An entry left with parts stays on the costume panel, and the panel then no longer matches the slots: a
created pick of that fighter would be taken for a standard costume, its record not sent (seen 2026-11-20, the two
games showing different costumes). The manager is filled from the save again for the next match, the cleared header
notwithstanding, so SyncEntries empties those entries again each frame of the select screen, before any pick.

**The VS screen's card.** The VS screen (from its own copy of the picture, 0x82909C60 for player 2's, loaded by
sub_8218D570) and Creation's list (sub_82303428) draw a created character's card from the picture in its slot: a file at
+13,576 whose 16 bytes give its kind (byte 0), a byte the kind uses, then big-endian its size, width and height (+2,
+4, +6) and a checksum of its data (+8, sub_8218A848, checked only when byte 0 has 0x80 set), its data from +16.
sub_8218B998 loads it when the size is not 0 and sub_8218BA70 uploads it, by its kind (byte 0 without 0x80): 0 a
compressed file (`PTC+MSHM`, sub_8218BBB8; Creation's are this with 0x80, 172x256 in 16 KB at most), 1 a GPU texture
header and its data (sub_8218BE20), 2 the game's own DCT codec and 3 the same LZX-compressed (+12 its size), into a
texture of the form byte 1 names (sub_8218C038: 0 L8, 1 1:5:5:5, 2 5:6:5, 3 A8R8G8B8; only the forms sub_8218AA10
lists make one, so 1 and 2 never load). The codec (sub_8250F828): a 10-byte little-endian header (width, row pitch in
bytes, height, flags: bits 3-4 the channels less one, 0x20 each channel coded on its own, else 0x04 a 1-bit first
channel, 0x8000 the last two at half size, and a colour transform after unless 0x40), then per channel (sub_8250EE08) a
64-bit little-endian mask of the coefficients present in the codec's order (the table at 0x82712660: k -> row * 8 +
column), a byte added after the inverse DCT, 64 quantization multipliers, then each present coefficient's value for
every 8x8 block in turn (signed bytes, `0x80 n` repeating the last value n times); the inverse DCT is MPEG's reference
integer one (sub_82513CE8). The other PC's picture is not sent (PROTOCOL.md 3.3), so until 0.1.4 that player's card
was black (seen by the owner, 2026-11-20). Slots 49 and 50 now get a picture of SCV Online's own (creation.cpp, Card,
made once at the hold point): kind 2 into an A8R8G8B8 texture, 128x192 (rows of 32 texels), four channels each coded
on its own (alpha no coefficients and 255 added), navy and gold with a figure and a question mark drawn at 172x256 and
averaged 16 points a texel, through the standard DCT with JPEG's luminance quantization (14,503 bytes of the 16,256);
the VS screen stretches it over the card as it does Creation's (seen 2026-11-23: the card in the other PC's game, the
player's own picture in theirs). `SC5_DBG_PICLOAD=1` logs each picture loaded, where from, and how its upload ended;
`SC5_DBG_SLOTDUMP=<frame>:<slot>,...` writes those slots (0-based) at those frames to `SC5_TEST_DUMP_DIR` (how
Creation's own picture was found: shot and saved by a scripted Creation session).

## Frame data in practice (framemeter.cpp)

Over Training and Online Practice, a frame meter as Street Fighter 6's (the launcher's *Frame data in practice*,
`SC5_FRAMEDATA`, docs\PROTOCOL.md 3.1; F7 in the game hides or shows it). Per player a row of one-frame cells: startup
green, impact (the frames the attack can hit) red, recovery blue, hit or guard stun yellow, busy without an attack (a
step, a jump, a throw holding the other player, getting up) gray, free to act dark; each run's length at its last
cell. Above player 1's row and below player 2's, a line about the player's last move: *Startup 13F / Impact 3F /
Recovery 25F / Total 40F / Advantage +2F*. At the top, a panel with each player's last hit (its damage, and its part of
the attack's own damage: 120% is a counter hit), the combo's damage and hits, and the last attack's level (High, Mid,
Low, Special Low, Throw). A meter starts when either player leaves "free to act" and runs until both have been free for
20 frames; past 64 frames it wraps, a white line after the newest frame. Startup is SCV's impact frame (i13: the frame
the attack first can hit, the move's first frame being 1); advantage, once both are free again after a hit or a
guard, how many frames sooner the player could act than the other. The offline input delay (2 frames, above) comes
before a move starts and is not counted, as in published frame data.

It only reads guest memory, from a hook after the game's step (sub_824A25C0, every run of a frame), and calls no game
code but the step it wraps: it cannot change what the game computes, and two players may show it or not. Per fighter
object (0x8294AC10, 0x829A8740):

| field | what |
|---|---|
| +0x177A u16 | 1 nothing of its own (standing, walking, holding guard), 2 its own action (attack, throw, step, jump), 3 reacting to a hit (stunned, thrown, in the air, down), 6 guard stun |
| +0x0578 u32 | 1 while it may act (attack, guard, move), 0 while an action or a reaction holds it (the fighter's task, sub_82123400, keeps it from the busy byte +0x14C0) |
| +0x228BC u32 | its attack's record (heap), +0x14DB set while the attack runs; sub_8237F928 compares the move's frame (a float at `[+0x228C4] + 8`) with the record's impact window (+0x36 first, +0x38 last, from 0) and keeps the phase at +0x177C u16: 1 startup, 2 impact, 3 recovery (with no record it leaves it: a throw that caught keeps 2) |
| record +0x33 u8 | 0x80 a throw; without 0x02 high (it passes over a crouching fighter); 0x08 standing guard blocks it, 0x10 crouching guard (seen: high 0x49, mid 0x4B and 0x6F, low 0x37, special low 0x1B, throw 0x81; "Special Mid", 0x08 and 0x10 with 0x40, is a guess) |
| record +0x3A s16 | its damage before scaling |
| +0x2268C f32 | health (a hit's damage is what it loses in a frame; Training's and Online Practice's refills raise it) |
| 0x82949D54 u32 | the fight's frame count: one a frame while the fight runs, standing still while the game is paused, set back by a rollback |

A fighter's cell: stun from the frame a hit or a guard lands (+0x177A 3 or more); in its own attack (2, a record)
startup, impact, or recovery until +0x0578 says it may act; else gray while it may not act, dark while it may.

**How they were found**: Training, Mitsurugi against Siegfried, the presses scripted; both fighters' objects written
every frame and compared around each press (+0x177C went 1 for 12 frames, 2 for 3, 3 for 36 on Mitsurugi's A); the
code that writes the phase led to the attack record; the record's fields by comparing 14 moves' records with their
published damage and levels; guard stun with Training's dummy on the second controller (2P Settings, 2P "Controller",
START on that pad, guard held); the pause by the counters that stand still in Training's menu.

**Checked** against 8wayrun's SCV tables (2018): Mitsurugi's impact frames A 13, 6A 15, 3A 15, 2A 13, 1A 34, 4A 19,
B 15, 3B 18, 1B 14, K 13, 6K 15, 3K 15, 2K 17, 1K 15 and their damage 12, 18, 22, 12, 38, 28, 16, 32, 21, 14, 23, 18,
20, 16, all as published; advantage on hit A +2, B +2, K +2, 2A +8 and on guard A -8, B -8, K -8, 2A -6, as published
(a hit on the second impact frame, from a little farther, gives one frame more, as on the console). Siegfried's K i13
and A i18 in Online Practice. Online Practice, two games, 60 ms each way (2-frame rollbacks, 2026-11-23): every frame
run again went back to an earlier fight frame, and the final readings of 401 traced frames and all 6 exchanges logged
were the same in both games (22 and 24 rollbacks) and the same as at 40 ms. (A rollback of every frame in Training is
no check: Training's own state is not in the battle's snapshot, and the fight's count came back as 2 every fifth frame.)

**The overlay**: an ImGui dialog on the SDK's drawer (Sc5App::OnCreateDialogs), drawn on the UI thread from what the
game thread hands over each frame; Segoe UI Bold at 64 px (Sc5App::OnConfigureFonts; Arial Bold else: the runtime's own
font is 10 px); text 1/20 of the window's height (54 px at 1080 lines), 64 cells across 86% of its width, white with a
black outline over a dark panel; hidden while the fight stands still (Training's menus and help windows). In Training
the meter ends at 80% of the height (above its input history) and the panel is below Training's own displays; in Online
Practice at 86% and under the timer. F7 was the runtime's own key for its achievements overlay (SCV has none): that
bind goes, and F7 shows or hides the meter, with a notice for 2.5 s; the launcher keeps F6-F12 out of its key map.
Each exchange is logged once (`sc5 framemeter: fight frames 263-304: P1 startup 13 impact 3 recovery 25 total 40
advantage +2, damage 12 (100%) combo 12 (1 hit), High | P2 ...`); `SC5_FRAMEMETER_TRACE=1` (or `<from>:<to>`, fight
frames) logs every reading, a frame run again's marked `(again)`.

## Sound levels (sound.cpp)

The game's sound is CRI AtomEx with one configuration (the ACF, registered from memory; `SC5_DBG_SOUND=1` logs its
@UTF tables): 23 categories in two groups, and a cue is in one category of each group. Group 0 is what the game's
Sound options set: id 0 SE, 1 SYSTEMSE (the menus' sounds), 2 VOICE (the fighters and the announcer), 3 BGM. Group 1
(BGM_battle, SE_hit_weapon, Voice_chara, Voice_narration, ...) stays at 1. sub_82346FA8 applies the options (the
object at 0x82921808: +20 SE and SYSTEMSE, +28 VOICE, +16 BGM, levels 0 to 15 through sub_821D40B8) through
sub_821D4E30 (r3 the category, f1 the volume), which clamps the volume to 0-1, scales it by the category's base
level (0x82916188: 0.85, 0.90, 0.90, 0.60) and calls criAtomExCategory_SetVolumeById (sub_8257B428; nothing else
does). The BGM level is also kept at 0x827610B8 for the movies (CRI Mana, sub_825B25C0) and a BGM player.

The launcher's music, effects and voices levels scale what the game hands sub_8257B428 (music BGM, effects SE and
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
START too); AstralHome shows two windows that A closes, title update 3's notice of its changes ("The following changes
and additions have been made with the title update: Added Cepheus Store ... Character balance ...") and then the
first-visit window, and opens its menu with the cursor on Story: the hold point. Each window takes A only some seconds
after it comes (the notice once its text has scrolled and "Close" shows: an A 2 s after it came was not taken, one at
6 s was), so the autopilot goes by the main menu's own state, not by time: AstralHome's update (sub_82333088, slot 1
of its vtable 0x820CF990) switches on the object's +0x438. A fresh save goes 2, 3, 4 as it comes in, 21 (the notice
made), 22 (the notice up, until A), 23 (the notice's flag set in the save; without the first visit's flag, 18), 18
(the first-visit window made), 19 (up, until A), 20, then 6, the menu itself; a save that has seen both goes from 4
to 6, and a save from title update 1 days shows the notice alone. The autopilot presses A on 22 and 19 once they
have been up 2 s and every 2 s after until they go, and takes the menu once 6 has lasted a second (with a fresh save
about 10 s after AstralHome came, 2026-11-25). Title update 1 had no 21 to 23 (its 21 to 29 are 24 to 32 now).
AstralHome's creator (sub_82335A68) makes a 1088-byte object; its menu (at +8, vtable 0x820BF47C) keeps the depth at
+0x88 (0 the top level, 1 Offline Play's submenu), the top level's cursor at +0x94 (Offline Play 0, Story 2) and the
submenu's at +0x98 (Quick Battle 2, VS Battle 3); UP lowers them. Back from VS Battle the game opens the submenu again,
on VS Battle, so the autopilot goes by these cursors, not by a fixed path: Offline Play, A; VS Battle, A; VS Battle's
settings menu (Start Match, Player "Player VS CPU", Battle Count 3, Round Time 60, CPU Strength, Default), A on Start
Match; the select screen; the stage select; loading; the fight. After the last round the winner's pose, the Results
screen (A: Next), then a menu: Rematch ("fight again with the same character and settings": no loading, no intro;
online when both players chose REMATCH, above, "The round's call"),
Character Select, Return to Astral Home (it asks again, Yes / No, the cursor on No). A lobby's match (`choose=1`) has
its loser choose the next one on the Results screen (below, "The loser's choice"). START in a fight pauses it: Return
to Game, Move List, Options, Character Select, Return to Astral Home (the same question). `park` leaves a fight through
that menu, the result through the result's, the select screens with B.

**The select screen** (select.cpp). VS Battle's is set up by sub_82308520 (vtable 0x820CC658; sub_82303EA8 and
sub_8230DC78 set up the other modes'). Its object keeps each side's step at +0x9C + 4 side (0 choosing a character,
1 its costume, 2 and 3 done) and cursor at +0xAC + 12 side (x 0-7, y 0-3); sub_8230ACD0 moves a cursor (wrapping
both ways; the empty slots 19 and 20 make one crest in the third row that moves skip) and on A picks the slot y*8+x
(sub_8230C828; Random, slot 31, sub_8230CC28) unless +1600 + 4 slot says it is locked. The costume panel's label
goes through sub_8230C710(side, n, count) every frame it is up. "Player VS CPU": the first pad picks player 1's
character and costume, then player 2's. The 32 slots (the table at 0x82756020, 72 bytes each: +0 the game's
character id, +0x24 the release id, +0x26 the icon), row by row, as the screen names them:

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 0 | Patroklos (18) | Z.W.E.I. (14) | Leixia (22) | Natsu (20) | Ezio Auditore (31) | Raphael (9) | Viola (15) | Pyrrha (16) |
| 1 | Siegfried (3) | Hilde (11) | Xiba (21) | Ivy (4) | Cervantes (8) | Aeon (23) | Tira (10) | Nightmare (6) |
| 2 | Mitsurugi (0) | Maxi (1) | Yoshimitsu (24) | (empty) | (empty) | Dampierre (13) | Voldo (2) | Astaroth (7) |
| 3 | Creation | Alpha Patroklos (19) | Elysium (25) | Edge Master (5) | Algol (12) | Kilik (26) | Pyrrha Omega (17) | Random |

Devil Jin is not on it (in SCV a style for created characters only), though data\sc5.json lists him.

**Picks.** A side the start line names (a roster id, or `<character id>.<costume>` from a result line) the autopilot
picks on both PCs: the cursor one press toward the slot at a time, A, the costume panel to the costume (in a mirror
match the second side cannot have the first side's: after three presses that change nothing it takes what the panel
shows), A. A side named `-` its player picks on their own PC with their own controller (only moves, A and on the
costume panel B get through; A only on a slot both games can load, so Dampierre only when both players have his
package) while the other game follows: their cursor as it moves, then their character and costume (PICK packets,
repeated). B on the costume panel takes the player back to the characters, as on the console: the other game follows
only a whole pick, so it sees the cursor again and nothing else. Before, B was held back there too (0.1.3's report: "can't
press back anymore" on the created characters' panel), and a player whose panel held back A on every costume (each of
their created characters wearing downloadable parts the other PC lacks, say) could neither pick nor leave.
**Taking a pick back** (TakeBack). The select screen takes player 1's whole pick, then player 2's, in both games, and
the game's own B goes back one step at a time (from player 2's characters to player 1's costume panel, from a costume
panel to the characters; on player 1's characters it would leave the screen, so the autopilot never presses it there).
Player 2's last A ends the screen, so only player 1 has a whole pick to take back, while player 2 still picks: their B
makes their game ask (PICK phase 3, the request's number in the costume) and wait; the other game, unless its player
has finished, holds them back, presses B until player 1's side is at its characters, forgets the created character's
record (net::ForgetRemoteCreated, the slot cleared) and answers with a PICK for player 1's side, phase 3, the same
number; player 1's game then presses B to the characters too and the player picks again. Player 2 finishing first is
the no. A spectator takes a whole pick back once the player's game shows phase 0 or 1 for it. Only when both sides
are their players' (`-`): a side the launcher names is picked at once. Two games (2026-11-24, `SC5_SELECT_INPUT_P1/_P2`, the pad path):
player 1 picked a created character, took it back (player 1's game: side 1 3 -> 1 -> 0; the other's: side 1 3 -> 1 ->
0, the record forgotten), picked Alpha Patroklos instead, player 2 picked, and the match ran in sync to the result.
`SC5_TEST_LOCAL_PICK=<slot>[.<costume>]` stands in for the player in tests. A side named `c.<checksum>` (a ranked
winner's created character, or a lobby's rematch that starts again) its own PC's autopilot picks: the fighter, then,
once the costume panel is there, its created costume. The A that picked the fighter is held to its end while the panel
comes in: cut off there (until 0.1.4) its last frames came as a new A on the panel, which took the first costume, so
the player got the fighter's standard costume instead.

**The stage** must be the stage select's own choice in both games: with Random each game draws its own (seeding all
four generators in the frame of the press did not make the draws alike), and player 2's game taking player 1's stage
id at the battle setup was not enough (the fighters started elsewhere: the stage select sets up more than the id the
battle setup copies). The stage select (VS Battle's and Training's; select.cpp) is the object sub_82359E08 updates
every frame it is up: its cursor at +200 goes over 28 places, 0 Random and the 27 stages to its right, wrapping both
ways (sub_8235A1D0). Online the player whose turn it is picks on their own PC with their own controller (LEFT, RIGHT
and A; while A is down it goes alone, so a press as the cursor wraps cannot land on Random) and the other game
follows: its cursor where theirs is, then their stage (STAGE packets, repeated; a spectator's game from the feed).
Random is the match's seed's (A on it: place 1 + seed % 27, the same in both games; the game's own Random would draw
on each PC); a player who has not picked after a minute gets it too. Whose turn: player 1's in a match from a start
line, then the last match's loser's in a rematch through Character Select (player 1's after a draw; player 1's game
says whose in its STAGE packets). The chooser's game says which place it took as the game takes it (versus.cpp's
hook of the battle setup, sub_8244C6A8, calls the autopilot's OnSetup before player 2's game waits there for player
1's setup: player 1's game must have its pick first). The game's own Rematch keeps the stage; a rematch through
Character Select with `stage=keep` takes both cursors to the last match's place. Presses come 5 s after the stage
select came up (a press before it takes them was lost in one game and not the other). Offline (Training) the
autopilot goes RIGHT 1 + seed % 27 times from Random, A. `SC5_TEST_STAGE=<place>|random` stands in for the chooser
in tests (a press every half second).

**The loser's choice** (autopilot.cpp `OnResultChoice`, result_overlay.cpp; 0.1.4). After a lobby's match
(`choose=1`) both games stay at their result screens. Once the Results screen is up (12 s after the match's end), the
match's loser (player 1 after a draw: both games know the rounds) chooses REMATCH or CHARACTER SELECT with their own
controller: UP and DOWN (the stick too) move the choice, A takes it, on a panel the game draws over the Results
screen with the SDK's ImGui drawer, large and high-contrast (the frame meter's bold font, its lines a 22nd of the
window's height), which both players see with the loser's cursor on it.
The other game follows the cursor and the choice (CHOICE packets for the coming match, repeated). Both games write
`chose` and wait for the launcher's `rematch`, which takes them through the game's own menu as before: Next, the menu
150 frames later, DOWN for Character Select, A. The game's menu itself is not opened for the choice: with it open for
as long as the choice took, which the two games count a frame or two apart, a player's animation went on in one game
and not the other, and the rematch parted at its first check (2026-11-20: one game's snapshot had a block of player
2's object, +0x1C360, the other's had not; the same two games with the menu pressed on the same frames stayed in
step, as 0.1.3's rematches did). No choice in a minute: the launcher parks the game (Return to Astral Home).
`SC5_TEST_RESULT_CHOICE=rematch|select,...` stands in for the loser, one choice per match.

**The battle setup** (versus.cpp, sub_8244C6A8 copying the VS settings record into the battle setup; the record:
per side 88 bytes: +0 1 a player / 2 the CPU, +4 the input, +8 the rounds to win (Battle Count), +12 and +16 health
240.0; then +176 the round time, +208 the stage). Online, player 1's game sets the rounds to win from the start line
(best of N: (N+1)/2) and sends its stage, round time, rounds and its values of the generators the setup does not
seed (Park-Miller and TT800, 0x82938574-0x82924A87) and of the counters that started at boot; player 2's game waits
for them and takes them (SETUP). Both then seed the C library's rand and the Mersenne Twister with the match's seed.

**The result.** A match is over when a KO goes on to neither its replay nor "between rounds" (battle state 0xBC,
then 0x160). Each side's round points: in each player's object +0x75C, the rounds it won, and +0x764, the drawn ones
(a round that runs out of time even, and a double KO: both players get the point); +0x760 counts the lost ones. The
game ends the match once a side's points reach the rounds to win, and as a draw when a drawn round takes both there
(its Results screen: Draw for both players). +0x788 counts the points too, but a KO that ends the match plays its replay
first, and by the end only +0x75C has that round. The result line's `p1_rounds`/`p2_rounds` are the points and
`winner` the side with more, 0 a draw, which the server records for nobody (no casual win, no point in a set). Checked
2026-11-19, both games alike each time: a KO ending a best of 1 (0-1), of 3 (2-1) and of 5 (3-1), a round won on health
at the time limit (1-0), a won round then a drawn one (2-1), two drawn rounds (2-2) and a double KO (1-1). The line's
`p1_char`/`p2_char` are the character ids above with the costume index after a dot, a created character's
`c.<checksum>` ("Created characters online").

**Tests** (tools\linktest.py: two games on this PC through cmd.txt and status.txt, each with its own session folder
and an empty save, the scripted fight), 2026-11-17: boot to `ready` in 40 s; start to `in_match` in 31-38 s (31 s
since START skips the intros too, 2026-11-18); picks
by roster id and on the select screen (`-`, Xiba in costume 2 and Voldo; a Pyrrha mirror: costumes 1 and 2) in sync
to the result, also with 60 ms each way (rollbacks of at most 2 frames, 4.5 ms on average, 60 frames a second, none
over 20 ms); Rematch in sync; park from the result back to `ready` in 20 s, from a fight in 7 s; a second match after
a park; quit. Elysium against Dampierre, and Xiba against Voldo with 60 ms each way and picks on the select screen: the first
match, a rematch and the next match after a park all in sync.

**The second player.** The VS menus ignore the second pad: the select (sub_82164BB8, at P1's first pick) makes the
other side the CPU (VS settings: type 2 at +88/+92 of the select's record; the battle's player then has input mode 2,
`+628`, taking the AI's virtual pad, and pad index 0, `+432`), signed-in second user or not. versus.cpp makes the
fight's second player the second pad's after the battle setup (mode 1, pad 1) in every online match, and in tests with
`SC5_TEST_P2_HUMAN=1`.

**Training (practice)** is the Offline Play submenu's fifth item (Arcade, Legendary Souls, Quick Battle, VS Battle,
Training: the submenu's cursor 4; it opens on Quick Battle, 2). It goes straight to the select screen (the same one,
sub_82308520, every character open; the first pad picks the player's fighter, then the dummy), its stage select, the
round's call (no loading state between) and the fight with an endless clock. A fresh save shows its help windows
first ("Training", then the control pages), which A closes. START opens Training Options: Return to Game, Move List,
Switch Mode/State, System Settings, Reset Position, 2P Settings, Instant Replay, Display Settings, Cancel Changes,
Options, Switch Ring (greyed), Character Select, Return to Astral Home; the menu wraps, so UP from the top is the last
item, which asks again (Yes / No, on No). Display Settings: Life Bar, Mode/State, Advice Window, Damage, Display
Attack Properties, 2P Status, Display Control Help, Command Input History: no frame data (framemeter.cpp adds it:
above, "Frame data in practice"). `start mode=practice`
(the start line's p1 and p2: the player's fighter and the dummy) goes there and hands the controllers to the player
once the fight is up (`in_match match=practice`); `park` closes any help window (A), then START, UP, A, UP, A; the
player leaving for the main menu in the game is the hold point again (`ready`).

**The plain games** (sc5_app.cpp, autopilot::InitPlain): `SC5_TRAINING=1` takes a game from boot to Training's select
screen and `SC5_VERSUS=1` to VS Battle's with "Player VS Player", and the autopilot stops there: the players pick.
VS Battle's Player setting (Player VS CPU, CPU VS Player, CPU VS CPU, Player VS Player: RIGHT three times) offers
Player VS Player only with a second user signed in, which users.cpp then signs in (a local profile, user 1); the
select screen asks the second player to press START on their controller. Whatever the save, the autopilot closes the
main menu's windows as they come (above, "The menus": none with the player's own save once it has seen them) and sets
off once its menu is up. Should it get lost on the way, it stops and the game is the player's.

**Online Practice** (`mode=sparring`, sparring.cpp): VS Battle has no endless clock (Round Time 30, 45 or 60), so at
the top of every frame of the match in sync, reruns included, the round's clock (0x82949D3C: +0 the round's time, +0xC
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
takes the counters (the larger of the players'), player 1's battle state machine and whole fight there (the players'
block sync gives player 2's game only the blocks that differ; a spectator cannot ask), then runs every frame on both
players' inputs from the feed: never a guess, the neutral frames where the players had them, its own checks against
theirs. Far behind, it runs up to 120 frames a frame unseen (frame::RunAgain) until within 30 frames. Three games
(tools\linktest.py --watch, a relay in place of the launchers' feed): the spectator's frame log was byte for byte
the players', its 221 checks agreed with theirs, the same result line, also with 60 ms each way between the players
(22 rollbacks) and picks on the select screen; the feed was 254 KB.

**The match's stats**: `stats-<match>.json` in the session folder every 10 s and at the match's end (docs\PROTOCOL.md
3.4), from net::MatchStatsJson.

**Controls in a fight** (controls.cpp, 0.1.4). The battle's input task (sub_8213E538) turns each player's pad into the
game's buttons, A 0x1 (horizontal attack), B 0x2 (vertical), K 0x4 (kick), G 0x8 (guard), through sub_82172068(pad,
bits): the d-pad and START let through (0xBC00 of the pad manager's bits, 0x82784B80 + 128 a pad: +0 held, +4 newly
pressed), and for each of eight buttons pressed (their bits in the table at 0x8276FC1C: LT 0x2, LB 0x4, RT 0x10, B 0x20,
A 0x40, RB 0x80, Y 0x100, X 0x200) the combination its option says: the pad's layout at 0x82911094 + 12 a pad, eight
option indices into the table at 0x82768E44 (0 nothing, A, B, K, G, A+G, B+G, A+B, K+G, A+K, B+K, A+B+K, A+B+K+G: 13 of
the 16; A+B+G, A+K+G and B+K+G cannot be set). The layout comes from the save's Control Settings (0x82925630: 60 bytes
a pad, four 15-byte records, byte 12 the type chosen; a record's first 8 bytes in the menu's order A B X Y LB LT RB RT,
which sub_8213E538 copies in the table's order when the type changes): a fresh save has Type A, A G, B K, X A, Y B, LB
A+G, LT B+G, RB A+B, RT A+B+K. The stick clicks (0x1, 0x8) press nothing. The launcher's `[fight]` (PROTOCOL.md 3.1)
sets any combination on any of those buttons and the stick clicks. Online the netplay turns the player's own pad into
Type A's face buttons before it sends it (`controls::ToDefaultLayout`): every button `[fight]` sets presses the face
buttons of its combination and nothing of its own, which both games' Type A (an online game's save is fresh) turn into
that combination, and the spectators see the same. Equivalent to the game's own shortcut: the battle ORs the buttons
pressed and takes newly pressed ones from the change of that set, so a shortcut and its buttons at once come out the
same. Offline the hook of sub_82172068 takes player 1's pad (pad 0, the first controller and the keyboard; the
netplay not active): the bits of the buttons `[fight]` sets go out of what the game converts, their combinations in.
Only the battle's conversion sees any of it: the menus read the pads as they are. `SC5_DBG_FIGHT=1` logs the game's
buttons each pad newly pressed. Checked 2026-11-20: offline Training (LB A+B+K+G, LS K, X B, RT nothing; Y and RB the
game's own), and two games with 40 ms each way, player 1's LB K+G, LS A+B+K+G, X B: both games saw those presses on
the same frames, in sync to the result.

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

