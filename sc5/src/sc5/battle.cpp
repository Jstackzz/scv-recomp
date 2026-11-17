// The game's own battle state snapshots, see battle.h.

#include "sc5/battle.h"
#include "sc5/rng.h"
#include "sc5/frame.h"
#include "sc5/game.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(sub_82159DF0);  // saves the battle's state into a stream (r3)
REX_EXTERN(sub_8215A360);  // restores it from a stream (r3)
REX_EXTERN(__imp__sub_8213DEC0);  // a task's update: the players' held and newly pressed buttons (0x82927E88)

namespace sc5::battle {
namespace {

constexpr uint32_t kStreamVtable = 0x820BA8B8;
constexpr uint32_t kCapacity = 0x28000;
constexpr uint32_t kMainLoopReturn = 0x82189268;  // what a call from main's loop leaves in lr

rex::memory::Memory* Mem() {
  auto* ks = REX_KERNEL_STATE();
  return ks ? ks->memory() : nullptr;
}

void Put32(uint32_t address, uint32_t value) {
  *Mem()->TranslateVirtual<uint32_t*>(address) = __builtin_bswap32(value);
}

uint32_t Get32(uint32_t address) { return __builtin_bswap32(*Mem()->TranslateVirtual<const uint32_t*>(address)); }

// What the game's snapshot does not carry exactly, kept raw beside it (SC5_BATTLE_RAW=1, being tried): the players'
// objects (static), the battle's state machine, the pad manager (0x82771080: pads, presses, its frame counters; and the
// 0x100 bytes before it, being looked at), the game's 360 pads (0x828B3A40: four objects of 400 bytes that read
// XInput, with the last packet number each saw), the random numbers sub_824A33D8 and sub_824A3248 draw (a Park-Miller
// generator at 0x829249F8) and sub_824A3030 (TT800: 25 words at 0x82924A20, its position at +100), the fighters'
// AI (sub_822DC390 runs it for both players, human or not, every frame: its state 0x82A15980-0x82A19140, the virtual
// pads at 0x82A18518, 152 bytes a player; being tried), the players' input delay rings (0x828FD130: per player a write count, then 61 slots of 8 bytes, then a flag; sub_8216E2F8
// puts each frame's input `delay` slots ahead, 0x8275C15C per player, 2 offline), the frame counters that start at
// boot, the main thread's rand state.
struct Region {
  uint32_t address, size;
};
const Region kRaw[] = {
    {0x82937070, 0x5DB30}, {0x82994BA0, 0x5DB30}, {0x82767D08, 0x200}, {0x82770F80, 0x100},
    {0x82771080, 0xB40},   {0x828B3A40, 0x6A8},   {0x828FD130, 0x3E0}, {0x829249F0, 0x98},
    {0x8280DE94, 4},       {0x8281929C, 4},       {0x8281991C, 4},     {0x828E8790, 4},
    {0x835B3EB4, 4},       {0x82A15980, 0x37C0},
};
// Found as the game runs, kept with their addresses (DynamicRegions): the main thread's rand state; the Mersenne
// Twister's state (sub_824A2FD0 seeds it: [0x82773FB0] -> 4992 bytes of words, then its index); the task sub_8213DEC0
// updates (a heap object, 0x401C0200 in every run so far: at +128 each player's buttons held in the last frame, which
// its "newly pressed" bits come from); the forces sub_821936C8 sums for each player every frame (a manager in the
// heap, 0x401CC300: its list at +128, each force's next at +128, its vector from vtable slot +32; the sum, clamped,
// goes to each player's +0x1CE30, which the pose's cloth and secondary bones take in: a constant one, one by the
// stage's floor under the player, and two point forces with a transform at +448 their owners move).
uint32_t g_rand_state = 0;  // the main thread's _getptd() + 0x14, once known
std::atomic<uint32_t> g_input_task{0};
std::atomic<uint32_t> g_force_manager{0};
constexpr uint32_t kMtPointer = 0x82773FB0, kMtSize = 4996;
// The forces are NOT kept: the manager (336 bytes, sub_82193608) queues calls for the next frame, its forces (400
// bytes, sub_82193AD8: an animated wind 0x820BCDE8, one by the floor 0x820BCE0C) and the gusts (576 bytes,
// sub_82194210, at random intervals) come and go with attacks and time; put back, they froze or crashed the game.

std::vector<Region> DynamicRegions() {
  std::vector<Region> out;
  auto valid = [](uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; };
  if (g_rand_state) out.push_back({g_rand_state, 4});
  if (const uint32_t mt = Get32(kMtPointer); valid(mt)) out.push_back({mt, kMtSize});
  if (const uint32_t task = g_input_task.load(std::memory_order_relaxed); valid(task)) out.push_back({task + 128, 8});
  return out;
}

struct Kept {
  std::vector<uint8_t> fixed;  // kRaw, in order
  std::vector<Region> dynamic;
  std::vector<uint8_t> dynamic_bytes;
};
std::unordered_map<uint32_t, Kept> g_raw;  // stream -> its raw memory
std::mutex g_raw_lock;

// the raw memory beside the snapshot: on unless SC5_BATTLE_RAW=0 (tests)
bool RawEnabled() {
  static const bool on = [] { const char* v = std::getenv("SC5_BATTLE_RAW"); return !(v && v[0] == '0'); }();
  return on;
}

// the blocks of each stream's last save (OnSnapWrite)
std::unordered_map<uint32_t, std::vector<Block>> g_blocks;
uint32_t g_recording = 0;
std::vector<Block> g_recorded;

void KeepRaw(PPCContext& ctx, uint8_t* base, uint32_t stream) {
  if (!g_rand_state) g_rand_state = sc5::rng::MainRandState(ctx, base);
  std::lock_guard<std::mutex> lock(g_raw_lock);
  Kept& k = g_raw[stream];
  k.fixed.clear();
  for (const Region& r : kRaw) {
    const uint8_t* p = Mem()->TranslateVirtual<const uint8_t*>(r.address);
    k.fixed.insert(k.fixed.end(), p, p + r.size);
  }
  k.dynamic = DynamicRegions();
  k.dynamic_bytes.clear();
  for (const Region& r : k.dynamic) {
    const uint8_t* p = Mem()->TranslateVirtual<const uint8_t*>(r.address);
    k.dynamic_bytes.insert(k.dynamic_bytes.end(), p, p + r.size);
  }
}

void PutRawBack(uint32_t stream) {
  std::lock_guard<std::mutex> lock(g_raw_lock);
  auto it = g_raw.find(stream);
  if (it == g_raw.end()) return;
  const Kept& k = it->second;
  const uint8_t* p = k.fixed.data();
  for (const Region& r : kRaw) {
    std::memcpy(Mem()->TranslateVirtual<uint8_t*>(r.address), p, r.size);
    p += r.size;
  }
  p = k.dynamic_bytes.data();
  for (const Region& r : k.dynamic) {
    std::memcpy(Mem()->TranslateVirtual<uint8_t*>(r.address), p, r.size);
    p += r.size;
  }
}

}  // namespace

uint32_t NewStream() {
  auto* mem = Mem();
  if (!mem) return 0;
  const uint32_t s = mem->SystemHeapAlloc(12 + kCapacity, 0x80);
  if (!s) {
    REXLOG_ERROR("sc5: no guest memory for a battle snapshot");
    return 0;
  }
  Put32(s, kStreamVtable);
  Put32(s + 4, 0);
  Put32(s + 8, 0);
  return s;
}

uint32_t Save(PPCContext& ctx, uint8_t* base, uint32_t stream) {
  if (!stream) return 0;
  Put32(stream + 4, 0);
  Put32(stream + 8, 0);
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = stream;
  ctx.lr = kMainLoopReturn;
  g_recording = stream;
  g_recorded.clear();
  sub_82159DF0(ctx, base);
  g_recording = 0;
  g_blocks[stream].swap(g_recorded);
  ctx.lr = lr;
  if (RawEnabled()) KeepRaw(ctx, base, stream);
  return Get32(stream + 4);
}

bool Restore(PPCContext& ctx, uint8_t* base, uint32_t stream) {
  if (!stream) return false;
  const uint32_t size = Get32(stream + 4);
  Put32(stream + 8, 0);
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = stream;
  ctx.lr = kMainLoopReturn;
  sub_8215A360(ctx, base);
  ctx.lr = lr;
  const uint32_t read = Get32(stream + 8);
  if (read != size) {
    REXLOG_WARN("sc5: battle restore read {} of {} bytes", read, size);
    return false;
  }
  if (RawEnabled()) PutRawBack(stream);
  return true;
}

std::string RawDifferences(uint32_t a, uint32_t b, uint32_t& count) {
  count = 0;
  std::lock_guard<std::mutex> lock(g_raw_lock);
  auto ia = g_raw.find(a), ib = g_raw.find(b);
  if (ia == g_raw.end() || ib == g_raw.end()) return "";
  std::string out;
  int ranges = 0;
  auto compare = [&](const uint8_t* x, const uint8_t* y, size_t n, auto&& where) {
    for (size_t i = 0; i < n;) {
      if (x[i] == y[i]) {
        i++;
        continue;
      }
      size_t j = i;
      while (j < n && x[j] != y[j]) j++;
      count += uint32_t(j - i);
      if (ranges++ < 10) out += fmt::format(" {:08X}+{}", where(i), j - i);
      i = j;
    }
  };
  const Kept& x = ia->second;
  const Kept& y = ib->second;
  if (x.fixed.size() == y.fixed.size())
    compare(x.fixed.data(), y.fixed.data(), x.fixed.size(), [&](size_t i) -> uint32_t {
      size_t o = 0;
      for (const Region& r : kRaw) {
        if (i < o + r.size) return r.address + uint32_t(i - o);
        o += r.size;
      }
      return 0;
    });
  size_t ox = 0;
  for (const Region& r : x.dynamic) {
    size_t oy = 0;
    for (const Region& s2 : y.dynamic) {
      if (s2.address == r.address && s2.size == r.size) {
        compare(x.dynamic_bytes.data() + ox, y.dynamic_bytes.data() + oy, r.size,
                [&](size_t i) -> uint32_t { return r.address + uint32_t(i); });
        break;
      }
      oy += s2.size;
    }
    ox += r.size;
  }
  if (ranges > 10) out += fmt::format(" (+{} more)", ranges - 10);
  return out;
}

uint32_t Size(uint32_t stream) { return stream ? Get32(stream + 4) : 0; }

void OnSnapWrite(uint32_t stream, uint32_t pos, uint32_t size, uint32_t source) {
  if (stream && stream == g_recording) g_recorded.push_back({pos, size, source});
}

const std::vector<Block>* Blocks(uint32_t stream) {
  auto it = g_blocks.find(stream);
  return it == g_blocks.end() ? nullptr : &it->second;
}

// each player's raw pointers in the snapshot (sub_82141778 sets the first five; the rest point into loaded data too)
constexpr uint32_t kPlayerPointers[] = {0x22638, 0x5CB44, 0x5CBF4, 0x5CCA4, 0x5CD54, 0x5CE04, 0x5CEB4,
                                        0x5D2B4, 0x5D30C, 0x5D364, 0x5D3BC, 0x5DACC, 0x5DAD0};

Kind KindOf(uint32_t source) {
  for (uint32_t player : {0x82937070u, 0x82994BA0u}) {
    for (uint32_t k = 0; k < 3; k++) {
      const uint32_t lo = player + 0x3190 + k * 0x8000;
      if (source >= lo && source < lo + 6208) return Kind::Pose;
      // the three buffers the pose fills for the drawing, taken in turn (0x800 apart): a frame run again leaves the
      // pose out (frame.cpp), so they keep what the frame gone back to had; nothing in the fight reads them
      const uint32_t drawn = player + 0x1B310 + k * 0x800;
      if (source >= drawn && source < drawn + 0x800) return Kind::Pose;
    }
    if (source >= player + 0x22638 && source < player + 0x5DAD4)
      for (uint32_t o : kPlayerPointers)
        if (source >= player + o && source < player + o + 4) return Kind::Pointer;
  }
  // a static object past player 2's (the snapshot's 3152-byte block from 0x829F4E90) keeps one heap pointer
  if (source >= 0x829F552Cu && source < 0x829F5530u) return Kind::Pointer;
  // ... and at +0xC44 a value sub_82210320 takes from the stage as the fight loads (sub_822134F8), -40.0 when the
  // stage's data is not in yet: how far the loading had got, different on two PCs, and nothing in the fight reads it
  // differently (two games with -40.0 and -5.5 there stayed alike in all else)
  if (source >= 0x829F59D4u && source < 0x829F59D8u) return Kind::Pointer;
  if (source >= 0x70000000u && source < 0x80000000u) return Kind::Stack;
  if (source >= 0x40000000u && source < 0x70000000u) return Kind::Heap;
  if (source >= 0xA0000000u) return Kind::Heap;  // physical memory: a battle object there stamps a frame counter
  return Kind::Fight;
}

uint64_t FightHash(uint32_t stream) {
  const std::vector<Block>* blocks = Blocks(stream);
  const uint8_t* data = Data(stream);
  const uint32_t size = Size(stream);
  uint64_t h = 1469598103934665603ull;
  if (!blocks || !data) return 0;
  for (const Block& b : *blocks) {
    if (b.pos + b.size > size) continue;
    // byte by byte: a pose buffer's block can start in the middle of a player's object
    for (uint32_t i = 0; i < b.size; i++) {
      if (KindOf(b.source + i) != Kind::Fight) continue;
      h = (h ^ data[b.pos + i]) * 1099511628211ull;
    }
  }
  return h;
}

const uint8_t* Data(uint32_t stream) { return stream ? Mem()->TranslateVirtual<const uint8_t*>(stream + 12) : nullptr; }

}  // namespace sc5::battle

REX_EXTERN(__imp__sub_821936C8);  // each player's forces (cloth, secondary bones): see DynamicRegions
REX_HOOK_RAW(sub_821936C8) {
  sc5::battle::g_force_manager.store(ctx.r3.u32, std::memory_order_relaxed);
  // TEMPORARY: SC5_DBG_FORCES_FROM/TO (fight frames): each force's contents when they are summed
  static const int64_t from = std::getenv("SC5_DBG_FORCES_FROM") ? std::atoll(std::getenv("SC5_DBG_FORCES_FROM")) : -1;
  static const int64_t to = std::getenv("SC5_DBG_FORCES_TO") ? std::atoll(std::getenv("SC5_DBG_FORCES_TO")) : -1;
  const int64_t ff = sc5::game::FightFrame();
  if (from >= 0 && ff >= from && ff < to) {
    auto* mem = REX_KERNEL_STATE()->memory();
    auto get = [&](uint32_t a) { return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(a)); };
    auto hash = [&](uint32_t a, uint32_t n) {
      const uint8_t* p = mem->TranslateVirtual<const uint8_t*>(a);
      uint64_t h = 1469598103934665603ull;
      for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
      return h;
    };
    std::string list = fmt::format(" mgr {:016X}", hash(ctx.r3.u32, 0x100));
    for (uint32_t f = get(ctx.r3.u32 + 128), i = 0; f && i < 16; f = get(f + 128), i++) {
      const uint32_t vt = get(f);
      list += fmt::format(" [{:08X} {:08X} {:016X}]", f, vt, hash(f, vt == 0x820BCE78 ? 576 : 400));
    }
    REXLOG_INFO("sc5 dbg: forces {}{}:{}", ff, sc5::frame::Replaying() ? " again" : "", list);
    if (const char* dir = std::getenv("SC5_TEST_DUMP_DIR"); dir && *dir) {
      std::vector<uint32_t> objs{ctx.r3.u32};
      for (uint32_t f = get(ctx.r3.u32 + 128), i = 0; f && i < 16; f = get(f + 128), i++) objs.push_back(f);
      for (uint32_t o : objs) {
        const std::string path = fmt::format("{}/force_{}{}_{:08X}.bin", dir, ff, sc5::frame::Replaying() ? "_again" : "", o);
        if (FILE* fp = std::fopen(path.c_str(), "wb")) {
          std::fwrite(mem->TranslateVirtual<const uint8_t*>(o), 1, 576, fp);
          std::fclose(fp);
        }
      }
    }
  }
  // a visual system: its random numbers (the forces it creates, from its queue) from the visual states (rng.h)
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_821936C8(ctx, base);
  sc5::rng::LeaveVisual();
}

REX_HOOK_RAW(sub_8213DEC0) {
  sc5::battle::g_input_task.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_8213DEC0(ctx, base);
}
