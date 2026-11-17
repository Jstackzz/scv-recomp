#pragma once
// Everything on the disc unlocked, without touching the player's save (the way the Tekken Tag Tournament 2 port
// opens its whole roster). The game keeps a release state for each of its 7426 unlockable things (characters,
// stages, music, creation parts, titles: the "release table", 4 bits each, [[0x8273EF10]] + 0x22C248) and asks for
// one through sub_8245BED0(table, id): 2 released, 0 locked (unlocked by playing), 3 downloadable content (released
// only while a DLC package the player installed grants it: the DLC manager at 0x8276F430), 4 hidden. This layer
// answers "released" for the locked ones and leaves downloadable content and hidden entries to the game: paid DLC
// stays the player's own (docs\PLAN.md).
//
//   SC5_UNLOCK_ALL=0   the game's own unlocks only (what the save has earned); anything else or unset: all of them
namespace sc5::unlocks {

// reads the environment (once, at startup)
void Init();
bool Enabled();

}  // namespace sc5::unlocks
