// Everything on the disc unlocked, see unlocks.h.

#include "sc5/unlocks.h"

#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_8246B318);  // the release state of an id: (release table, id) -> state | 8 if new

namespace sc5::unlocks {
namespace {

constexpr uint32_t kIds = 0x1D02;  // the table's entries (sub_8246B318 answers 0 from there on)
bool g_enabled = true;

}  // namespace

void Init() {
  const char* v = std::getenv("SC5_UNLOCK_ALL");
  g_enabled = !(v && v[0] == '0');
  REXLOG_INFO("sc5: {}", g_enabled ? "everything on the disc unlocked (SC5_UNLOCK_ALL=0 for the save's own unlocks)"
                                   : "the save's own unlocks only (SC5_UNLOCK_ALL=0)");
}

bool Enabled() { return g_enabled; }

// A locked entry (state 0, or 1) answers released (2), keeping its "new" mark; downloadable content (3) and hidden
// entries (4) go to the game's own answer. The table itself is not changed: the save keeps what was earned, and the
// game's own unlocks (sub_8246B3D8 reads the table directly) still record into it.
bool Released(const uint8_t* base, uint32_t table, uint32_t id, uint32_t& state_out) {
  if (!g_enabled || !table || id >= kIds) return false;
  const uint32_t at = table + (id >> 1);
  if (at >= 0xE0000000u) return false;  // the guest heap; physical memory maps elsewhere
  const uint8_t byte = base[at];
  const uint32_t nibble = (id & 1) ? uint32_t(byte >> 4) : uint32_t(byte & 0xF);
  const uint32_t state = nibble & 7;
  if (state != 0 && state != 1) return false;
  state_out = (nibble & 8) | 2;
  return true;
}

}  // namespace sc5::unlocks

REX_HOOK_RAW(sub_8246B318) {
  uint32_t state = 0;
  if (sc5::unlocks::Released(base, ctx.r3.u32, ctx.r4.u32 & 0xFFFF, state)) {
    ctx.r3.u64 = state;
    return;
  }
  __imp__sub_8246B318(ctx, base);
}
