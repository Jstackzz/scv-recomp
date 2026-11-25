# Target build

The game is built from the World disc's `default.xex` with title update 3, the last title update of the Xbox 360
release. Both players of a match must run the same code, so the launcher takes this disc and this update only
(`launcher/data/sc5.json`; docs/PROTOCOL.md 3.7). Until 0.1.5 it was title update 1 (below, as history).

The disc was identified 2026-11-16 and title update 3 on 2026-11-25, with `tools/py/x360.py`, from the files in
`Desktop\Soulcalibour V recomp`.

## Disc

`Soulcalibur V (World).iso`, 8,738,846,720 bytes (XGD3, game partition at 0x2080000).
SHA-1 `43dbb40b9ef4e5a651623ada40ed4eb2066017d5`, the Redump entry's.

`default.xex`: 8,536,064 bytes, SHA-1 `dfe4d5e1e9198bc9df6db46ddf2bdf2a5cbf3f80`, original PE name `game.exe`,
encrypted, basic compression. Execution info: title `4E4D083D`, media `165FAD4F`, version 0.0.0.2, disc 1/1.

Top level of the game partition: `$SystemUpdate/` (dashboard update, not needed), `AvatarAssetPack`,
`cdata.cpk`, `cpudata.cpk`, `creation.cpk`, `default.xex`, `demo.cpk`, `dna.cpk`, `human.cpk`, `motion.cpk`,
`nxeart`, `stage.cpk`, `stream/` (CRI ADX2 `.awb` banks, HCA streams: decoded by the game on the CPU, no XMA),
and the rest of the CPKs (same layout as the PS3 release).

## Title update 3 (what the game is built on)

`TU_174Q21T_0000008000000.00000000000O2` (the last two characters are the capital letter O and the digit 2),
1,208,320 bytes, STFS `LIVE`, content type `000B0000` (title update), title `4E4D083D`, media `165FAD4F`, display
name "SOULCALIBUR V Title Update".

| file | bytes | SHA-1 |
|---|---|---|
| `default.xexp` | 876,544 | `e8f8eb269aed296aec32549a79b689e31cde14dc` |
| `patch.cpk` | 268,288 | `c41ceee22a4bea04d8508f34df15039450e34f02` |

`default.xexp`: a delta patch from version 0.0.0.2 to 0.0.3.2, source digest
`b715f6840f46965cf1a601723a3b023d17ab2340` (the same disc executable as title update 1's). Extracted copies are in
`orig\x360\tu3`.

The public list of the game's Xbox 360 title updates has three: 2012-02-01, 2012-03-22 and 2012-06-06. Number 3 is
the last, so no later update can split the players again. Title update 3 patches the disc's executable directly (a
delta from 0.0.0.2, as title update 1 was), so a player needs only this one package, not the ones before it.

Where a player gets it: an Xbox 360 downloads the game's newest title update from Xbox Live when it starts the game
online, and keeps it on its hard drive, usually in `Content\0000000000000000\4E4D083D\000B0000\`. The player copies
that file from their own console to the PC and picks it on Build game, step 2. Nothing in SCV Online downloads it.

## Title update 1 (until 0.1.5)

`TU_174Q21T_0000008000000.0000000000082`, 872,448 bytes, STFS `LIVE`, female, content type `000B0000`
(title update), title `4E4D083D`, media `165FAD4F`, display name "SOULCALIBUR V Title Update".

| file | bytes | SHA-1 |
|---|---|---|
| `default.xexp` | 759,808 | `47a09662fd3fe376d283208fcd8a31eee819c736` |
| `patch.cpk` | 47,364 | `0b2b9d30ce56c02fafa91a0f4fbdaeb23d441a93` |

`default.xexp`: module flags 0x50 (patch), delta compression, delta patch descriptor: source version 0.0.0.2,
target version 0.0.1.2, source digest `b715f6840f46965cf1a601723a3b023d17ab2340`. Extracted copies are in
`orig\x360\tu1`.

SCV Online 0.1.0 to 0.1.5 were built on it. The launcher still knows its hashes (`older_title_updates` in
`launcher/data/sc5.json`) so that a player who picks it, or whose game was built on it, is told in plain words that
this is title update 1 and that title update 3 is needed, with that file's name.
