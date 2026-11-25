// The character select screen, see select.h.
//
// The select object (VS Battle's: set up by sub_82308520, vtable 0x820CC658; the other modes' by sub_82303EA8 and
// sub_8230DC78) keeps, from what the game's own code reads and writes (sub_8230ACD0, a side's cursor and its pick;
// sub_8230C828, the pick; sub_82302A70, the grid):
//   +0x9C + 4 side   the side's step: 0 choosing a character, 1 its costume, 2 and 3 done
//   +0xAC + 12 side  the side's cursor: x (0-7), y (0-3), a repeat timer; sub_8230ACD0 moves it (with wrap-around, and
//                    past the two empty slots 19 and 20, which make one crest in the third row)
//   +1600 + 4 slot   0: the slot can be picked; 1 locked, 2 Dampierre's without his package (sub_82302A70)
// The costume panel's "n / count" label goes through sub_8230C710(side, n, count) every frame it is up.
// The slots, row by row, are the table at 0x82756020 (72 bytes each: +0 the game's character id, +0x24 the release
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

constexpr uint32_t kSlotTable = 0x82756020, kSlotSize = 72;

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
std::atomic<uint32_t> g_stage_obj{0};
std::atomic<uint64_t> g_stage_frame{0};  // the last frame its update ran
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
  if (slot == kCreationSlot) {  // Creation: its list of original characters (the screen refuses an empty one)
    const uint32_t obj = Object();
    return obj && Rd(obj + 1600 + 4 * uint32_t(slot)) == 0;
  }
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
  // tests (tools\linktest.py --p1 creation): the Creation slot, the first of its list unless a costume says otherwise
  if (_stricmp(id.c_str(), "creation") == 0) return kCreationSlot;
  for (int s = 0; s < kSlots; s++)
    if (kSlotNames[s].roster && _stricmp(kSlotNames[s].roster, id.c_str()) == 0) return s;
  return -1;
}

int CharacterOfSlot(int slot) {
  if (slot < 0 || slot >= kSlots || !kSlotNames[slot].roster) return -1;
  return int(Rd(kSlotTable + kSlotSize * uint32_t(slot)));
}

int IdOfSlot(int slot) { return slot >= 0 && slot < kSlots ? int(Rd(kSlotTable + kSlotSize * uint32_t(slot))) : -1; }

int SlotOfCharacter(int character) {
  for (int s = 0; s < kSlots; s++)
    if (kSlotNames[s].roster && CharacterOfSlot(s) == character) return s;
  return -1;
}

const char* NameOfSlot(int slot) { return slot >= 0 && slot < kSlots ? kSlotNames[slot].name : "?"; }

uint32_t StageObject() {
  const uint32_t obj = g_stage_obj.load(std::memory_order_relaxed);
  return obj && frame::Count() <= g_stage_frame.load(std::memory_order_relaxed) + 2 ? obj : 0;
}

int StageCursor() {
  const uint32_t obj = StageObject();
  if (!obj) return -1;
  const uint32_t v = Rd(obj + 200);
  return v < uint32_t(kStagePlaces) ? int(v) : -1;
}

}  // namespace sc5::select

// the select screens' setup functions (each builds the grid, sub_82302A70)
REX_EXTERN(__imp__sub_82308520);
REX_HOOK_RAW(sub_82308520) {
  sc5::select::SetUp(ctx.r3.u32, "VS Battle's");
  __imp__sub_82308520(ctx, base);
}
REX_EXTERN(__imp__sub_82303EA8);
REX_HOOK_RAW(sub_82303EA8) {
  sc5::select::SetUp(ctx.r3.u32, "a");
  __imp__sub_82303EA8(ctx, base);
}
REX_EXTERN(__imp__sub_8230DC78);
REX_HOOK_RAW(sub_8230DC78) {
  sc5::select::SetUp(ctx.r3.u32, "a");
  __imp__sub_8230DC78(ctx, base);
}

// the stage select's update, every frame it is up (r3 its object)
REX_EXTERN(__imp__sub_82359E08);
REX_HOOK_RAW(sub_82359E08) {
  sc5::select::g_stage_obj = ctx.r3.u32;
  sc5::select::g_stage_frame = sc5::frame::Count();
  __imp__sub_82359E08(ctx, base);
}

// the costume panel's label: (side, n, count), n from 1; (side, 0, 0) clears it
REX_EXTERN(__imp__sub_8230C710);
REX_HOOK_RAW(sub_8230C710) {
  const uint32_t side = ctx.r3.u32, n = ctx.r4.u32, count = ctx.r5.u32;
  if (side < 2 && n >= 1 && n <= count && count < 64) {
    sc5::select::g_costume[side] = int(n) - 1;
    sc5::select::g_costumes[side] = int(count);
  }
  __imp__sub_8230C710(ctx, base);
}
