# The ReXGlue SDK

SC5 is recompiled and run with ReXGlue v0.10.0 (https://github.com/rexglue/rexglue-sdk, tag `v0.10.0`,
commit `c94f5eb`) with this project's changes on top, in `patches\rexglue-sdk-v0.10.0.patch`:

- MK9 Online's patch (mk9-rollback `patches\rexglue-sdk-v0.10.0.patch` as of MK9 0.5.11, 2026-11-12): file
  reads into write-protected guest pages, the critical section hook rollback uses, GPU fixes, logging limits and
  the rest that MK9's `docs\GAME.md` lists. SC5 needs the same rollback hooks, so it starts from there.
- `dcbz` clears 128 bytes (`src\codegen\builders\system.cpp`). The Xbox 360's CPU has no 32-byte cache line
  mode: `dcbz` clears the whole 128-byte line, like `dcbz128` (Xenia Canary 72ce130, the commit from which
  Soulcalibur V runs there). SC5 clears memory with `dcbz; addi rX,rX,128` loops (630 of them); with a 32-byte
  clear three quarters of every line keep old data and the game never leaves its boot sequence (black screen).

## Where it lives

| folder | what |
|---|---|
| `external\rexglue-sdk` | the SDK source: v0.10.0 with its submodules and the patch applied (not in git) |
| `external\rexglue-sdk\out\build\win-amd64` | its build (Ninja Multi-Config, Release) |
| `external\sdk` | the installed SDK that `sc5\build.cmd` uses (`CMAKE_PREFIX_PATH`) and that ships to players: `bin\rexglue.exe` (codegen), `rexruntime.dll`, `rexgpu-xenos.dll`, headers, libraries |

## Set it up on a new machine

```
git clone https://github.com/rexglue/rexglue-sdk external\rexglue-sdk
git -C external\rexglue-sdk checkout v0.10.0
git -C external\rexglue-sdk submodule update --init
git -C external\rexglue-sdk apply ..\..\patches\rexglue-sdk-v0.10.0.patch
tools\build-sdk.cmd
```

On Windows, git checks out libmspack's 15 symlinks (`thirdparty\libmspack\cabextract\mspack\*`) as one-line
files that name their targets, and the build stops at `lzxd.c`. Put each target's content in its place first
(as MK9's tree has it):

```
cd external\rexglue-sdk\thirdparty\libmspack
git ls-files -s | awk '$1=="120000"{print $4}' | while read f; do cp "$(dirname "$f")/$(cat "$f")" "$f"; done
```

`tools\build-sdk.cmd` builds everything (Release) and installs it into `external\sdk`;
`tools\build-sdk.cmd rexglue` builds the codegen tool alone. After changing the SDK, refresh the patch (Git Bash,
from the repository; the second diff adds the header this project created, which a plain diff leaves out):

```
{ git -C external/rexglue-sdk diff --ignore-submodules;
  git -C external/rexglue-sdk diff --no-index -- /dev/null include/rex/kernel/xam/ui.h; } > patches/rexglue-sdk-v0.10.0.patch
```

(Last refreshed 2026-11-16: 33 files, with the message box answer, media verification and the sign-in XUID fix.)

A change to the code generator needs `rexglue codegen` run again (`sc5\codegen.cmd`) before the game is
built; a change to the runtime needs only the game linked again.
