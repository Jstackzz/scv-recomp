// The fight's HUD and its round markers, see hud.h.
//
// A round's end (sub_8213E180, from the "fighting" state's update in the battle's state machine, sub_82441628) gives its
// winner the round through sub_82146C48: the player's rounds won (+0x1108, a halfword; the match ends when it reaches
// +0x110C, the rounds to win; the game's other count at +0x75C goes up at the same time, sub_821882F0) and the HUD's
// own count of them (HUD +0x94 + 4 * side, the side the player's +0x1A8) each go up by one, and the marker lights
// (sub_8217D090, effect 1966 or 1967). The HUD draws its markers from its own count every frame (sub_82181588 from
// sub_82182898: five a side, marker n lit while n <= the count), and sets it from the player's count only as each round
// starts (sub_8231CAA0, from the round's call). The players' objects are in every snapshot; the HUD, a heap object, is
// not. So a frame run again over a KO counted the round on the HUD once more (its first run and its rerun), and a KO
// that came only on a guess, which the real input then took back, stayed on the HUD: a player shown with a round more
// than they had until the next round's call, and to the result after the match's last round (four markers in a first
// to 3). Found with SC5_DBG_WATCH on the HUD's count: with no lag at all a rollback over round 1's KO at fight frame
// 935 counted it twice (2026-11-24).
#include "sc5/hud.h"

#include <cstdlib>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/battle.h"
#include "sc5/frame.h"
#include "sc5/game.h"

namespace sc5::hud {
namespace {

constexpr uint32_t kHudRef = 0x829F2F98;
constexpr uint32_t kPlayers[2] = {0x82937070, 0x82994BA0};
constexpr uint32_t kRoundsWon = 0x1108;  // the player's rounds won (halfword), which the HUD's count follows
constexpr uint32_t kRoundsWonToo = 0x75C;  // the game's other count of them (a word)
constexpr uint32_t kSide = 0x1A8;  // the player's side (a byte: 0 player 1, 1 player 2)

uint32_t Get32(uint32_t a) {
  return __builtin_bswap32(*REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint32_t*>(a));
}
uint32_t Get16(uint32_t a) {
  return __builtin_bswap16(*REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint16_t*>(a));
}
uint32_t Get8(uint32_t a) { return *REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(a); }

}  // namespace

uint32_t Object() {
  if (!REX_KERNEL_STATE()) return 0;
  const uint32_t hud = Get32(kHudRef + 4);
  if (hud < 0x40000000u || hud >= 0xA0000000u) return 0;
  return Get32(hud + 48) == Get32(kHudRef) ? hud : 0;
}

bool KeepMarkers() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_HUD_KEEP");
    return !(v && v[0] == '0');
  }();
  return on;
}

void OnFrameEnd() {
  // the frame as the player sees it: its first run, in a fight, not a KO's replay (it plays the round back with the
  // players' counts from before the KO, while the HUD keeps the present's)
  if (frame::Replaying() || game::FightFrame() < 0) return;
  // a KO the frames run again took back (it came on a wrong guess): the battle "fighting" again after a frame shown in
  // its KO, and no replay
  const uint32_t state = game::BattleState();
  static uint32_t last = 0;
  if (last == game::kBattleKO && state == game::kBattleFighting && !battle::InKoReplay())
    REXLOG_INFO("sc5 hud: fight frame {}: a KO shown in the frame before was taken back", game::FightFrame());
  last = state;
  if (battle::InKoReplay()) return;
  // (0xBC and 0x160: the match is over, the winner's pose)
  if (state != game::kBattleFighting && state != game::kBattleKO && state != game::kBattleBetweenRounds &&
      state != 0xBC && state != 0x160)
    return;
  const uint32_t hud = Object();
  if (!hud) return;
  uint32_t shown[2] = {Get32(hud + kMarkers), Get32(hud + kMarkers + 4)}, won[2] = {}, too[2] = {};
  for (uint32_t p : kPlayers) {
    const uint32_t side = Get8(p + kSide) & 1;
    won[side] = Get16(p + kRoundsWon);
    too[side] = Get32(p + kRoundsWonToo);
  }
  static bool apart = false;
  static int told = 0;
  const bool now_apart = shown[0] != won[0] || shown[1] != won[1];
  if (now_apart == apart) return;
  apart = now_apart;
  if (told++ >= 200) return;
  REXLOG_INFO("sc5 hud: fight frame {} (battle {:X}): markers {}-{}, rounds won {}-{} (+0x75C {}-{}){}",
              game::FightFrame(), state, shown[0], shown[1], won[0], won[1], too[0], too[1],
              now_apart ? ": the markers are wrong" : ": the markers are right again");
}

}  // namespace sc5::hud
