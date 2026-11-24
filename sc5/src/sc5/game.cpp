// What the game is doing, see game.h.

#include "sc5/game.h"
#include "sc5/hud.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"

REX_EXTERN(__imp__sub_824A35A8);
REX_EXTERN(sub_824A35A8);  // seeds the game's random numbers: srand(seed >> 4) and its Mersenne Twister

namespace sc5::dbg {
void OnFrameEnd();  // TEMPORARY (debug_watch.cpp)
}

namespace sc5::game {
namespace {

// a seed every call of sub_824A35A8 uses instead of its own (SC5_TEST_SEED, later netplay's agreed one), or none
std::atomic<int64_t> g_seed_override{-1};

// SC5_TEST_SEED, read before the game first seeds (main's setup, before the first frame)
int64_t SeedOverride() {
  static const bool read_env = [] {
    if (const char* seed = std::getenv("SC5_TEST_SEED"); seed && *seed)
      g_seed_override.store(std::strtoll(seed, nullptr, 0), std::memory_order_relaxed);
    return true;
  }();
  (void)read_env;
  return g_seed_override.load(std::memory_order_relaxed);
}

constexpr uint32_t kBattleMachine = 0x82767D08;  // the battle's state machine: +0 the current state, +4 the pending one

std::atomic<int> g_sequence{kSeqNone};
// the fight clock: armed when a fight starts loading, started at round 1's call
bool g_fight_armed = false;
int64_t g_fight_start = -1;
uint32_t g_last_battle = 0xFFFFFFFF;

// SC5_TEST_HASH=1: each fight frame's players hashed into the log, to compare two runs with the same inputs
constexpr uint32_t kPlayer1 = 0x82937070, kPlayer2 = 0x82994BA0, kPlayerSize = 0x5DB30;
int g_test_hash = -1;
// SC5_TEST_DUMP=<fight frame>,...: the players' objects written raw at those fight frames into SC5_TEST_DUMP_DIR
std::vector<int64_t> g_dump_frames;
std::string g_dump_dir;

uint64_t Hash(uint32_t address, uint32_t size) {
  auto* ks = REX_KERNEL_STATE();
  const uint8_t* p = ks->memory()->TranslateVirtual<const uint8_t*>(address);
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i + 8 <= size; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
    h ^= h >> 29;
  }
  return h;
}

}  // namespace

uint32_t ReadU32(uint32_t address) {
  auto* ks = REX_KERNEL_STATE();
  if (!ks || address < 0x10000 || address >= 0xE0000000u) return 0;
  return __builtin_bswap32(*ks->memory()->TranslateVirtual<const uint32_t*>(address));
}

int SequenceId() { return g_sequence.load(std::memory_order_relaxed); }

const char* SequenceName(int id) {
  switch (id) {
    case kSeqSetup: return "Setup";
    case kSeqAttract: return "Attract";
    case kSeqTitle: return "Title";
    case kSeqAstralHome: case 18: return "AstralHome";
    case kSeqEntry: return "Entry";
    case kSeqGame: return "Game";
    case kSeqSignOut: return "SignOut";
    case kSeqVersus: return "VSStandard";
    case 10: return "VSSpecial";
    case kSeqArcade: return "Arcade";
    case kSeqCreation: return "Creation";
    case kSeqOption: return "Option";
    case kSeqTraining: return "Training";
    case kSeqLogo: return "LogoSequence";
    case kSeqSample: return "Sample";
    case kSeqWorldMap: return "WorldMap";
    case kSeqOnline: return "Online";
    case kSeqQuickBattle: return "QuickBattle";
    case kSeqLegendarySouls: return "LegendaryOfSouls";
    case kSeqInvite: return "Invite";
    default: return "?";
  }
}

uint32_t BattleState() {
  const uint32_t cur = ReadU32(kBattleMachine);
  return cur > kBattleMachine && cur < kBattleMachine + 0x200 ? cur - kBattleMachine : 0;
}

const char* BattleStateName(uint32_t state) {
  switch (state) {
    case 0: return "none";
    case kBattleIdle: return "idle";
    case kBattleLoading: return "loading";
    case kBattleIntro: return "intro";
    case kBattleRoundCall: return "round call";
    case kBattleFighting: return "fighting";
    case kBattleKO: return "KO";
    case kBattleBetweenRounds: return "between rounds";
    default: return "other";
  }
}

int64_t FightFrame() {
  return g_fight_start < 0 ? -1 : int64_t(sc5::frame::Count()) - g_fight_start;
}

void OnFrameEnd(uint64_t frame) {
  if (g_test_hash < 0) {
    const char* v = std::getenv("SC5_TEST_HASH");
    g_test_hash = v && v[0] == '1';
    if (const char* d = std::getenv("SC5_TEST_DUMP"); d && *d) {
      for (const char* p = d; *p;) {
        g_dump_frames.push_back(std::strtoll(p, const_cast<char**>(&p), 10));
        while (*p == ',' || *p == ' ') p++;
      }
      const char* dir = std::getenv("SC5_TEST_DUMP_DIR");
      g_dump_dir = dir ? dir : ".";
    }
  }
  if (g_fight_start >= 0 && !g_dump_frames.empty()) {
    const int64_t ff = int64_t(frame) - g_fight_start;
    for (int64_t want : g_dump_frames) {
      if (want != ff) continue;
      auto* ks = REX_KERNEL_STATE();
      for (auto [name, addr, size] : {std::tuple{"p1", kPlayer1, kPlayerSize}, std::tuple{"p2", kPlayer2, kPlayerSize},
                                      std::tuple{"machine", kBattleMachine, 0x200u}}) {
        const std::string path = g_dump_dir + "/" + name + "_" + std::to_string(ff) + ".bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(ks->memory()->TranslateVirtual<const uint8_t*>(addr), 1, size, f);
          std::fclose(f);
        }
      }
      REXLOG_INFO("sc5: fight frame {} dumped into {}", ff, g_dump_dir);
    }
  }
  const uint32_t battle = BattleState();
  if (g_test_hash && g_fight_start >= 0) {
    REXLOG_INFO("sc5: fight frame {} {} P1 {:016X} P2 {:016X} | P1+23C8 {:08X} {:08X}", int64_t(frame) - g_fight_start,
                BattleStateName(battle), Hash(kPlayer1, kPlayerSize), Hash(kPlayer2, kPlayerSize),
                ReadU32(kPlayer1 + 0x23C8), ReadU32(kPlayer1 + 0x23CC));
  }
  if (battle != g_last_battle) {
    REXLOG_INFO("sc5: frame {} battle {} -> {} ({})", frame, BattleStateName(g_last_battle), BattleStateName(battle),
                SequenceName(SequenceId()));
    // a fight loads, or VS Battle's Rematch starts one that stayed loaded (from the result straight to "between rounds")
    if (battle == kBattleLoading || (battle == kBattleBetweenRounds && g_last_battle != kBattleKO)) {
      g_fight_armed = true;
      g_fight_start = -1;
    } else if (battle == kBattleRoundCall &&
               (g_fight_armed || (g_last_battle == kBattleIdle && SequenceId() == kSeqTraining))) {
      // (Training goes from its stage select straight to the round's call, with no loading between)
      g_fight_armed = false;
      g_fight_start = int64_t(frame);
      REXLOG_INFO("sc5: fight clock started at frame {}", frame);
    } else if (battle == kBattleIdle || battle == 0) {
      g_fight_armed = false;
      g_fight_start = -1;
    }
    g_last_battle = battle;
  }
  sc5::hud::OnFrameEnd();  // the round markers against the rounds won (logged)
  sc5::dbg::OnFrameEnd();  // TEMPORARY (debug_watch.cpp)
  // TEMPORARY: SC5_TEST_STALL=<fight frame>:<ms>,...: the frame's end held that long (does the game follow the clock?)
  static std::vector<std::pair<int64_t, int>> stalls = [] {
    std::vector<std::pair<int64_t, int>> out;
    if (const char* v = std::getenv("SC5_TEST_STALL"))
      for (const char* p = v; *p;) {
        const int64_t ff = std::strtoll(p, const_cast<char**>(&p), 10);
        int ms = 50;
        if (*p == ':') ms = int(std::strtol(p + 1, const_cast<char**>(&p), 10));
        out.emplace_back(ff, ms);
        while (*p == ',') p++;
      }
    return out;
  }();
  if (g_fight_start >= 0 && !sc5::frame::Replaying())
    for (auto [ff, ms] : stalls)
      if (int64_t(frame) - g_fight_start == ff) {
        REXLOG_INFO("sc5: fight frame {}: stalled {} ms", ff, ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
}

void SetSeed(int64_t seed) { g_seed_override.store(seed, std::memory_order_relaxed); }

std::atomic<int64_t> g_last_seed{-1};
std::atomic<uint32_t> g_home{0};
uint32_t HomeObject() { return g_home.load(std::memory_order_relaxed); }

int64_t FightSeed() {
  const int64_t over = SeedOverride();
  return over >= 0 ? over : g_last_seed.load(std::memory_order_relaxed);
}

void Reseed(PPCContext& ctx, uint8_t* base, uint32_t seed) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = seed;
  ctx.lr = 0x82189268;  // as from main's loop
  sub_824A35A8(ctx, base);
  ctx = saved;
}

void SequenceCreated(int id) {
  const int was = g_sequence.exchange(id, std::memory_order_relaxed);
  if (was != id) REXLOG_INFO("sc5: sequence {} ({})", SequenceName(id), id);
}

}  // namespace sc5::game

// The sequence creators' first instructions (config\sc5_codegen.toml, "Sequences"): which screen comes next.
#define SC5_SEQUENCE_HOOK(addr, id) \
  void Sc5Sequence_##addr() { sc5::game::SequenceCreated(id); }
SC5_SEQUENCE_HOOK(82328C48, 1)
SC5_SEQUENCE_HOOK(82328CE8, 2)
SC5_SEQUENCE_HOOK(82328D88, 3)
SC5_SEQUENCE_HOOK(82328E70, 4)
SC5_SEQUENCE_HOOK(82328F50, 6)
SC5_SEQUENCE_HOOK(82328FE8, 7)
SC5_SEQUENCE_HOOK(82329060, 12)
SC5_SEQUENCE_HOOK(823290D8, 13)
SC5_SEQUENCE_HOOK(82329150, 8)
SC5_SEQUENCE_HOOK(823291F8, 9)
SC5_SEQUENCE_HOOK(82329298, 11)
SC5_SEQUENCE_HOOK(82329338, 14)
SC5_SEQUENCE_HOOK(823293B0, 15)
SC5_SEQUENCE_HOOK(82329428, 17)
SC5_SEQUENCE_HOOK(823294C0, 19)
SC5_SEQUENCE_HOOK(82329560, 20)
SC5_SEQUENCE_HOOK(82329600, 22)
SC5_SEQUENCE_HOOK(823296A0, 23)
SC5_SEQUENCE_HOOK(82329740, 25)

// AstralHome's creator (the main menu, sequence 4): makes its 1088-byte object and puts its handle at [r3] and the
// object at [r3 + 4]
REX_EXTERN(__imp__sub_82328E70);
REX_HOOK_RAW(sub_82328E70) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_82328E70(ctx, base);
  sc5::game::g_home = sc5::game::ReadU32(out + 4);
  REXLOG_INFO("sc5: the main menu's object at {:08X} (frame {})", sc5::game::g_home.load(), sc5::frame::Count());
}

// The game's random numbers are seeded by sub_824A35A8: at boot from the date and time (sub_82163490), and again
// as a battle is set up from a value its own online code agrees on (sub_823A0070, sub_823A22C8, the seed at
// +0x19444). Every PC in a match, and every run of a test, must draw the same numbers.
REX_HOOK_RAW(sub_824A35A8) {
  const int64_t over = sc5::game::SeedOverride();
  REXLOG_INFO("sc5: random numbers seeded with {:08X}{} (from {:08X}, frame {})", ctx.r3.u32,
              over >= 0 ? fmt::format(", made {:08X}", uint32_t(over)) : std::string(), uint32_t(ctx.lr),
              sc5::frame::Count());
  if (over >= 0) ctx.r3.u64 = uint32_t(over);
  sc5::game::g_last_seed.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_824A35A8(ctx, base);
}

// The battle state snapshot the game just saved (sc5_codegen.toml): with SC5_TEST_HASH=1 its hash into the log
void Sc5BattleSnapshotSaved(PPCRegister& r31) {
  using namespace sc5::game;
  if (g_test_hash != 1) return;
  const uint32_t stream = r31.u32;
  const uint32_t used = ReadU32(stream + 4);
  if (!used || used > 0x28000) return;
  REXLOG_INFO("sc5: fight frame {} snapshot {} bytes hash {:016X}", FightFrame(), used, Hash(stream + 12, used & ~7u));
  if (const char* dir = std::getenv("SC5_TEST_DUMP_DIR"); dir && *dir) {
    const std::string path = std::string(dir) + "/snap_" + std::to_string(FightFrame()) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      auto* ks = REX_KERNEL_STATE();
      std::fwrite(ks->memory()->TranslateVirtual<const uint8_t*>(stream + 12), 1, used, f);
      std::fclose(f);
    }
  }
}
