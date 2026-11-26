// A player's preferred side, see mirror.h.
//
// What the fight is on this PC stays exactly what it is on the other: only how this game shows it and how its own
// player's controller is read change.
//
// - The picture. The game draws its 3D scene into targets of its own, then puts that picture on the screen with one
//   full-screen draw (its final pass: the colour grading and the picture's colour matrix), then draws the HUD and every
//   text over it. The GPU (the SDK's MirrorSceneFromNow, docs\SDK.md) draws that one pass reflected about the middle of
//   the screen: the scene is a mirror image (into the screen stays into the screen, up stays up; nothing is culled
//   otherwise, the scene itself being drawn as it always is), and what comes after it, the HUD, the round's call, the
//   names and every text, as it always is. The camera, which is part of the fight (each player's +0xC4, docs\GAME.md
//   "The battle's cameras"), is left alone.
// - The controls. While the fight is mirrored and runs (not stood still in a menu: Training's, the pause menu), the left
//   and right of the player's own controller and keyboard (d-pad and left stick) are swapped where the PC's controller
//   is read (input::ReadPhysical), after the launcher's controls: the game offline and the netplay online take them so,
//   and "forward" stays toward the other fighter as this player sees them. The netplay sends that input; the other game
//   and the spectators get it as any input. Scripted test inputs are the simulation's and never come this way.
// - The sound: the front and surround channels' left and right swapped in what the game hands the system.
//
// Which player it is for: online the start line's side (a spectator's game never mirrors); offline the one player on a
// controller (the other the CPU: VS CPU, Training, Arcade, Story...), none with two (the offline versus). The game
// mirrors when that player's fighter would otherwise start on the side the player does not prefer: player 2 (on the
// right) who prefers the left, player 1 who prefers the right.

#include "sc5/mirror.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/input.h"
#include "sc5/versus.h"

namespace sc5::mirror {
namespace {

constexpr uint32_t kPlayers[2] = {0x8294AC10, 0x829A8740};
constexpr uint32_t kInputMode = 628;  // a player's input: 1 a pad, 2 the CPU (versus.cpp)
constexpr uint32_t kInputPad = 432;   // which pad
constexpr uint32_t kFightCount = 0x82949D54;  // the fight's frame count: stands still while a menu pauses the fight

std::atomic<int> g_preferred{int(Side::kOff)};
std::atomic<bool> g_mirrored{false};
std::atomic<int> g_swap_user{-1};  // the user index whose left and right are swapped now, -1 none
bool g_gpu_mirrored = false;       // what the GPU was told last
uint32_t g_last_count = 0;
int g_still = 0;
std::string g_told;  // the log's last line about it

// The fight is on the screen: the battle's states from the intro to the winner's pose (0xBC) and the result screen
// after it (0x160, its menu over a close-up of the winner, which stays mirrored with the pose it follows: turned back
// there, the game's fade from the pose's last picture showed that picture turned, its subtitle reversed). Not the
// menus, not the loading screen (the VS screen).
bool FightShown() {
  const uint32_t state = game::BattleState();
  return state != 0 && state != game::kBattleIdle && state != game::kBattleLoading;
}

// Which player this game's player is (0 player 1, 1 player 2) and the user index of their own controller
bool OwnPlayer(int& side, int& user, const char*& why) {
  const int online = versus::OnlineSide();
  if (online == 2) {
    why = "a spectator's game";
    return false;
  }
  if (online == 0 || online == 1) {
    side = online;
    user = 0;  // the netplay sends the first controller and the keyboard as this player's
    why = "online";
    return true;
  }
  int humans = 0;
  for (int p = 0; p < 2; p++) {
    if (game::ReadU32(kPlayers[p] + kInputMode) != 1) continue;
    humans++;
    side = p;
    user = int(game::ReadU32(kPlayers[p] + kInputPad));
  }
  if (humans != 1 || user < 0 || user > 3) {
    why = humans ? "two players on this PC" : "no player on a controller";
    return false;
  }
  why = "offline, one player";
  return true;
}

}  // namespace

void SetPreferred(Side side) {
  if (g_preferred.exchange(int(side)) != int(side))
    REXLOG_INFO("sc5 mirror: preferred side {} (the launcher's setting)", SideName(side));
}

Side Preferred() { return Side(g_preferred.load(std::memory_order_relaxed)); }

Side SideFromText(const char* text) {
  if (!text) return Side::kOff;
  if (_stricmp(text, "left") == 0) return Side::kLeft;
  if (_stricmp(text, "right") == 0) return Side::kRight;
  return Side::kOff;
}

const char* SideName(Side side) {
  return side == Side::kLeft ? "left" : side == Side::kRight ? "right" : "off";
}

bool Mirrored() { return g_mirrored.load(std::memory_order_relaxed); }

void OwnPad(uint32_t user, input::Pad& pad) {
  if (int(user) != g_swap_user.load(std::memory_order_relaxed)) return;
  const uint16_t lr = input::kDpadLeft | input::kDpadRight;
  const uint16_t held = pad.buttons & lr;
  if (held == input::kDpadLeft || held == input::kDpadRight) pad.buttons = uint16_t((pad.buttons & ~lr) | (held ^ lr));
  pad.lx = pad.lx == -32768 ? int16_t(32767) : int16_t(-pad.lx);
}

void BeforeDraw() {
  const Side want = Preferred();
  bool mirrored = false;
  int user = -1;
  std::string told = "off";
  // whose fight this is, as it was when the fight came on the screen: an online match's game is parked (the
  // launcher's park after the result) while its winner's pose may still play, and the picture does not turn then
  static int fight_side = -1, fight_user = -1;
  static const char* fight_why = "";
  const bool shown = FightShown();
  if (!shown) fight_side = -1;
  if (want != Side::kOff && shown) {
    int side = -1, own = -1;
    const char* why = "";
    bool known = OwnPlayer(side, own, why);
    if (known) {
      fight_side = side;
      fight_user = own;
      fight_why = why;
    } else if (fight_side >= 0 && versus::OnlineSide() < 0) {
      side = fight_side;
      own = fight_user;
      why = fight_why;
      known = true;
    }
    if (known) {
      mirrored = (want == Side::kLeft && side == 1) || (want == Side::kRight && side == 0);
      told = fmt::format("{} for player {} ({}, controller {}), who prefers the {}", mirrored ? "mirrored" : "as it is",
                         side + 1, why, own, SideName(want));
      if (mirrored) user = own;
    } else {
      told = std::string("as it is: ") + why;
    }
  }
  // the fight stood still for two frames (a menu over it): the controller as it is there
  const uint32_t count = game::ReadU32(kFightCount);
  g_still = count == g_last_count ? g_still + 1 : 0;
  g_last_count = count;
  const bool paused = g_still >= 2 && game::BattleState() == game::kBattleFighting;
  g_mirrored = mirrored;
  g_swap_user = mirrored && !paused ? user : -1;
  if (shown) told = fmt::format("(battle state {:X}) ", game::BattleState()) + told;
  if (told != g_told) {
    REXLOG_INFO("sc5 mirror: frame {}: the fight {}", frame::Count(), told);
    g_told = told;
  }
  auto* gpu = rex::Runtime::instance() ? rex::Runtime::instance()->graphics_system() : nullptr;
  if (mirrored != g_gpu_mirrored && gpu) {
    g_gpu_mirrored = mirrored;
    gpu->MirrorSceneFromNow(mirrored);
  }
  // diagnostics: SC5_DBG_DRAWLOG=<fight frame>[:<frames>],... logs the GPU's draws of those frames (the SDK's draw log)
  static const std::vector<std::pair<int64_t, uint32_t>> logs = [] {
    std::vector<std::pair<int64_t, uint32_t>> out;
    if (const char* v = std::getenv("SC5_DBG_DRAWLOG"))
      for (const char* p = v; *p;) {
        char* end = nullptr;
        const int64_t at = std::strtoll(p, &end, 10);
        if (end == p) break;
        uint32_t n = 1;
        if (*end == ':') n = uint32_t(std::strtoul(end + 1, &end, 10));
        out.emplace_back(at, n);
        p = end;
        while (*p == ',') p++;
      }
    return out;
  }();
  for (auto [at, n] : logs)
    if (at == game::FightFrame() && gpu) {
      REXLOG_INFO("sc5 mirror: the GPU's draws of fight frames {} to {} into the log", at, at + n - 1);
      gpu->LogDrawsFromNow(n);
    }
}

void SwapSound(uint32_t* samples) {
  if (!samples || !Mirrored()) return;
  // the channels one after the other, 256 samples each: front left, front right, centre, LFE, surround left and right
  constexpr int kN = 256;
  for (int pair : {0, 4})
    for (int i = 0; i < kN; i++) std::swap(samples[pair * kN + i], samples[(pair + 1) * kN + i]);
}

}  // namespace sc5::mirror
