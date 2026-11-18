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
- Fighters' textures made as a fight loads (`src\graphics\d3d12\command_processor.cpp`, `IssueCopy`). Soulcalibur V
  draws each costume texture on the GPU, mip by mip (the costume's colours over the texture's template), resolves
  the image into guest memory and compresses it: the large mips on the GPU (a pass draws the DXT blocks into a
  64bpp float render target and resolves them into the texture), the mips 16 blocks high or less on the CPU, which
  reads the image back from guest memory. A resolve writes only the GPU's copy of guest memory, so the CPU
  compressed zeros. Those mips came out black, and so did every fighter's eyes: a close-up draws them from the eye
  texture's 64x64 mip, and only their highlights (drawn over them) showed. While the game compresses textures (for
  60 frames after each compression pass), a resolve of an image a compression pass sampled is read back to the CPU
  at once. Fights read nothing back at once (only the auto exposure's small resolves, a frame late, below).
  `--readback_resolve=full` shows the same eyes, at the cost of a stall on every resolve of every frame.
- Those textures at render scale 2 to 4 (`resolution_scale`, the launcher's render scale; `IssueCopy`,
  `WriteUnscaledResolve`, 2026-11-18). A scaled resolve writes only the scaled copy of guest memory, and a texture
  any of whose memory one wrote is drawn whole from that copy: the CPU's mips never reached it, and the eyes stayed
  black. Nor were the compression passes' blocks a texture at a scale: the game's shader works out the block it
  draws, and which half of a DXT5 block, from the position interpolated at the pixel's centre, and is right only
  exactly there. Of a 1x pixel's host pixels one drew its block and the others a neighbour's or nothing (rainbow noise
  in beards and sashes), and at scale 3 even the middle one is just off, the interpolation not being exact (DXT5
  alpha halves wrong: Ezio's stubble gone, Mitsurugi's kimono see-through). Now, while the game makes these
  textures, every resolve of their passes is taken to 1x in the GPU's copy of guest memory and marked unscaled
  (`TextureCache::MarkRangeAsUnscaledResolved`), and a compression pass is drawn as at 1x into its render target's
  top-left (`IssueDraw`: the host viewport at its 1x size), so its blocks are the ones scale 1 draws, gathered from
  there (`src\graphics\shaders\resolve_unscale.cs.hlsl`; an image's 1x pixel is the average of its host pixels). The
  textures come out as at scale 1 and are drawn at 1x, as there: those made at scales 2 and 3 decode within 2/255 of
  scale 1's on average, and Mitsurugi's and Ezio's close-ups at scales 2, 3 and 4 look as at scale 1 (white eyes
  with irises, clean beards, sashes and kimono), at 2 with the ROV path too. An image a scaled resolve wrote
  before a load's first compression pass is taken to 1x when that pass samples it. Nothing more is read back, and a
  load takes as long as before (38 s either way to Mitsurugi's fight at scale 2).
  The scaled resolve layout (from the `resolve_*_scaled` shaders' code): the 16 bytes holding E 1x pixels of a row
  (4 at 32bpp, 2 at 64bpp) become scale_x x scale_y 16-byte chunks of E host pixels, chunk j x scale_y + r for host
  row r. A 1x pixel's host pixels lie together only for 64bpp at 2x. `resolve_downscale.cs.hlsl` (`readback_resolve`,
  the auto exposure's small readbacks) assumes they always do: right for a range's first pixel (the auto exposure's
  texel), not in general.
- The auto exposure (`src\graphics\d3d12\command_processor.cpp`, `ReadBackSmallResolve`, 2026-11-18). Every frame
  the game shrinks the scene's brightness on the GPU: a 64x64 resolve (16 KB), then three of a single 32x32 tile
  (4 KB each, 8-bit RGBA), the last 8x8. Its frame's step reads the last on the CPU: the first texel of the tile at
  physical 0x0F95A000 (guest 0xAF95A000), which `sub_8233E140`, a task of the step (so frames run again run it too),
  turns into the exposure it eases towards (a float at 0x827445F4) and its post effects' constants. A resolve writes
  only the GPU's copy of guest memory, so the game read 0 and over-exposed every scene: paler skin, silver-white
  hair on Mitsurugi (dark with grey streaks on the console), the sleeve prints washed out. Now a resolve of 8 KB or
  less (`kSmallResolveReadbackMax`) that the game makes again within two frames is copied into guest memory a frame
  late, from the newest of four copies the GPU has finished, never waiting for it (`d3d12_readback_small_resolves`,
  on by default; `=false` gives the old look). In a fight that is the three tiles and a 32x1 surface the GPU alone
  samples (0x0F973000): four 4 KB copies a frame, about 25 µs of the GPU thread a frame, no stall. A range resolved
  once (the textures made as a fight loads) is not read back; at render scale 2 to 4 the copy is downscaled first,
  as `readback_resolve` does.
  Two PCs can't part over it. A hardware watch found `sub_8233E140` the only reader of the texel, and `sub_8233F698`
  the only other reader of the exposure, for two drawing functions (`sub_8233F230`, `sub_82162200`). None of it is in
  the battle snapshot whose fight bytes the netplay hashes: two games, one reading back and one reading 0, had the
  same fight bytes at every check (they differed only in pose, pointer and heap bytes, which the hash leaves out).
  And two games stayed in sync over 16 matches, 10 of them with 40 ms each way (100 to 300 frames run again a
  match): RTV against ROV, render scale 1 against 2, the readback against none, and random bytes in place of the
  readback every frame on one of them. With the textures' fix above in too (the plugin of 03:52), 6 more with 40 ms
  each way: render scale 1 against 2 (3), 3 with ROV against 4 (2), and at 3 with ROV the readback against none (1).
- Render scale 3 and 4: a new command list after a texture loaded from guest memory, on both render target paths
  (`src\graphics\d3d12\command_processor.cpp`, `IssueDraw`, 2026-11-18). On an RTX 5080 with two or more games on
  it, games at scale 3 or 4 lost the device, `0x887A0006 DEVICE_HUNG`, in menus or fights, most often as a rematch
  began. With `--render_target_path_d3d12=rov` (the default on Intel GPUs) within minutes: 13 of 22 two-game tests
  (both games ROV, Mitsurugi against Hilde, 40 ms each way, three matches), one of them with only two light games of
  another session beside it (1.8 GB used of a 15 GB budget, so not short of video memory). Never at scale 1 or 2 (40
  game-minutes under the same load). The driver's log (System, nvlddmkm) has a GPU timeout, then every warp stopped
  at one place. DRED (breadcrumbs, with every operation of the command list described on the CPU) named the
  operation each time: the draw right after a texture's data was loaded from guest memory, or the one after it. Such
  a load copies the CPU-written pages into the shared memory, untiles each slice with a compute dispatch and copies
  it into the texture (`CopyTextureRegion`), each step behind barriers; on ROV one of the 32x32 cube maps the game's
  CPU rewrites during fights, or a 256x32 menu texture. The draw was often not even sampling it, the draws before
  the load with the same shaders and textures had completed, and loads from scaled resolves (no upload) never came
  before it. Why the GPU stops there is not known. The host render target path (RTV, the default on NVIDIA and AMD)
  does the same, far more rarely: in pairs going through eight rematches a run beside a second pair, an RTV game was
  lost once in 45 rematches, its DRED report naming the RTV draw of the loading screen right after a 256x32 DXT1
  texture's load (same driver log), where ROV without the change lost a game in 7 of 8 runs, each time while a
  rematch loaded or in its first 21 s. Now such a draw goes into a new command list on both paths (`EndSubmission`,
  `BeginSubmission`; not inside an occlusion query, which that would end); on RTV the render targets the render
  target cache bound for the draw are bound again in the new list (`RestoreCommandListRenderTargets`). One or two a
  frame in a fight, still 60 frames/s. With it no game was lost, and both games ended every match alike, in sync: on
  ROV 39 matches before (13 pairs, two or three at a time: 33 with both games at scale 3, 3 at scale 4, 3 with ROV
  against RTV) and 93 more (81 rematches, 54 of the matches with the installed plugin's code), on RTV 63 matches (56
  rematches, 18 with the installed plugin's code). RTV pairs also kept running through the 7 GPU resets of unfixed
  ROV games beside them. Not covered: scale 4 on RTV, and with 8 to 12 games on the GPU some games stalled while
  loading a fight. A scale-3 game on RTV commits about 7.5 GB of system memory (ROV 3 GB) and uses 5 to 7 GB of
  video memory: four RTV games and two ROV games ran this PC's 31 GB out (stalls of up to 90 s).

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
from the repository; the other diffs add the files this project created, which a plain diff leaves out):

```
{ git -C external/rexglue-sdk diff --ignore-submodules;
  git -C external/rexglue-sdk diff --no-index -- /dev/null include/rex/kernel/xam/ui.h;
  git -C external/rexglue-sdk diff --no-index -- /dev/null src/graphics/shaders/resolve_unscale.cs.hlsl;
  git -C external/rexglue-sdk diff --no-index -- /dev/null src/graphics/shaders/bytecode/d3d12_5_1/resolve_unscale_cs.h;
} > patches/rexglue-sdk-v0.10.0.patch
```

`resolve_unscale_cs.h` is built from `resolve_unscale.cs.hlsl` with the Windows SDK's `fxc` (the command is at the
top of the `.hlsl`).

(Last refreshed 2026-11-18: 37 files, with the new command list after a texture loaded from guest memory on both
render target paths.)

A change to the code generator needs `rexglue codegen` run again (`sc5\codegen.cmd`) before the game is
built; a change to the runtime needs only the game linked again.
