# Target build

Identified 2026-11-16 with `tools/py/x360.py` from the files in `Desktop\Soulcalibour V recomp`.

## Disc

`Soulcalibur V (World).iso`, 8,738,846,720 bytes (XGD3, game partition at 0x2080000).
SHA-1 `43dbb40b9ef4e5a651623ada40ed4eb2066017d5`, the Redump entry's.

`default.xex`: 8,536,064 bytes, SHA-1 `dfe4d5e1e9198bc9df6db46ddf2bdf2a5cbf3f80`, original PE name `game.exe`,
encrypted, basic compression. Execution info: title `4E4D083D`, media `165FAD4F`, version 0.0.0.2, disc 1/1.

Top level of the game partition: `$SystemUpdate/` (dashboard update, not needed), `AvatarAssetPack`,
`cdata.cpk`, `cpudata.cpk`, `creation.cpk`, `default.xex`, `demo.cpk`, `dna.cpk`, `human.cpk`, `motion.cpk`,
`nxeart`, `stage.cpk`, `stream/` (CRI ADX2 `.awb` banks, HCA streams: decoded by the game on the CPU, no XMA),
and the rest of the CPKs (same layout as the PS3 release).

## Title update 1

`TU_174Q21T_0000008000000.0000000000082`, 872,448 bytes, STFS `LIVE`, female, content type `000B0000`
(title update), title `4E4D083D`, media `165FAD4F`, display name "SOULCALIBUR V Title Update".

| file | bytes | SHA-1 |
|---|---|---|
| `default.xexp` | 759,808 | `47a09662fd3fe376d283208fcd8a31eee819c736` |
| `patch.cpk` | 47,364 | `0b2b9d30ce56...` |

`default.xexp`: module flags 0x50 (patch), delta compression, delta patch descriptor: source version 0.0.0.2,
target version 0.0.1.2, source digest `b715f6840f46965cf1a601723a3b023d17ab2340`.

Whether a later title update exists for the Xbox 360 release is not known; the PS3 release had three patches
(1.01-1.03). Both players must run the same one; the builder pins this one.
