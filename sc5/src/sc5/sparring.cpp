// Online Practice's rules, see sparring.h.
//
// The fighters' health: a float at +0x2268C of each player's object (0x82937070, 0x82994BA0), full at +0x22688 (240.0
// in VS Battle). The round's clock (a block of the fight at 0x8293619C): +0xC the seconds left, +0x10 the frames left
// in the second (60 down to 1; at 0 a second goes), +0 the round's time as set. Found by comparing a fight's snapshots.

#include "sc5/sparring.h"

#include <atomic>
#include <cstring>
#include <map>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/game.h"

namespace sc5::sparring {
namespace {

std::atomic<bool> g_on{false};

constexpr uint32_t kPlayer[2] = {0x82937070, 0x82994BA0};
constexpr uint32_t kHealth = 0x2268C, kFullHealth = 0x22688;
constexpr uint32_t kClock = 0x8293619C, kClockSet = 0x0, kClockSeconds = 0xC, kClockFrames = 0x10;

// below this part of full health at a frame's top a fighter is full again at once: no single hit takes more
constexpr float kFloor = 0.4f;
// frames a fighter's health stays the same before it is full again (a combo's hits come sooner than this)
constexpr uint32_t kRefillAfter = 90;

// per side: the health at the frame's top after the rules, and the frames it has stayed so
struct Memo {
  uint32_t last[2] = {0, 0};
  uint32_t still[2] = {0, 0};
};
std::map<uint32_t, Memo> g_memo;  // by match frame: what that frame's top left
int g_logged = 0;

uint32_t Get(uint32_t a) {
  return __builtin_bswap32(*REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint32_t*>(a));
}
void Put(uint32_t a, uint32_t v) {
  *REX_KERNEL_STATE()->memory()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v);
}
float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

void Set(bool on) {
  if (g_on.exchange(on) != on) REXLOG_INFO("sc5 sparring: Online Practice {}", on ? "on" : "off");
  g_memo.clear();
}

bool On() { return g_on.load(); }

void OnFrame(uint32_t frame) {
  if (!g_on.load()) return;
  // what the frame before left (a frame run again starts from the same)
  Memo memo;
  if (frame) {
    auto it = g_memo.find(frame - 1);
    if (it != g_memo.end()) memo = it->second;
  }
  g_memo.erase(g_memo.lower_bound(frame), g_memo.end());
  if (game::BattleState() == game::kBattleFighting) {
    // the clock stays where the round started
    const uint32_t set = Get(kClock + kClockSet);
    if (set && set < 1000) {
      Put(kClock + kClockSeconds, set);
      Put(kClock + kClockFrames, 60);
    }
    for (int s = 0; s < 2; s++) {
      const uint32_t full_bits = Get(kPlayer[s] + kFullHealth), bits = Get(kPlayer[s] + kHealth);
      const float full = AsFloat(full_bits), h = AsFloat(bits);
      if (!(full > 0.0f) || !(h > 0.0f) || h >= full) {  // full, or down for good
        memo.last[s] = bits;
        memo.still[s] = 0;
        continue;
      }
      const uint32_t frames = bits == memo.last[s] ? memo.still[s] + 1 : 0;
      if (h < kFloor * full || frames >= kRefillAfter) {
        Put(kPlayer[s] + kHealth, full_bits);
        memo.last[s] = full_bits;
        memo.still[s] = 0;
        if (g_logged < 40 && ++g_logged)
          REXLOG_INFO("sc5 sparring: P{}'s health {:.1f} back to {:.0f} at frame {} ({})", s + 1, h, full, frame,
                      h < kFloor * full ? "below the floor" : "no damage for a while");
      } else {
        memo.last[s] = bits;
        memo.still[s] = frames;
      }
    }
  }
  g_memo[frame] = memo;
  while (g_memo.size() > 64) g_memo.erase(g_memo.begin());
}

}  // namespace sc5::sparring
