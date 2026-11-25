# What a player's PC needs to build the game: nothing but the package

Nobody ships the game's code: every PC translates its own copy from its own disc and title update 3 (the game's last
update, `TU_174Q21T_0000008000000.00000000000O2`, from the player's own Xbox 360; docs\TARGET.md; title update 1 until
0.1.5), and that translated C++ has to be compiled there. The package holds everything for that, so the launcher's Build game page works on a PC
with no developer tools at all: no Visual Studio, no CMake, no Python. MK9 Online's way (mk9-rollback `mk9\player`),
with the same SDK.

## How

The game is two parts:

| part | what | built |
|---|---|---|
| `games\sc5\host\sc5.exe` | everything that is ours: hooks, netplay, rollback, the launcher link, the app, linked with the C++ runtime | at release, on the developer's PC (`tools\build-player-host.cmd` -> `dist\host`) |
| `sc5game.dll` | only the game's translated code | on the player's PC, by `build-game.cmd` |

`games\sc5\build-game.cmd` (from `tools\player\build-game.cmd`; the launcher runs it with `SC5_GAME_DIR`,
`SC5_UPDATE_DIR` and `SC5_GAME_EXE`, docs\PROTOCOL.md 3.7):

1. copies `default.xex` and the title update's `default.xexp` from `SC5_GAME_DIR` (the launcher's `disc\`) into
   `project\xex`, where the package's manifest reads them, checks that `default.xexp` is the update this release is
   built on (its SHA-1 against `project\title-update.txt`, which `tools\package.ps1` writes from the launcher's
   `sc5.json`: "<SHA-1> <the update's number>"; Windows' `certutil` hashes it), and runs the package's `rexglue.exe
   codegen`. Another update (title update 1, left in place by an install of 0.1.5 or older) stops it with exit code 1
   and a line saying which update to choose on Build game: the launcher refuses it before that, this is for a run by
   hand;
2. compiles the translated code with the package's clang and links `sc5game.dll` with its lld (`build.ninja`, run by
   its ninja);
3. puts `host\sc5.exe`, `sc5game.dll` and the runtime's DLLs (the SDK's, Microsoft's C++ runtime) where
   `SC5_GAME_EXE` says, and writes `built.txt` (the game's release it was built from).

`sc5game.dll` needs no C++ library, no Windows headers and no CRT. It is compiled against

- `include\` here: tiny stand-ins for the C++ library's headers and for the SDK headers that drag in host code. They
  hold only what the translated code uses (put first on the include path, with `-nostdlibinc`);
- the SDK's own `rex/ppc/*` headers and simde;
- clang's own headers (intrinsics, `stdint.h`);

and linked (`/NODEFAULTLIB /NOENTRY`) against import libraries that are all made at release: `sc5.lib` (the hooks, from
`sc5.exe`), `rexruntime.lib` (the SDK's), and three tiny ones from the `.def` files here for the few C runtime functions
the translated code calls (`sqrt`, `sqrtf`, `log2f`, `fma` without AVX2, and the guest lock's `_Mtx_lock` and
`_Mtx_unlock`; a new one shows up as an undefined symbol in the player's link: add it to the `.def`).

The two parts call each other: `sc5.exe` calls the original game functions it hooks (exported by the DLL, listed by
`tools\player\boundary.py` from `sc5.exe`'s own objects just before it links), and the DLL calls the hooks, the midasm
hooks and the trap log (`guestlog.cpp`, the translated code's only log lines), all exported by `sc5.exe`. Windows
resolves a DLL's imports from the exe that is already loaded, as long as the exe keeps its name: `build-game.cmd` refuses
an `SC5_GAME_EXE` not named `sc5.exe`.

**A hook is only a weak-symbol override**: the translated code defines each game function as a weak alias of the
original, and a weak name does not pull a member out of a library. `build.ninja` gives the linker `sc5.lib` with
`/WHOLEARCHIVE`. Without it the DLL still links and runs, with every hook silently left out: after any change here check
that `sc5game.dll` imports all of them (`llvm-readobj --coff-imports sc5game.dll`, or `check_package.py`: 55 functions
from `sc5.exe` on 2026-11-17, the 25 hooked `sub_*` among them, 22 midasm hooks, 7 Xam imports, `Sc5GuestLog`).

**The image comes from the DLL**: `sc5.exe` starts the game with `sc5::GameImage()` (`src/sc5/gameimage.h`, from
`Sc5GameImage` in `game_stub.cpp`). Host code must never name `PPCImageConfig` or `PPCFuncMappings`: they are the DLL's
data, which the ready-made exe cannot link (`boundary.py` stops its build when it does).

## The same files on every PC

Codegen splits the game's functions into files by size and keeps that split (`codegen.partition.json`) from one run to
the next. The package carries the release's (`project\generated\default\codegen.partition.json`), so every player's
codegen writes exactly the developer's files, the ones `build.ninja` names (`build-game.cmd` checks the list against
`project\sources.cmake`); a fresh split happened to give the same 102 files on 2026-11-17, but other functions in them,
and a later config could change their number. An update that changes the config then compiles only the files whose
functions changed.

## Why this is the same game

The translated code's machine code was compared function by function with a normal build's (`tools\player\
compare_objs.py`; all 103 files, 23,460 functions, 2026-11-17), the player's compile given the normal build's exception
flags, no `-march`, and logging macros big enough that `ppc_trap` stays out of line. 856 differ, all expected:

- 737 with a guest lock (`mfmsr`/`mtmsrd`): Microsoft's `std::recursive_mutex` and atomics against the stand-ins' (no
  exception cleanup, the unreachable "mutex error" paths gone) and its full memory barriers, a call to
  `atomic_thread_fence(5)`, where the stand-in has `lock or [rsp]`;
- `ppc_trap` (96 copies) and the 21 functions that call it: the guest traps, which only log, log through `sc5.exe`;
- one function that calls `log2f` through the import thunk instead of `__imp_log2f`, and one with other register choices.

When the SDK or the codegen changes, redo this.

## Checking a package

- `tools\player\test-clean-build.ps1 -Game <package>\games\sc5 -Disc <folder with default.xex and default.xexp>` builds
  it as a PC with nothing installed would, as the launcher runs it; add `-March x86-64-v2` for what a CPU without AVX2
  builds (the libcalls differ). A `-Disc` folder with title update 1's `default.xexp` (`orig\x360\tu1`) must stop at
  once with the line naming title update 3.
- `tools\player\check_package.py <package>` fails if anything needs a DLL outside the package and Windows, or if a built
  `sc5game.dll` left a hook out.
- `tools\player\test-boot.ps1 -Exe <package>\games\sc5\sc5.exe -Work <empty folder> -Disc <disc> -Update <update>`
  starts the built game windowed as the launcher does (`SC5_NET=1`, a fresh save) and waits for `ready`, the main menu.

## Folders

Every path in the compile is relative to `games\sc5`, also in the depfiles ninja reads (clang's own headers through
`-resource-dir`; no precompiled header, whose users' depfiles name its headers by absolute path): ninja cannot read an
absolute path with `&` in a depfile, took every header for missing and compiled everything again on every run. Paths
from outside (`SC5_GAME_DIR`, `SC5_GAME_EXE`) are only ever quoted or printed through a `for` variable. `package.ps1`
stamps `sc5.exe` and `rexglue.exe` with `utf8.manifest`: both keep paths in narrow strings, so without it a folder with a
letter outside the system's code page (a Windows account's name, say) stops codegen and the game at once. Tested in
`Jeux & Démos\SC5-Online (1)` and, moved, in `Игры & Démos\SC5-Online (1)`, the game's disc, saves and log there too.

## Updates

`build-game.cmd` costs a second when nothing changed: codegen sees its inputs are the same, ninja has no work, and the
new `sc5.exe` is copied in. A release that changes only `sc5.exe`'s code does not make anyone compile the game: running
the script again puts it in place (`built.txt` names the release a game was built from: after an update the launcher
builds the game again when it differs, and starts no game until then). A release that moves to another title update
(0.1.5's title update 1 to title update 3) makes every player choose the new package on Build game first: the
launcher asks for it, and the compile that follows translates the whole game again. A new hook is a relink. A change to the headers here compiles everything again,
a change to the codegen's config the files whose functions changed.

On this PC (Ryzen 7 9800X3D, 16 threads): codegen 6 s, the whole compile 25-30 s, about 190 MB per clang, 2.4 GB for 16
at once; `sc5game.dll` 37 MB.
