// A VS fight's setup, see versus.h.
//
// sub_8243D2A0 copies the VS settings record (r4: two sides of 88 bytes, then the match's) into the battle setup;
// sub_8243D430 then sets the players up from it. The record (0x402D075C in tests):
//   side +0  1 a player, 2 the CPU          +4  the input: 1 a pad, 2 the CPU's
//        +8  the rounds to win ("Battle Count", 3 unless changed)   +12, +16  health 240.0   +20  240
//        +48 1, then nine words 0x22
//   +176 the round time in seconds (60)     +208 the stage (Random already drawn: 0x25 with seed 12345)
// Online, player 1's game sets the rounds and sends its record's stage, round time and rounds to player 2's game,
// which waits for them at its own setup and takes them (net::SendSetup / WaitSetup): both fight on player 1's stage
// whatever each game's Random drew. The VS menus leave player 2 to the CPU (only the first user is signed in): after
// the setup, player 2 becomes the second pad's (input mode 1, pad 1), which the netplay answers for.

#include "sc5/versus.h"

#include <atomic>
#include <cstdlib>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/autopilot.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/net/netplay.h"

REX_EXTERN(__imp__sub_8243D2A0);
REX_EXTERN(__imp__sub_8243D430);

namespace sc5::versus {
namespace {

std::atomic<bool> g_online{false};
std::atomic<int> g_side{0}, g_wins{2};

uint32_t Get(uint32_t a) {
  return __builtin_bswap32(*REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint32_t*>(a));
}
void Put(uint32_t a, uint32_t v) {
  *REX_KERNEL_STATE()->memory()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v);
}

constexpr uint32_t kSide = 88, kRounds = 8, kRoundTime = 176, kStage = 208;

// What both games must have alike from the battle setup on, which player 2's game takes from player 1's: the random
// number generators the setup does not seed (Park-Miller and TT800, 0x829249F0-0x82924A87, sc5/rng.cpp) and the
// counters that started at boot (as the netplay's barrier agrees them again: the loading in between draws from them),
// but the three that count frames presented to the GPU, which each game's own GPU waits by (battle.cpp's kRaw).
// The C library's rand() and the Mersenne Twister both games seed with the match's seed (sub_824A35A8).
std::vector<uint32_t> SetupAddresses() {
  std::vector<uint32_t> out;
  for (uint32_t a = 0x829249F0; a < 0x829249F0 + 0x98; a += 4) out.push_back(a);
  for (uint32_t a : {0x82771B40u, 0x82771B44u, 0x82771B48u, 0x82771B4Cu, 0x82771B50u, 0x82771B54u, 0x82771B58u,
                     0x82771B5Cu, 0x828E8790u, 0x835B3EB4u})
    out.push_back(a);
  return out;
}

}  // namespace

void SetOnline(bool on, int side, int wins) {
  g_side = side;
  g_wins = wins < 1 ? 1 : wins > 5 ? 5 : wins;
  g_online = on;
}

}  // namespace sc5::versus

// the VS settings copied into the battle setup
REX_HOOK_RAW(sub_8243D2A0) {
  using namespace sc5::versus;
  const uint32_t rec = ctx.r4.u32;
  sc5::autopilot::OnSetup();  // the stage select's pick taken: told before player 2's game waits below for player 1's
  if (rec)
    REXLOG_INFO("sc5 versus: VS settings at {:08X}: sides {}/{} and {}/{}, rounds to win {} and {}, round time {}, stage "
                "{:X} (frame {})",
                rec, Get(rec), Get(rec + 4), Get(rec + kSide), Get(rec + kSide + 4), Get(rec + kRounds),
                Get(rec + kSide + kRounds), Get(rec + kRoundTime), Get(rec + kStage), sc5::frame::Count());
  if (g_online && rec) {
    const std::vector<uint32_t> addresses = SetupAddresses();
    if (g_side == 0) {
      for (uint32_t s = 0; s < 2; s++) Put(rec + s * kSide + kRounds, uint32_t(g_wins.load()));
      sc5::net::Setup mine{Get(rec + kStage), Get(rec + kRoundTime), uint32_t(g_wins.load()), {}};
      for (uint32_t a : addresses) mine.words.push_back(Get(a));
      sc5::net::SendSetup(mine);
      REXLOG_INFO("sc5 versus: stage {:X}, round time {}, {} rounds to win, sent to player 2's game", mine.stage,
                  mine.round_time, mine.wins);
    } else {
      sc5::net::Setup theirs;
      if (sc5::net::WaitSetup(theirs, 20000)) {
        Put(rec + kStage, theirs.stage);
        Put(rec + kRoundTime, theirs.round_time);
        for (uint32_t s = 0; s < 2; s++) Put(rec + s * kSide + kRounds, theirs.wins);
        for (size_t i = 0; i < addresses.size() && i < theirs.words.size(); i++) Put(addresses[i], theirs.words[i]);
        REXLOG_INFO("sc5 versus: player 1's stage {:X}, round time {}, {} rounds to win, {} words", theirs.stage,
                    theirs.round_time, theirs.wins, theirs.words.size());
      } else {
        REXLOG_WARN("sc5 versus: no setup from player 1's game: this game's own (the fight will not match)");
      }
    }
    // the C library's rand() and the Mersenne Twister from the match's seed on both
    const int64_t seed = sc5::game::FightSeed();
    if (seed >= 0) sc5::game::Reseed(ctx, base, uint32_t(seed));
  }
  __imp__sub_8243D2A0(ctx, base);
}

// The round's call (sub_82323DD0, from the battle's state machine) first asks whether a sound from before still plays
// (an audio object's +380, the setup's +180): if so it stops it and its state (0x82767D2C) keeps 1 at +0xC, else 2.
// That follows the audio's own timing, so after a first match two PCs took different ways and their round calls ran
// apart (a desync in the second match's first check). The call's state always keeps 2 here (the way with nothing
// playing; a sound still playing is stopped all the same).
REX_EXTERN(__imp__sub_82323DD0);
REX_HOOK_RAW(sub_82323DD0) {
  __imp__sub_82323DD0(ctx, base);
  constexpr uint32_t kCallAfterSound = 0x82767D2C + 0xC;
  if (sc5::versus::Get(kCallAfterSound) == 1) sc5::versus::Put(kCallAfterSound, 2);
}

// the players set up: online, player 2 is the second pad's
REX_HOOK_RAW(sub_8243D430) {
  __imp__sub_8243D430(ctx, base);
  static const bool test_human = [] { const char* v = std::getenv("SC5_TEST_P2_HUMAN"); return v && v[0] == '1'; }();
  if (!sc5::versus::g_online && !sc5::net::Active() && !test_human) return;
  constexpr uint32_t kP2 = 0x82994BA0;
  sc5::versus::Put(kP2 + 628, 1);  // the input: a pad
  sc5::versus::Put(kP2 + 432, 1);  // which: the second
  REXLOG_INFO("sc5 versus: player 2 is the second pad's");
}
