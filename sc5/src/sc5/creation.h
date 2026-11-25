#pragma once
// Created characters online (creation.cpp): the save's 50 creation slots, the player's own characters brought into an
// online game's fresh save, and the other player's record (checked first) in slot 50.
#include <cstdint>
#include <string>
#include <vector>

#include "sc5/creation_check.h"  // kOriginal (an original character's type), kFighterBase

struct PPCContext;

namespace sc5::creation {

constexpr uint32_t kSlots = 50;
constexpr uint32_t kSlotSize = 29832;    // a 132-byte header, the record, its picture
constexpr uint32_t kRecordAt = 132;      // the record in a slot (the battle's per-side copy has it there too)
constexpr uint32_t kRecordSize = 13340;  // TK_CreationCharacterDNA_st
constexpr uint32_t kPictureAt = kRecordAt + kRecordSize;
// the other player's created character: slot 49 for player 1's, 50 for player 2's (a spectator places both)
constexpr uint32_t RemoteSlot(int side) { return kSlots - 2 + uint32_t(side & 1); }

using Record = std::vector<uint8_t>;  // kRecordSize bytes, as the save keeps them (big-endian)

// the save image in memory (the save object's, [0x8274EF10] + 0x48; 0 before the save is in)
uint32_t SaveImage();
// slot n's first byte in memory (0 when there is no save image)
uint32_t Slot(uint32_t n);
// slot n holds a created character
bool Used(uint32_t n);
// the record's character type (+9): 32 an original character, else the fighter it customizes
int TypeOf(const uint8_t* record);

// Once at the main menu: the player's own created characters from their own save (SC5_CREATION_SAVE, read only, never
// locked) into this game's save in memory, each one checked; slot 50 stays free for the other player's. Tests:
// SC5_TEST_CREATION_SLOT=<file>[,<file>...] (whole slots) into the first slots.
void OnHoldPoint();

// The select screen's pick of a created character: the slot of the `costume`-th entry (from 0) of `character`'s
// costume panel (`costumes` entries; the Creation slot's character 32 lists the original characters), -1 for a
// standard costume. A fighter's created costumes come after its standard ones, in slot order.
int SlotOfPick(int character, int costume, int costumes);
// slot n's entry (from 0) in `character`'s costume panel of `costumes` entries; -1 when it is not there
int CostumeOfSlot(int character, uint32_t n, int costumes);
// slot n's record
bool ReadRecord(uint32_t n, Record& out);
// the player's own slot (not the other PC's) whose record has this checksum (+13336); -1 none
int SlotOfChecksum(uint32_t checksum);
// a record's checksum as it keeps it
uint32_t ChecksumOf(const Record& r);

// What this PC's game can load for a created character (creation_check.h's Known): read once from the game's own
// tables at the select screen (the creation context, [select object + 1264]: each base type's parts sets with their
// models, the weapons of each style), keeping only what the game says is released here (sub_8246B318, unlocks.cpp:
// downloadable parts only with their package). Main thread (it asks the game); false while the tables are not there.
bool ReadKnown(PPCContext& ctx, uint8_t* base, uint32_t select_object);
// true once ReadKnown has read the tables and DropRefused has taken out what fails the check: created characters are
// picked only from then on
bool KnownRead();
// a fingerprint of this PC's downloadable parts (the same on two PCs with the same ones; 0 before ReadKnown), and
// whether a record uses any of them: such a record is picked only when the other PC's fingerprint is the same
uint32_t DlcFingerprint();
bool UsesDlc(const Record& r);
// Once Known is read and the creation manager is there (0: not yet, it waits): the player's own created characters
// that fail the check out of this game's lists (their slot and the creation manager's entry, which SyncEntries keeps
// empty), so they cannot be picked. SC5_TEST_CREATION_UNCHECKED keeps them (tests).
void DropRefused(PPCContext& ctx, uint8_t* base, uint32_t manager);

// The player's own created characters this online game leaves out (OnHoldPoint's ImportOwn and DropRefused): the slot
// (from 0), the character type, the name, and why in plain words for the player (the log has the check's own words).
// The select screen's overlay shows them (autopilot.h, HeldOutNow), the status line counts them. Any thread.
struct HeldOut {
  int slot = -1, type = -1;
  std::string name, why;
};
std::vector<HeldOut> HeldOutList();

// Everything a record from another PC must pass before anything loads it (creation_check.h's CheckRecord against this
// PC's Known): "" when it passes, else why not
std::string Check(const Record& r);
// On top of Check, for a record that passed it: the game's own repair pass (sub_82222B28, what a console runs on a
// save's records, here checking the checksum too) on a copy in a buffer of the SDK's own heap, which nothing of the
// game reads (not a save slot: the creation manager copies slots as it goes). "" when the pass takes the record as it
// is; why not when it refuses it or would change any byte. Main thread.
std::string GameRepairCheck(PPCContext& ctx, uint8_t* base, const Record& r);
// a player's record from the other PC (checked) into their side's slot (RemoteSlot); false when it does not pass
bool PlaceRemote(int side, const Record& r);
// both remote slots empty again (a new match), or one side's (its player took the pick back)
void ClearRemote(int only = -1);

// The creation manager (the select screen's [object + 1256]; vtable 0x820BF BDC) keeps its own copy of each slot, 50
// entries of 13,544 bytes from +4: the record, then what it works out from it (a count of break-level parts at +13476
// that its lists take as "used", the type at +13480, the DLC flag at +13528). It copies the save's slots as a VS
// Battle's menus come, so a record placed during the select screen needs its entry filled too: the game's own
// sub_82221978 does it from the record (main thread, from a frame's top). False without a manager.
bool FillEntry(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t n);
// a remote slot's entry that its save slot no longer backs (an earlier match's), and a refused own one, listed no
// more: each frame of the select screen, before any pick
void SyncEntries(uint32_t manager);

}  // namespace sc5::creation
