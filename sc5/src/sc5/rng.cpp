// The game's random numbers, see rng.h.

#include "sc5/rng.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"

REX_EXTERN(sub_8266D2B0);          // the C runtime's _getptd: the calling thread's runtime data (rand's state at +0x14)
REX_EXTERN(__imp__sub_82195D48);   // the effects' update (particles)
REX_EXTERN(__imp__sub_82194210);   // the stage's gusts of wind (a new 576-byte force at random intervals)
REX_EXTERN(__imp__sub_8219D508);   // a task of the effects' (particle emitters), from the step's task calls
REX_EXTERN(__imp__sub_824A5ED0);   // a task that times itself and draws from Park-Miller and TT800 (ambient)

namespace sc5::rng {
namespace {

constexpr uint32_t kParkMiller = 0x829249F8, kParkMillerSize = 4;
constexpr uint32_t kTt800 = 0x82924A20, kTt800Size = 104;  // 25 words and the position
constexpr uint32_t kMtPointer = 0x82773FB0, kMtSize = 4996;  // 4992 bytes of words and the index

uint32_t g_main_rand = 0;

struct States {
  uint8_t pm[kParkMillerSize];
  uint8_t tt[kTt800Size];
  std::vector<uint8_t> mt;
  uint8_t crt[4];
  bool valid = false;
};
States g_visual;  // the visual systems' own, never put back
States g_fight;   // the fight's, kept aside while a visual system runs
int g_depth = 0;

rex::memory::Memory* Mem() { return REX_KERNEL_STATE()->memory(); }

uint32_t MtAddress() {
  const uint32_t a = __builtin_bswap32(*Mem()->TranslateVirtual<const uint32_t*>(kMtPointer));
  return a >= 0x40000000u && a < 0xA0000000u ? a : 0;
}

void Capture(States& s) {
  std::memcpy(s.pm, Mem()->TranslateVirtual<const uint8_t*>(kParkMiller), kParkMillerSize);
  std::memcpy(s.tt, Mem()->TranslateVirtual<const uint8_t*>(kTt800), kTt800Size);
  if (const uint32_t mt = MtAddress()) {
    const uint8_t* p = Mem()->TranslateVirtual<const uint8_t*>(mt);
    s.mt.assign(p, p + kMtSize);
  } else {
    s.mt.clear();
  }
  if (g_main_rand) std::memcpy(s.crt, Mem()->TranslateVirtual<const uint8_t*>(g_main_rand), 4);
  s.valid = true;
}

void Apply(const States& s) {
  std::memcpy(Mem()->TranslateVirtual<uint8_t*>(kParkMiller), s.pm, kParkMillerSize);
  std::memcpy(Mem()->TranslateVirtual<uint8_t*>(kTt800), s.tt, kTt800Size);
  if (const uint32_t mt = MtAddress(); mt && s.mt.size() == kMtSize)
    std::memcpy(Mem()->TranslateVirtual<uint8_t*>(mt), s.mt.data(), kMtSize);
  if (g_main_rand) std::memcpy(Mem()->TranslateVirtual<uint8_t*>(g_main_rand), s.crt, 4);
}

bool Separate() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_RNG_SHARED");  // 1: the visual systems draw from the fight's states (tests)
    return !(v && v[0] == '1');
  }();
  return on;
}

}  // namespace

uint32_t MainRandState(PPCContext& ctx, uint8_t* base) {
  if (!g_main_rand && frame::OnMainThread()) {
    const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
    sub_8266D2B0(ctx, base);
    g_main_rand = ctx.r3.u32 + 0x14;
    ctx.r3.u64 = r3;
    ctx.lr = lr;
  }
  return g_main_rand;
}

void EnterVisual(PPCContext& ctx, uint8_t* base) {
  if (!Separate() || !frame::OnMainThread()) return;
  if (g_depth++ > 0) return;
  MainRandState(ctx, base);
  Capture(g_fight);
  if (!g_visual.valid) g_visual = g_fight;  // they part from here on
  Apply(g_visual);
}

void LeaveVisual() {
  if (!Separate() || !frame::OnMainThread() || g_depth == 0) return;
  if (--g_depth > 0) return;
  Capture(g_visual);
  Apply(g_fight);
}

bool InVisual() { return g_depth > 0; }

namespace {
bool DrawsOn() {
  static const bool on = std::getenv("SC5_DBG_DRAWS") != nullptr;
  return on;
}
std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_draws;  // match frame -> (caller, state before)
uint32_t g_draws_frame = 0xFFFFFFFFu;
FILE* g_draws_file = nullptr;
}  // namespace

void DrawsFrame(uint32_t match_frame) {
  if (!DrawsOn()) return;
  g_draws_frame = match_frame;
  g_draws[match_frame].clear();
}

void DrawsSettled(uint32_t below) {
  if (!DrawsOn()) return;
  if (!g_draws_file) g_draws_file = std::fopen(std::getenv("SC5_DBG_DRAWS"), "w");
  while (!g_draws.empty() && g_draws.begin()->first < below) {
    if (g_draws_file) {
      std::fprintf(g_draws_file, "%u:", g_draws.begin()->first);
      for (const auto& [caller, state] : g_draws.begin()->second) std::fprintf(g_draws_file, " %08X:%08X", caller, state);
      std::fputc('\n', g_draws_file);
    }
    g_draws.erase(g_draws.begin());
  }
  if (g_draws_file) std::fflush(g_draws_file);
}

void NoteDraw(uint32_t caller) {
  if (!DrawsOn() || g_depth > 0 || g_draws_frame == 0xFFFFFFFFu || !frame::OnMainThread()) return;
  uint32_t state;
  std::memcpy(&state, Mem()->TranslateVirtual<const uint8_t*>(kParkMiller), 4);
  g_draws[g_draws_frame].emplace_back(caller, __builtin_bswap32(state));
}

void ResetVisual() { g_visual.valid = false; }

}  // namespace sc5::rng

// tests: the fight's Park-Miller draws by caller (SC5_DBG_DRAWS)
REX_EXTERN(__imp__sub_824A33D8);
REX_EXTERN(__imp__sub_824A3248);
REX_HOOK_RAW(sub_824A33D8) {
  sc5::rng::NoteDraw(uint32_t(ctx.lr));
  __imp__sub_824A33D8(ctx, base);
}
REX_HOOK_RAW(sub_824A3248) {
  sc5::rng::NoteDraw(uint32_t(ctx.lr));
  __imp__sub_824A3248(ctx, base);
}

REX_HOOK_RAW(sub_82195D48) {
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_82195D48(ctx, base);
  sc5::rng::LeaveVisual();
}

// SC5_NO_GUSTS=1 (tests): no gusts at all
REX_HOOK_RAW(sub_82194210) {
  static const bool off = [] { const char* v = std::getenv("SC5_NO_GUSTS"); return v && v[0] == '1'; }();
  if (off) return;
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_82194210(ctx, base);
  sc5::rng::LeaveVisual();
}

// Two tasks the step calls (sub_82493230, 0x8249346C) that keep timers in their heap objects and so draw a different
// number of times when a frame runs again (found by counting each call site's draws in frames and in their reruns)
REX_HOOK_RAW(sub_8219D508) {
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_8219D508(ctx, base);
  sc5::rng::LeaveVisual();
}
REX_HOOK_RAW(sub_824A5ED0) {
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_824A5ED0(ctx, base);
  sc5::rng::LeaveVisual();
}
