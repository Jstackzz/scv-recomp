// The game's own battle state snapshots, see battle.h.

#include "sc5/battle.h"
#include "sc5/rng.h"
#include "sc5/frame.h"
#include "sc5/game.h"

#include <algorithm>
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
// AI (sub_822DC390 runs it for both players, human or not, every frame: its state 0x82A15980-0x82A190E0, the virtual
// pads at 0x82A18518, 152 bytes a player; being tried), the players' input delay rings (0x828FD130: per player a write count, then 61 slots of 8 bytes, then a flag; sub_8216E2F8
// puts each frame's input `delay` slots ahead, 0x8275C15C per player, 2 offline), the frame counters that start at
// boot, the main thread's rand state.
struct Region {
  uint32_t address, size;
};
constexpr uint32_t kPlayerObjects[2] = {0x82937070, 0x82994BA0};
constexpr uint32_t kThrowObject = 0x5BE90;  // in each player's object: a throw's heap object (see Restore)
const Region kRaw[] = {
    {0x82937070, 0x5DB30}, {0x82994BA0, 0x5DB30}, {0x82767D08, 0x200}, {0x82770F80, 0x100},
    {0x82771080, 0xB40},   {0x828B3A40, 0x6A8},   {0x828FD130, 0x3E0}, {0x829249F0, 0x98},
    {0x828E8790, 4},       {0x835B3EB4, 4},       {0x82A15980, 0x3760},
};
// (Not the three counters of the game's frames on the GPU, though they too start at boot: 0x8281929C, the low word of
// the frame sync's count of frames presented (0x82819200 + 152, sub_82542D50), 0x8280DE94 (sub_82550DF8) and
// 0x8281991C (sub_82543CB0). Only the present (sub_824945F8, never in a frame run again) moves them, so each rollback
// set them back by its frames, and the game's waits for its GPU waited at last for a frame it had already presented:
// the main thread blocked in sub_82542D50 for good, at a KO's replay or a match's loading, after a few hundred frames
// run again (2026-11-18). Nothing in the fight reads them.)
// (The AI's region ends where the game's main allocator begins: its control block at 0x82A190E0, a TLSF pool of 3 MB
// that sub_8237B250 / sub_8237B268 / sub_8237B278 allocate, free and resize in, its start, end and size, then its free
// lists by size. The region once ran to 0x82A19140 and put back the first 18 lists of small blocks as they were when
// the frame was kept: a block taken since was handed out again, and the game crashed on a later allocation inside a
// restore, writing through a "next free" read from a block in use: Aeon Calcos against Siegfried with 40 ms each way,
// "write of guest 0x3ED82A23" from sub_8237AB38, 2026-11-17.)
// Found as the game runs, kept with their addresses (DynamicRegions): the main thread's rand state; the Mersenne
// Twister's state (sub_824A2FD0 seeds it: [0x82773FB0] -> 4992 bytes of words, then its index); the task sub_8213DEC0
// updates (a heap object, 0x401C0200 in every run so far: at +128 each player's buttons held in the last frame, which
// its "newly pressed" bits come from); the forces sub_821936C8 sums for each player every frame (a manager in the
// heap, 0x401CC300: its list at +128, each force's next at +128, its vector from vtable slot +32; the sum, clamped,
// goes to each player's +0x1CE30, which the pose's cloth and secondary bones take in: a constant one, one by the
// stage's floor under the player, and two point forces with a transform at +448 their owners move).
//   ... and the KO replay's manager (sub_8211A8A8 runs it once a frame: r3 is [battle task + 128] + 4, a heap object,
// 0x41780004 so far). While a round is fought it saves the battle into the next of its five streams (0x2800C bytes
// each from +0x2CC, a sixth holds the present during a replay) whenever its own frame counter (+0xF0318) is 84 past the
// last save's stamp (five stamps at +0x2B4, the next stream at +0x2B0); after a KO it plays one back. Nothing put its
// fields back, so every frame a rollback ran again counted twice and the saves came earlier: two games with a different
// number of frames run again saved at different fight frames (2213 and 2218), and replayed a KO from different points
// (render scale 1 against 2, the check during the replay parted them; in a launcher test one game replayed a KO for 3
// frames and the other not, 2026-11-17). Its fields are kept: the 0x2CC bytes before the streams, each stream's
// 12-byte header (+4 written, +8 read) and the 0x7C after them; a frame run again where the first run saved saves
// again, the same stream at the same point (the streams' contents need no keeping).
uint32_t g_rand_state = 0;  // the main thread's _getptd() + 0x14, once known
std::atomic<uint32_t> g_input_task{0};
std::atomic<uint32_t> g_force_manager{0};
std::atomic<uint32_t> g_replay_manager{0};
constexpr uint32_t kMtPointer = 0x82773FB0, kMtSize = 4996;
constexpr uint32_t kReplayHead = 0x2CC, kReplayStream = 0x2800C, kReplayStreams = 6, kReplayTail = 0x7C;
//   ... and the battle's cameras. The side of the screen each player is on, which turns the stick's left and right
// into back and forward, comes from them: each player's +0xC4 (sub_8216E2F8), 1 for the player sub_8216C378 names,
// the sign of a cross product of the two fighters' positions and the camera's (sub_8216B338: the view at +16 of the
// cameras' director, its x and z through vtable slots +80 and +88), or player 1 while the camera with the greatest
// blend weight (sub_824A4660: eight cameras at +608, the weight at +156) is in its modes 1 and 3. The camera follows
// the fighters smoothly, and nothing put it back: a frame run again moved it on once more, so after their rollbacks
// two games' cameras stood a little apart, and as the fighters crossed over, the games turned them on different frames
// and read the same stick as other directions (Ezio against Viola with 40 ms each way: P1's inputs at +0x240C apart
// from frame 3480). The director is the battle scene's task from +224 ([0x82927BB8], 0x540 bytes to the task's end:
// the cameras' positions, targets and blends; pointers only to itself, to the two arrays below and to what the fight
// loaded), and its cameras are two arrays in physical memory, 8 of 0x320 bytes ([+0x2B4]) and 9 of 0x280 ([+0x384]),
// whose pointers are to static objects (a camera's target, one of the players' objects): kept with each frame.
constexpr uint32_t kCameraDirector = 0x82927BB8, kCameraSystem = 0x540;
//   ... and the battle's four save slots, which moves' scripts use (sub_82129B10's commands: 9002 saves the battle into
// a slot, sub_82479360; 9003 puts a slot back, sub_82479488, keeping a few of the present's fields: each player's
// +468, +1588, +4188 and one of +1788's; 9004 empties them all): a move saves the battle and a few frames on goes back
// to it. Each slot is a stream like the KO replay's (a 12-byte header, then the game's own snapshot), at 0x49CA0 +
// slot * 0x2800C in the buffer of the battle's setup ([0x82767D1C] + 140, that object current while its +48 is the
// state machine's +16); per slot a count at 0x8273C21C (-1 empty, 0 saved, +1 each time it is put back; 2 written
// instead of a save while 0x82927FA8 is set), a word at 0x82927BDC (0 saved, 1 put back) and the KO replay manager's
// copy of the counts at 0x828FD084.
// Nothing put them back: a frame run again after its first run had put a slot back found the slot's count and word
// as that run had left them, and the move's script went on without going back. With the same inputs, one game went
// back 7 frames (the slot saved at 6015, put back at 6022) and the other, which had run 6022 on a guess first, did
// not (Xiba against Leixia with 40 ms each way: P1's counters 7 apart from frame 6023, the check at 6030 apart,
// 2026-11-18). The counts and words are kept with each frame, and so is each slot that holds a save (a frame run
// again may not save where its first run did: the slot holds what the kept frame had).
constexpr uint32_t kSlotCounts = 0x8273C21C, kSlotsPutBack = 0x82927BDC, kSlotCountsCopy = 0x828FD084;
constexpr uint32_t kSlotSetup = 0x82767D1C, kSlotOffset = 0x49CA0, kSlotStride = 0x2800C, kSlots = 4;
// The forces are NOT kept: the manager (336 bytes, sub_82193608) queues calls for the next frame, its forces (400
// bytes, sub_82193AD8: an animated wind 0x820BCDE8, one by the floor 0x820BCE0C) and the gusts (576 bytes,
// sub_82194210, at random intervals) come and go with attacks and time; put back, they froze or crashed the game.

std::vector<Region> DynamicRegions() {
  std::vector<Region> out;
  auto valid = [](uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; };
  if (g_rand_state) out.push_back({g_rand_state, 4});
  if (const uint32_t mt = Get32(kMtPointer); valid(mt)) out.push_back({mt, kMtSize});
  if (const uint32_t task = g_input_task.load(std::memory_order_relaxed); valid(task)) out.push_back({task + 128, 8});
  if (const uint32_t m = g_replay_manager.load(std::memory_order_relaxed); valid(m)) {
    out.push_back({m, kReplayHead});
    for (uint32_t i = 0; i < kReplayStreams; i++) out.push_back({m + kReplayHead + i * kReplayStream, 12});
    out.push_back({m + kReplayHead + kReplayStreams * kReplayStream, kReplayTail});
  }
  // the battle's save slots (see kSlotCounts): their counts and words, and each slot that holds a save
  out.push_back({kSlotCounts, kSlots * 4});
  out.push_back({kSlotsPutBack, kSlots * 4});
  out.push_back({kSlotCountsCopy, kSlots * 4});
  if (const uint32_t setup = Get32(kSlotSetup); valid(setup) && Get32(setup + 48) == Get32(kSlotSetup - 4)) {
    if (const uint32_t buffer = Get32(setup + 140); valid(buffer))
      for (uint32_t i = 0; i < kSlots; i++) {
        if (int32_t(Get32(kSlotCounts + 4 * i)) < 0) continue;
        const uint32_t slot = buffer + kSlotOffset + i * kSlotStride;
        out.push_back({slot, 12 + std::min(Get32(slot + 4), kSlotStride - 12)});
      }
  }
  // the battle's cameras (see kCameraDirector)
  if (const uint32_t d = Get32(kCameraDirector); valid(d)) {
    out.push_back({d, kCameraSystem});
    const auto physical = [](uint32_t a) { return a >= 0xA0000000u && a < 0xE0000000u; };
    if (const uint32_t cams = Get32(d + 0x2B4); physical(cams)) out.push_back({cams, 8 * 0x320});
    if (const uint32_t more = Get32(d + 0x384); physical(more)) out.push_back({more, 9 * 0x280});
  }
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
  // Each player's +0x5BE90 is the address of a throw's heap object, which the game's own restore sets to the object it
  // makes again (a battle object's restore, sub_82471830, writes its owner's). The raw copy put back after it holds the
  // address the object had when the frame was kept, freed since: a call through it crashed the game a few frames after
  // a rollback over a throw (Aeon Calcos's back throw, "invalid call ... 0x00000032"). So where the game's restore set
  // the field, its value stays; where it left it (no throw in the frame gone back to), the kept one is put back.
  uint32_t before[2], after[2];
  for (int i = 0; i < 2; i++) before[i] = Get32(kPlayerObjects[i] + kThrowObject);
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
  for (int i = 0; i < 2; i++) after[i] = Get32(kPlayerObjects[i] + kThrowObject);
  if (RawEnabled()) PutRawBack(stream);
  for (int i = 0; i < 2; i++)
    if (after[i] != before[i]) Put32(kPlayerObjects[i] + kThrowObject, after[i]);
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

std::vector<uint8_t> RawBytes(uint32_t stream, uint32_t offset, std::vector<Block>& blocks) {
  std::lock_guard<std::mutex> lock(g_raw_lock);
  auto it = g_raw.find(stream);
  if (it == g_raw.end()) return {};
  const Kept& k = it->second;
  uint32_t pos = offset;
  for (const Region& r : kRaw) {
    blocks.push_back({pos, r.size, r.address});
    pos += r.size;
  }
  for (const Region& r : k.dynamic) {
    blocks.push_back({pos, r.size, r.address});
    pos += r.size;
  }
  std::vector<uint8_t> out = k.fixed;
  out.insert(out.end(), k.dynamic_bytes.begin(), k.dynamic_bytes.end());
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

// A KO's replay is not fought: the netplay runs it as it runs the KO around it, on neutral inputs and without
// guesses (net/netplay.cpp RoundFought). Fought, with rollbacks in it, the two games' replays came apart: a player's
// press there ends the replay early (the manager also waits on camera slot 0's blend weight, +156), and the frame
// that ended it, when a rollback ran it again, left the cameras otherwise than its first run had (part of what they
// take in is worked out in the drawing, which a frame run again leaves out): with the next round's cameras apart, a
// round went its own way after a crossing over (Cervantes against Yoshimitsu with 40 ms each way, from frame 5790);
// and each player's +0xC4, which side of the screen the player is on (sub_8216C378: player 1 while the camera with
// the greatest blend weight is in its modes 1 and 3), differed in the replay's one check (Aeon Calcos against
// Siegfried, frame 2730), 2026-11-18. At the replay's end the game puts the present back.
bool InKoReplay() {
  const uint32_t m = g_replay_manager.load(std::memory_order_relaxed);
  return m >= 0x40000000u && m < 0xA0000000u && Get32(m) == 2;
}

// each player's raw pointers in the snapshot (sub_82141778 sets the first five; the rest point into loaded data too).
// +0x5BE90 is set by a throw that lands (a heap object of it): two PCs' heaps lie apart, so the check found every landed
// throw "out of step" though both fights went on alike (2026-11-17, the first matches between two PCs: Hilde's and
// Ezio's throws; a game with its heap 64 MB higher showed it on one PC, that word alone differing by the 64 MB)
constexpr uint32_t kPlayerPointers[] = {0x22638, 0x5BE90, 0x5CB44, 0x5CBF4, 0x5CCA4, 0x5CD54, 0x5CE04,
                                        0x5CEB4, 0x5D2B4, 0x5D30C, 0x5D364, 0x5D3BC, 0x5DACC, 0x5DAD0};

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
  // a static object past player 2's (the snapshot's 3152-byte block from 0x829F4E90) keeps two heap pointers (the
  // first set during a fight: Ezio against Nightmare, a game with its heap 64 MB higher)
  if (source >= 0x829F5528u && source < 0x829F5530u) return Kind::Pointer;
  // the battle's state machine keeps one at +0x14 (the menus' object it was set up from; net/netplay.cpp) and one at
  // +0x13C that a fight sets
  if ((source >= 0x82767D1Cu && source < 0x82767D20u) || (source >= 0x82767E44u && source < 0x82767E48u))
    return Kind::Pointer;
  // ... and the count that comes with +0x13C at +0x138: between rounds sub_8231F8D8 stores the pair that
  // sub_82148B90 works out from a player's pose (it hands sub_822998E8 the pose object at +0x3140), and the two games'
  // poses differ in their last bits: 1142 and 1126 in two games whose fights went on alike to the same result (Kilik
  // against Ezio with 40 ms each way, from round 2's first check, 2026-11-18)
  if (source >= 0x82767E40u && source < 0x82767E44u) return Kind::Pose;
  // ... and at +0xC44 a value sub_82210320 takes from the stage as the fight loads (sub_822134F8), -40.0 when the
  // stage's data is not in yet: how far the loading had got, different on two PCs, and nothing in the fight reads it
  // differently (two games with -40.0 and -5.5 there stayed alike in all else)
  if (source >= 0x829F59D4u && source < 0x829F59D8u) return Kind::Pointer;
  // ... and at +0x740 two words that go from 0xFFFF and 0 to a flag and a number between rounds (sub_8220F370, from a
  // task through sub_822130C8 and sub_82212720; the flag's high half cleared later by sub_82210548, on different frames
  // in the two games), the number different from game to game and run to run like the float at +0xC44: Elysium against
  // Edge Master as their next styles loaded, 1468 against 1477, 1383 against 1577, 1553 against 1387, the fights alike
  // in all else to the match's end and the next round on the same frame (40 ms each way, 2026-11-18)
  // (... and so in each of the 16 entries of that list, 64 bytes apart: sub_8220F370 adds an entry a kind of thing the
  // stage does, and a fight with more of them fills the next ones: the second's number 829 against 944 from frame 810
  // on stage 27, 537 against 684 from 3630 on stage 11, the third's 1563 against 1860, Algol against Yoshimitsu pushed
  // out of the ring with 60 ms each way, 2026-11-24; the fights alike in all else to the same result)
  if (source >= 0x829F55D0u && source < 0x829F55D0u + 16 * 64 && ((source - 0x829F55D0u) & 63) < 8) return Kind::Pointer;
  // ... and the number each of five effect slots is made with (the pool at 0x829F3330: 176 bytes a slot, taken in turn,
  // +880; sub_82184A58 from a move's script hands sub_821845F0 a word of its fighter's, which that keeps at the slot's
  // +16): 1 in one game and 2 in the other from frame 355 to the match's end, Algol against Yoshimitsu on stage 1C with
  // 60 ms each way, the fights alike in all else to the same result; and 2 in both in the next run of the same match.
  // Nothing reads it in the second after but the snapshot's own save and restore (SC5_DBG_WATCH_READS, 2026-11-24).
  if (source >= 0x829F3330u && source < 0x829F3330u + 5 * 176) {
    const uint32_t in_slot = (source - 0x829F3330u) % 176;
    if (in_slot >= 16 && in_slot < 20) return Kind::Pointer;
  }
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
      uint8_t v = data[b.pos + i];
      // a float's -0.0 as +0.0: the same number, which two PCs reached from two sides (a product of zero and a
      // value of the secondary animation, whose last bits differ there: Xiba against Leixia, a word or two of the
      // players' objects for a few frames, the fights alike in all else)
      if (v == 0x80 && ((b.source + i) & 3) == 0 && i + 4 <= b.size && !data[b.pos + i + 1] && !data[b.pos + i + 2] &&
          !data[b.pos + i + 3])
        v = 0;
      h = (h ^ v) * 1099511628211ull;
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

// More visual systems whose random numbers come from the visual states (rng.h), each drawing as often as there are
// objects that rollback does not put back, so that after a rollback each game drew its own number of times and the
// fight's generators parted (Aeon Calcos against Siegfried with 40 ms each way: a Park-Miller draw for P1's +0x5DAF0,
// sub_821DF1C8, came out apart at frame 240; counted per call site in both games' last runs of each frame):
//  - the forces' own updates, slot 1 of each force's class: the animated wind and the force by the floor
//    (sub_82191358, vtables 0x820BCDE8 and 0x820BCE0C), the point forces (sub_82191BE0, sub_82193200) and the gusts
//    (sub_821929F0, 0x820BCE78), which add a random wobble to their vectors (sub_82190EB8, TT800: three draws a force a
//    frame, 27 in one game and 32 to 37 in the other);
//  - an emitter of effects (sub_82373660: positions and directions at random, then sub_82373290 takes an effect id
//    from the effects' table at 0x82763440), which a cooldown in its own object lets fire (sub_82376F88): 17
//    Park-Miller draws at frame 219 in one game and none in the other.
#define SC5_VISUAL_SYSTEM(fn)                    \
  REX_EXTERN(__imp__##fn);                       \
  REX_HOOK_RAW(fn) {                             \
    sc5::rng::EnterVisual(ctx, base);            \
    __imp__##fn(ctx, base);                      \
    sc5::rng::LeaveVisual();                     \
  }
SC5_VISUAL_SYSTEM(sub_82191358)
SC5_VISUAL_SYSTEM(sub_82191BE0)
SC5_VISUAL_SYSTEM(sub_82193200)
SC5_VISUAL_SYSTEM(sub_821929F0)
//  - the stage's particles (sub_824ADBD8, from the task sub_82489500: its emitters sub_82488708, sub_82488E28 and
//    sub_82489028 draw each particle's place and speed from Park-Miller, 60 to 70 draws a frame on the stage with
//    water, ids 21 and 22), whose particles live in heap objects: a game that went back drew its own number of times,
//    and the next Park-Miller number the fight took came out apart: each player's +0x5DAE0 / +0x5DAF0 (sub_821DF318
//    from a move's script, sub_821DE088), then the fight itself (health 160 against 68 at frame 5640). Algol against
//    Yoshimitsu pushed out of the ring, 60 ms each way with jitter and loss: every check apart from frame 480 on
//    stage 21 and from 570 on stage 22 (2026-11-24); the players of 2026-11-20 lost matches on stage 22 this way.
SC5_VISUAL_SYSTEM(sub_824ADBD8)
//  - the effects' emitters, all of them (the module from sub_82373290 to sub_82379B20: every function of it that draws
//    from a generator itself; sub_82373660 was the first found). A stage has its own (sub_82211950 from the stage's
//    update fires sub_82374F70, whose sub_82373AF8 drew 6 Park-Miller numbers in one game and 9 in the other at frame
//    475 on stage 21, the frame before the draw above came out apart: found with SC5_DBG_DRAWS, tools/py/drawscmp.py),
//    so a match went out of step on the stages whose emitters nobody had met yet
SC5_VISUAL_SYSTEM(sub_823738C8)
SC5_VISUAL_SYSTEM(sub_82373AF8)
SC5_VISUAL_SYSTEM(sub_823761B8)
SC5_VISUAL_SYSTEM(sub_82376528)
SC5_VISUAL_SYSTEM(sub_82377488)
SC5_VISUAL_SYSTEM(sub_82377910)
SC5_VISUAL_SYSTEM(sub_82378468)
SC5_VISUAL_SYSTEM(sub_82378B58)
SC5_VISUAL_SYSTEM(sub_82378D80)
SC5_VISUAL_SYSTEM(sub_82378E58)
SC5_VISUAL_SYSTEM(sub_82379438)
SC5_VISUAL_SYSTEM(sub_82379508)
SC5_VISUAL_SYSTEM(sub_82379748)
SC5_VISUAL_SYSTEM(sub_823798B8)
SC5_VISUAL_SYSTEM(sub_82379B20)
SC5_VISUAL_SYSTEM(sub_82373660)
#undef SC5_VISUAL_SYSTEM

// The KO replay's manager, kept with each frame (DynamicRegions): its address as the battle's task hands it over
REX_EXTERN(__imp__sub_8211A8A8);
REX_HOOK_RAW(sub_8211A8A8) {
  sc5::battle::g_replay_manager.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_8211A8A8(ctx, base);
}
