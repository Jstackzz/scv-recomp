#pragma once
// The character select screen (docs\GAME.md, "The character select"): where its cursors are, what each side has
// picked, the roster's slots, for the launcher's autopilot and the picks passed between two games online.
#include <cstdint>
#include <string>

namespace sc5::select {

constexpr int kSlots = 32;  // 8 across, 4 down
constexpr int kColumns = 8, kRows = 4;
constexpr int kRandomSlot = 31, kCreationSlot = 24;

// the select screen's object while one is up (its setup ran, the VS sequence still on), else 0
uint32_t Object();
// frames since the present select screen was set up
uint64_t Age();

struct Cursor {
  int x = 0, y = 0;
  int Slot() const { return y * kColumns + x; }
};
// side 0 (player 1) or 1 (player 2)
Cursor CursorOf(int side);
// where the side is: 0 choosing a character, 1 choosing its costume, 2 and 3 done
int Step(int side);
// the costume the side's panel shows (0 the first), and how many the character has (0: no panel); -1 before any
int Costume(int side);
int Costumes(int side);
// a slot that can be picked on this PC (a character, unlocked, or Creation's list; not Random, not an empty one)
bool Pickable(int slot);
// the slots pickable here, one bit each
uint32_t PickableMask();

// The stage select (VS Battle's and Training's: sub_8234C028 updates its object every frame it is up): its cursor
// at +200, a place of 28 (sub_8234C3F0 wraps it both ways): 0 Random, then the 27 stages to its right
constexpr int kStagePlaces = 28;
uint32_t StageObject();  // 0 when none is up
int StageCursor();       // -1 when none is up

// the roster (the launcher's ids, data\sc5.json) and the game's own character ids (the slot table at 0x82746018)
int SlotOfRosterId(const std::string& id);   // -1 unknown
int SlotOfCharacter(int character);          // -1 none
int IdOfSlot(int slot);                      // the slot table's character id, the Creation slot's too (32); -1 none
int CharacterOfSlot(int slot);               // the game's id, -1 for none
const char* NameOfSlot(int slot);

}  // namespace sc5::select
