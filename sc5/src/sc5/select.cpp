// The character select screen, see select.h.
//
// The select object (VS Battle's: set up by sub_822FBDF8, vtable 0x820CBD50; the other modes' by sub_822F72C8 and
// sub_82301500) keeps, from what the game's own code reads and writes (sub_822FE558, a side's cursor and its pick;
// sub_823000B0, the pick; sub_822F5E38, the grid):
//   +0x9C + 4 side   the side's step: 0 choosing a character, 1 its costume, 2 and 3 done
//   +0xAC + 12 side  the side's cursor: x (0-7), y (0-3), a repeat timer; sub_822FE558 moves it (with wrap-around, and
//                    past the two empty slots 19 and 20, which make one crest in the third row)
//   +1600 + 4 slot   0: the slot can be picked; 1 locked, 2 Dampierre's without his package (sub_822F5E38)
// The costume panel's "n / count" label goes through sub_822FFF98(side, n, count) every frame it is up.
// The slots, row by row, are the table at 0x82746018 (72 bytes each: +0 the game's character id, +0x24 the release
// id, +0x26 the icon's name); the names below are the ones the screen shows for them.

#include "sc5/select.h"

#include <atomic>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"
#include "sc5/game.h"

namespace sc5::select {
namespace {

constexpr uint32_t kSlotTable = 0x82746018, kSlotSize = 72;

struct Slot {
  const char* roster;  // the launcher's id (data\sc5.json), nullptr for none
  const char* name;
};
// slots 0-31, the screen's names (2026-11-17, cursor swept over every slot)
constexpr Slot kSlotNames[kSlots] = {
    {"Patroklos", "Patroklos"},   {"ZWEI", "Z.W.E.I."},      {"Leixia", "Leixia"},
    {"Natsu", "Natsu"},           {"Ezio", "Ezio Auditore"}, {"Raphael", "Raphael"},
    {"Viola", "Viola"},           {"Pyrrha", "Pyrrha"},      {"Siegfried", "Siegfried"},
    {"Hilde", "Hilde"},           {"Xiba", "Xiba"},          {"Ivy", "Ivy"},
    {"Cervantes", "Cervantes"},   {"AeonCalcos", "Aeon"},    {"Tira", "Tira"},
    {"Nightmare", "Nightmare"},   {"Mitsurugi", "Mitsurugi"}, {"Maxi", "Maxi"},
    {"Yoshimitsu", "Yoshimitsu"}, {nullptr, ""},             {nullptr, ""},
    {"Dampierre", "Dampierre"},   {"Voldo", "Voldo"},        {"Astaroth", "Astaroth"},
    {nullptr, "Creation"},        {"AlphaPatroklos", "Alpha Patroklos"}, {"Elysium", "Elysium"},
    {"EdgeMaster", "Edge Master"}, {"Algol", "Algol"},       {"Kilik", "Kilik"},
    {"PyrrhaOmega", "Pyrrha Omega"}, {nullptr, "Random"},
};

std::atomic<uint32_t> g_obj{0}, g_vtable{0};
std::atomic<uint64_t> g_since{0};
std::atomic<int> g_costume[2] = {-1, -1}, g_costumes[2] = {0, 0};

uint32_t Rd(uint32_t a) { return game::ReadU32(a); }

void SetUp(uint32_t obj, const char* which) {
  g_obj = obj;
  g_vtable = Rd(obj);
  g_since = frame::Count();
  g_costume[0] = g_costume[1] = -1;
  g_costumes[0] = g_costumes[1] = 0;
  REXLOG_INFO("sc5 select: {} select screen at {:08X} (frame {})", which, obj, frame::Count());
}

}  // namespace

uint32_t Object() {
  const uint32_t obj = g_obj.load(std::memory_order_relaxed);
  if (!obj) return 0;
  // gone once the battle loads, or its memory went to something else
  const uint32_t battle = game::BattleState();
  if ((battle != game::kBattleIdle && battle != 0) || Rd(obj) != g_vtable.load(std::memory_order_relaxed)) {
    g_obj = 0;
    return 0;
  }
  return obj;
}

uint64_t Age() { return Object() ? frame::Count() - g_since.load(std::memory_order_relaxed) : 0; }

Cursor CursorOf(int side) {
  Cursor c;
  if (const uint32_t obj = Object(); obj && (side == 0 || side == 1)) {
    c.x = int(Rd(obj + 0xAC + 12 * uint32_t(side)));
    c.y = int(Rd(obj + 0xB0 + 12 * uint32_t(side)));
  }
  return c;
}

int Step(int side) {
  const uint32_t obj = Object();
  return obj && (side == 0 || side == 1) ? int(Rd(obj + 0x9C + 4 * uint32_t(side))) : 0;
}

int Costume(int side) { return side == 0 || side == 1 ? g_costume[side].load() : -1; }
int Costumes(int side) { return side == 0 || side == 1 ? g_costumes[side].load() : 0; }

bool Pickable(int slot) {
  if (slot < 0 || slot >= kSlots || !kSlotNames[slot].roster) return false;
  const uint32_t obj = Object();
  return obj && Rd(obj + 1600 + 4 * uint32_t(slot)) == 0;
}

uint32_t PickableMask() {
  uint32_t m = 0;
  for (int s = 0; s < kSlots; s++)
    if (Pickable(s)) m |= 1u << s;
  return m;
}

int SlotOfRosterId(const std::string& id) {
  for (int s = 0; s < kSlots; s++)
    if (kSlotNames[s].roster && _stricmp(kSlotNames[s].roster, id.c_str()) == 0) return s;
  return -1;
}

int CharacterOfSlot(int slot) {
  if (slot < 0 || slot >= kSlots || !kSlotNames[slot].roster) return -1;
  return int(Rd(kSlotTable + kSlotSize * uint32_t(slot)));
}

int SlotOfCharacter(int character) {
  for (int s = 0; s < kSlots; s++)
    if (kSlotNames[s].roster && CharacterOfSlot(s) == character) return s;
  return -1;
}

const char* NameOfSlot(int slot) { return slot >= 0 && slot < kSlots ? kSlotNames[slot].name : "?"; }

}  // namespace sc5::select

// the select screens' setup functions (each builds the grid, sub_822F5E38)
REX_EXTERN(__imp__sub_822FBDF8);
REX_HOOK_RAW(sub_822FBDF8) {
  sc5::select::SetUp(ctx.r3.u32, "VS Battle's");
  __imp__sub_822FBDF8(ctx, base);
}
REX_EXTERN(__imp__sub_822F72C8);
REX_HOOK_RAW(sub_822F72C8) {
  sc5::select::SetUp(ctx.r3.u32, "a");
  __imp__sub_822F72C8(ctx, base);
}
REX_EXTERN(__imp__sub_82301500);
REX_HOOK_RAW(sub_82301500) {
  sc5::select::SetUp(ctx.r3.u32, "a");
  __imp__sub_82301500(ctx, base);
}

// the costume panel's label: (side, n, count), n from 1; (side, 0, 0) clears it
REX_EXTERN(__imp__sub_822FFF98);
REX_HOOK_RAW(sub_822FFF98) {
  const uint32_t side = ctx.r3.u32, n = ctx.r4.u32, count = ctx.r5.u32;
  if (side < 2 && n >= 1 && n <= count && count < 64) {
    sc5::select::g_costume[side] = int(n) - 1;
    sc5::select::g_costumes[side] = int(count);
  }
  __imp__sub_822FFF98(ctx, base);
}
