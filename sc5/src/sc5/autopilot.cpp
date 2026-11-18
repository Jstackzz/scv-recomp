// The launcher's autopilot, see autopilot.h.
//
// Where the game is comes from its sequence (game::SequenceId: Title, AstralHome the main menu, VSStandard VS
// Battle), the battle's state machine (game::BattleState) and the select screen (sc5/select.h). The menus, with the
// fresh save every online session has (docs\GAME.md, "Sequences"):
//   Title: START. AstralHome: a first-visit window that A closes, the cursor on Story: the hold point.
//   From there, by the main menu's cursors (ReadHome): Offline Play, A; VS Battle, A: VSStandard, its settings menu on
//   Start Match: A. The select screen: player 1's side picks a character (A) and a costume (A), then player 2's side,
//   both with the first pad ("Player VS CPU": the battle setup makes player 2 the second pad's, sc5/versus.cpp). Then
//   the stage (RIGHT from Random as the seed says, A: the same on both PCs), loading, the fight.
//   After the last round: the winner's pose, the Results screen (A: Next), then VS Battle's menu (Rematch, Character
//   Select, Return to Astral Home). A fight's pause menu (START) leaves it for park.
// The autopilot presses the first pad's buttons through input::SetOverride; while a match's frames run in sync the
// netplay's inputs answer instead.
//
// The picks: a side the launcher named (a roster id or a game's character id with a costume) is picked by the
// autopilot on both PCs: its cursor steered to the slot one press at a time, A, the costume panel turned to the
// costume, A. A side named "-" online is picked by its player on their own PC with their own controller (the first
// pad: B, BACK, START and the rest held back, A only on a slot both games can load), and the other game follows it:
// the cursor where theirs is, then their character and costume (sc5/net/netplay.h, PICK).

#include "sc5/autopilot.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <system_error>

#include <fmt/format.h>
#include <rex/logging.h>

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/input.h"
#include "sc5/net/netplay.h"
#include "sc5/select.h"

namespace sc5::autopilot {
namespace {

enum class Phase { Off, Boot, Home, Hold, ToVersus, Settings, Select, Stage, Fight, Result, Leave, Again, Back, Lost, Script };

const char* PhaseName(Phase p) {
  switch (p) {
    case Phase::Off: return "off";
    case Phase::Boot: return "boot";
    case Phase::Home: return "main menu";
    case Phase::Hold: return "hold point";
    case Phase::ToVersus: return "to VS Battle";
    case Phase::Settings: return "VS Battle's settings";
    case Phase::Select: return "select screen";
    case Phase::Stage: return "stage select";
    case Phase::Fight: return "fight";
    case Phase::Result: return "result";
    case Phase::Leave: return "back to the main menu";
    case Phase::Again: return "rematch";
    case Phase::Back: return "back out to the main menu";
    case Phase::Lost: return "lost";
    case Phase::Script: return "test script";
  }
  return "?";
}

constexpr uint32_t kPlayer[2] = {0x82937070, 0x82994BA0};
// in each player's object: the rounds that side won in the match (+0x784 holds the same; +0x728 counted one more
// than the rounds won by the end of a 3-0 test match)
constexpr uint32_t kRoundsWon = 0x75C;

struct Press {
  uint16_t buttons;
  int hold, after;
};

bool g_launcher = false;
Phase g_phase = Phase::Off;
uint64_t g_since = 0;
Plan g_plan;
std::deque<Press> g_steps;
int g_step_t = 0;
input::Pad g_pad;      // what the autopilot presses on the first pad this frame
bool g_local = false;  // the first pad is the local player's own this frame (with g_allow)
uint16_t g_allow = 0;  // the local player's buttons let through
int g_picked_slot[2] = {-1, -1}, g_picked_costume[2] = {-1, -1};
int g_last_step[2] = {0, 0};
int g_last_seq = -1;
uint32_t g_last_battle = 0xFFFFFFFF;
bool g_fought = false, g_over = false, g_over_reported = false;
uint64_t g_over_at = 0;
int g_won[2] = {0, 0};
bool g_reselect = false;     // a rematch goes through Character Select (always so far: Rematch keeps state apart)
bool g_menu_pressed = false;  // the after-result presses went out (Leave, Again)
uint32_t g_skip_logged = 0;   // the battle state whose skipping with A the log said last
bool g_both = false;          // this frame's press goes to both pads (each player skips their own intro)
int g_last_rights = 0;        // the last match's stage: that many to the right of Random on the stage select
bool g_same_stage = false;    // the next stage select picks the last match's stage (a rematch, the same two fighters)
bool g_fresh_save = true;     // the save has never seen the main menu (its first-visit window comes)
bool g_practice_script = false;  // tests: SC5_TEST_PRACTICE_SCRIPT plays

uint64_t Now() { return frame::Count(); }
uint64_t In() { return Now() - g_since; }

void Enter(Phase p) {
  // a plain game never keeps the player's controller from them: lost, the autopilot stops
  if (p == Phase::Lost && !g_launcher) {
    REXLOG_WARN("sc5 autopilot: lost on the way (sequence {}): the game is the player's from here",
                game::SequenceName(game::SequenceId()));
    p = Phase::Off;
  }
  if (p != g_phase)
    REXLOG_INFO("sc5 autopilot: {} -> {} at frame {} (sequence {}, battle {:X})", PhaseName(g_phase), PhaseName(p), Now(),
                game::SequenceName(game::SequenceId()), game::BattleState());
  g_phase = p;
  g_since = Now();
  g_steps.clear();
  g_step_t = 0;
  g_menu_pressed = false;
}

void Push(uint16_t buttons, int hold, int after) { g_steps.push_back({buttons, hold, after}); }

// the queued presses, one frame of them: true while any are left
bool RunSteps() {
  while (!g_steps.empty()) {
    const Press& p = g_steps.front();
    const int t = g_step_t++;
    if (t < p.hold) {
      g_pad.buttons = p.buttons;
      return true;
    }
    if (t < p.hold + p.after) return true;
    g_steps.pop_front();
    g_step_t = 0;
  }
  return false;
}

// one press, when none is going on
void Tap(uint16_t buttons) {
  if (g_steps.empty()) Push(buttons, 4, 8);
}

bool InHome(int seq) { return seq == game::kSeqAstralHome || seq == 18; }

// Tests: SC5_TEST_HOLD_SCRIPT=<press>,... is pressed from the hold point on, once: a press is a button name (UP DOWN
// LEFT RIGHT A B X Y START BACK LB RB, or WAIT), *n for n times, /f for the frames after it (default 30).
bool PushScript(const char* text) {
  if (!text || !*text) return false;
  struct Name {
    const char* name;
    uint16_t bits;
  };
  static const Name kNames[] = {{"UP", input::kDpadUp},       {"DOWN", input::kDpadDown}, {"LEFT", input::kDpadLeft},
                                {"RIGHT", input::kDpadRight}, {"A", input::kA},           {"B", input::kB},
                                {"X", input::kX},             {"Y", input::kY},           {"START", input::kStart},
                                {"BACK", input::kBack},       {"LB", input::kLeftShoulder},
                                {"RB", input::kRightShoulder}, {"WAIT", 0}};
  for (const char* p = text; *p;) {
    const char* end = std::strchr(p, ',');
    const std::string tok(p, end ? size_t(end - p) : std::strlen(p));
    p = end ? end + 1 : p + tok.size();
    std::string name = tok.substr(0, tok.find_first_of("*/"));
    int times = 1, after = 30;
    if (size_t star = tok.find('*'); star != std::string::npos) times = std::atoi(tok.c_str() + star + 1);
    if (size_t slash = tok.find('/'); slash != std::string::npos) after = std::atoi(tok.c_str() + slash + 1);
    for (const Name& n : kNames)
      if (name == n.name)
        for (int i = 0; i < times; i++) Push(n.bits, n.bits ? 6 : 0, after);
  }
  return true;
}

// The main menu's state: its object's menu (at +8, vtable 0x820BF434) keeps the depth at +0x88 (0 the top level, 1
// Offline Play's submenu), the top level's cursor at +0x94 (Offline Play 0, Story 2) and the submenu's at +0x98 (Quick
// Battle 2, VS Battle 3); UP lowers a cursor, DOWN raises it. Back from VS Battle the game opens the submenu again on
// VS Battle.
struct Home {
  bool ok = false;
  int depth = 0, top = 0, sub = 0;
};
Home ReadHome() {
  Home h;
  const uint32_t home = game::HomeObject();
  const uint32_t menu = home ? game::ReadU32(home + 8) : 0;
  if (!menu || game::ReadU32(menu) != 0x820BF434u) return h;
  h.ok = true;
  h.depth = int(game::ReadU32(menu + 0x88));
  h.top = int(game::ReadU32(menu + 0x94));
  h.sub = int(game::ReadU32(menu + 0x98));
  return h;
}
constexpr int kHomeOffline = 0, kOfflineVersus = 3, kOfflineTraining = 4;

// the same, logged
std::string HomeState() {
  const uint32_t home = game::HomeObject();
  const uint32_t menu = home ? game::ReadU32(home + 8) : 0;
  if (!menu) return "no menu";
  std::string out = fmt::format("menu {:08X}:", menu);
  for (uint32_t o = 0x80; o < 0xC0; o += 4) out += fmt::format(" {:X}", game::ReadU32(menu + o));
  const uint32_t sub = game::ReadU32(menu + 0xC0);
  if (sub) {
    out += fmt::format(" | {:08X}:", sub);
    for (uint32_t o = 0x30; o < 0x50; o += 4) out += fmt::format(" {:X}", game::ReadU32(sub + o));
  }
  return out;
}

// the rounds each side won, from the players' objects
void RoundsWon(int out[2]) {
  for (int s = 0; s < 2; s++) out[s] = int(game::ReadU32(kPlayer[s] + kRoundsWon));
}

// --- the select screen ---

// a press that moves a cursor toward a slot (the grid wraps both ways; the crest of slots 19-20 is skipped by the
// game's own moves)
uint16_t Toward(select::Cursor c, int slot) {
  const int tx = slot % select::kColumns, ty = slot / select::kColumns;
  int dx = tx - c.x;
  if (dx > select::kColumns / 2) dx -= select::kColumns;
  if (dx < -select::kColumns / 2) dx += select::kColumns;
  if (dx) return dx > 0 ? input::kDpadRight : input::kDpadLeft;
  int dy = ty - c.y;
  if (dy > select::kRows / 2) dy -= select::kRows;
  if (dy < -select::kRows / 2) dy += select::kRows;
  if (dy) return dy > 0 ? input::kDpadDown : input::kDpadUp;
  return 0;
}

// what a side is to be: the slot and costume, and whether to pick it now (false: only follow the cursor)
struct Target {
  bool known = false, pick = false;
  int slot = -1, costume = 0;
  bool costume_known = false;
};

Target TargetOf(int side) {
  Target t;
  if (g_plan.slots[side] >= 0) {
    t.known = t.pick = t.costume_known = true;
    t.slot = g_plan.slots[side];
    t.costume = g_plan.costumes[side] >= 0 ? g_plan.costumes[side] : 0;
    return t;
  }
  if (!g_plan.online) return t;
  const net::Pick p = net::RemotePick(side);
  if (p.phase < 0 || p.slot < 0) return t;
  t.known = true;
  t.slot = p.slot;
  t.pick = p.phase >= 1;
  t.costume_known = p.phase >= 2;
  t.costume = p.costume >= 0 ? p.costume : 0;
  return t;
}

bool LocalPicks(int side) { return g_plan.online && g_plan.slots[side] < 0 && side == g_plan.side; }

void OnSelect() {
  if (!select::Object()) {
    // the fight loads (both picked, the stage too) or the screen went
    if (game::BattleState() == game::kBattleLoading || game::BattleState() == game::kBattleIntro) Enter(Phase::Fight);
    else if (In() > 600) Enter(Phase::Lost);
    return;
  }
  const int step[2] = {select::Step(0), select::Step(1)};
  for (int s = 0; s < 2; s++) {
    // what a side picked, as it is picked (the cursor stays on it; the costume as its panel showed it)
    if (g_last_step[s] == 0 && step[s] >= 1) {
      g_picked_slot[s] = select::CursorOf(s).Slot();
      REXLOG_INFO("sc5 autopilot: side {} picked {} (slot {}) at frame {}", s + 1, select::NameOfSlot(g_picked_slot[s]),
                  g_picked_slot[s], Now());
    }
    if (g_last_step[s] == 1 && step[s] >= 2) {
      g_picked_costume[s] = std::max(0, select::Costume(s));
      REXLOG_INFO("sc5 autopilot: side {} costume {} at frame {}", s + 1, g_picked_costume[s] + 1, Now());
    }
    g_last_step[s] = step[s];
  }
  // this PC's player's pick, for the other game
  if (g_plan.online) {
    const int me = g_plan.side;
    net::Pick p;
    if (LocalPicks(me)) {
      p.phase = step[me] >= 2 ? 2 : step[me] >= 1 ? 1 : 0;
      p.slot = step[me] >= 1 ? g_picked_slot[me] : select::CursorOf(me).Slot();
      p.costume = step[me] >= 2 ? g_picked_costume[me] : step[me] >= 1 ? select::Costume(me) : -1;
    }
    net::SetLocalPick(me, p, select::PickableMask());
  }
  if (select::Age() < 80) return;  // the screen comes in
  const int side = step[0] < 2 ? 0 : step[1] < 2 ? 1 : -1;
  if (side < 0) {
    Enter(Phase::Stage);
    return;
  }
  // tests: SC5_TEST_LOCAL_PICK=<slot>[.<costume>] stands in for this PC's player picking on the select screen
  static const int test_pick = [] { const char* v = std::getenv("SC5_TEST_LOCAL_PICK"); return v ? std::atoi(v) : -1; }();
  static const int test_costume = [] {
    const char* v = std::getenv("SC5_TEST_LOCAL_PICK");
    const char* dot = v ? std::strchr(v, '.') : nullptr;
    return dot ? std::atoi(dot + 1) : 0;
  }();
  if (LocalPicks(side) && test_pick >= 0) {
    if (!g_steps.empty()) {
      RunSteps();
      return;
    }
    if (In() % 30 != 0) return;  // a player's pace
    if (step[side] == 0) {
      const uint16_t move = Toward(select::CursorOf(side), test_pick);
      Tap(move ? move : uint16_t(input::kA));
    } else if (step[side] == 1 && select::Costume(side) >= 0) {
      const int now = select::Costume(side);
      Tap(now < test_costume ? input::kDpadRight : now > test_costume ? input::kDpadLeft : input::kA);
    }
    RunSteps();
    return;
  }
  if (LocalPicks(side)) {
    // the player's own controller: moves, A on what both games can load; the costume panel's LEFT, RIGHT, A
    g_local = true;
    g_allow = input::kDpadUp | input::kDpadDown | input::kDpadLeft | input::kDpadRight;
    if (step[side] == 0) {
      const int slot = select::CursorOf(side).Slot();
      const uint32_t theirs = net::RemotePickable();
      // the paid character only when the other game has him too
      if (select::Pickable(slot) && (slot != 21 || (theirs >> 21 & 1))) g_allow |= input::kA;
    } else {
      g_allow |= input::kA;
    }
    return;
  }
  const Target t = TargetOf(side);
  if (!t.known || !g_steps.empty()) {
    RunSteps();
    return;
  }
  if (step[side] == 0) {
    const uint16_t move = Toward(select::CursorOf(side), t.slot);
    if (move) Tap(move);
    else if (t.pick && select::Pickable(t.slot)) Tap(input::kA);
    else if (t.pick && In() % 600 == 0)
      REXLOG_WARN("sc5 autopilot: side {}'s pick {} cannot be picked here", side + 1, select::NameOfSlot(t.slot));
  } else if (step[side] == 1 && t.costume_known) {
    // the costume panel: toward the costume, A on it. In a mirror match the second side cannot have the first side's
    // costume (the panel shows it greyed and refuses A): the next one then. Should the panel still not go where it is
    // sent, or A not take, three presses that change nothing give up: the next costume, or A on what it shows.
    static int last_seen = -1, stuck = 0;
    const int now = select::Costume(side), count = select::Costumes(side);
    if (now < 0 || count <= 0) return;  // the panel comes in
    int want = t.costume % count;
    const int other = 1 - side;
    if (g_picked_slot[other] == g_picked_slot[side] && g_picked_costume[other] == want && step[other] >= 2) {
      want = (want + 1) % count;
    }
    stuck = now == last_seen ? stuck + 1 : 0;
    last_seen = now;
    if (stuck >= 6) {  // nothing took: the next costume
      REXLOG_WARN("sc5 autopilot: side {}'s costume panel took nothing on costume {}: the next", side + 1, now + 1);
      Tap(input::kDpadRight);
      stuck = 0;
    } else if (now != want) {
      Tap(now < want ? input::kDpadRight : input::kDpadLeft);
    } else {
      Tap(input::kA);
    }
  }
  RunSteps();
}

void NewMatch() {
  g_picked_slot[0] = g_picked_slot[1] = -1;
  g_picked_costume[0] = g_picked_costume[1] = -1;
  g_last_step[0] = g_last_step[1] = 0;
  g_fought = g_over = g_over_reported = false;
  g_won[0] = g_won[1] = 0;
}

// the first pad while the autopilot holds the menus; the netplay's inputs once a match's frames run in sync
bool Override(uint32_t user, input::Pad& out) {
  if (g_phase == Phase::Off) return false;
  if (net::InSync()) return false;
  // practice: the player's own controllers once it runs (but while a test's script plays)
  if (g_plan.mode == Mode::Practice && g_phase == Phase::Fight && !g_practice_script) return false;
  out = input::Pad{};
  out.connected = true;  // the launcher's game has both pads (docs\PROTOCOL.md 3.1)
  if (user == 1 && g_both) out.buttons = g_pad.buttons;  // skipping the way into a fight: each player's own part
  if (user != 0) return user < 2;
  if (g_local) {
    input::Pad own = input::ReadPhysical(0);
    out.buttons = own.buttons & g_allow;
    out.lx = own.lx;
    out.ly = own.ly;
    return true;
  }
  out.buttons = g_pad.buttons;
  return true;
}

}  // namespace

void Init(bool launcher) {
  g_launcher = launcher;
  if (!launcher) return;
  Enter(Phase::Boot);
  input::SetOverride(&Override);
}

void InitPlain(const Plan& plan) {
  g_launcher = false;
  g_plan = plan;
  g_plan.hand_over = true;
  NewMatch();
  // The first-visit window comes 15 s into the main menu with a save that has never been there: with the player's
  // own save, which has, the way to the plan's screen starts at once
  const char* user = std::getenv("SC5_USER_DIR");
  bool fresh = true;
  if (user && *user) {
    std::error_code ec;
    for (const auto& profile : std::filesystem::directory_iterator(user, ec))
      if (std::filesystem::exists(profile.path() / "4E4D083D" / "00000001" / "SC5SaveData" / "savedata.bin", ec))
        fresh = false;
  }
  g_fresh_save = fresh;
  REXLOG_INFO("sc5 autopilot: a plain game for {}: to its select screen ({} save)",
              plan.mode == Mode::Practice ? "practice" : "the offline versus", fresh ? "a fresh" : "the player's");
  Enter(Phase::Boot);
  input::SetOverride(&Override);
  frame::SetMenuHook(&OnFrame);
}

bool Practicing() { return g_plan.mode == Mode::Practice && g_phase == Phase::Fight && g_fought; }

bool AtHoldPoint() { return g_phase == Phase::Hold; }
bool Busy() { return g_phase != Phase::Off && g_phase != Phase::Hold && g_phase != Phase::Lost; }
bool Lost() { return g_phase == Phase::Lost; }
int PickedSlot(int side) { return side == 0 || side == 1 ? g_picked_slot[side] : -1; }
int PickedCostume(int side) { return side == 0 || side == 1 ? g_picked_costume[side] : -1; }

void Begin(const Plan& plan) {
  g_plan = plan;
  NewMatch();
  Enter(Phase::ToVersus);
  REXLOG_INFO("sc5 autopilot: to a VS fight, {}; picks {} {}", plan.online ? fmt::format("online as player {}", plan.side + 1) : "offline",
              plan.slots[0] >= 0 ? select::NameOfSlot(plan.slots[0]) : "-",
              plan.slots[1] >= 0 ? select::NameOfSlot(plan.slots[1]) : "-");
}

void Park() {
  switch (g_phase) {
    case Phase::Hold:
    case Phase::Leave:
    case Phase::Back:
      return;
    case Phase::Fight:
    case Phase::Result:
    case Phase::Again:
      Enter(Phase::Leave);  // the fight's pause menu, or the result's
      return;
    case Phase::ToVersus:
    case Phase::Home:
      // still on the main menu: that is the hold point (the way to VS Battle goes by the menu's cursors)
      Enter(InHome(game::SequenceId()) ? Phase::Hold : Phase::Back);
      return;
    case Phase::Settings:
    case Phase::Select:
    case Phase::Stage:
      Enter(Phase::Back);  // B until the main menu
      return;
    default:
      Enter(Phase::Lost);
  }
}

void Rematch(const Plan& plan, const bool named[2], bool same_stage) {
  // Always through Character Select, the same fighters picked again by the autopilot on both PCs unless the launcher
  // names others (or `-`: that side's player picks again): the fight loads again and the games meet at its intro. (VS
  // Battle's own Rematch keeps the fight loaded and goes straight to "between rounds", whose state counts on from the
  // result screens, which took a different time on each PC: the two games left it on different frames.)
  g_reselect = true;
  const uint64_t over_at = g_over_at;
  const int slots[2] = {g_picked_slot[0], g_picked_slot[1]}, costumes[2] = {g_picked_costume[0], g_picked_costume[1]};
  Plan next = plan;
  for (int s = 0; s < 2; s++) {
    if (!named[s] && slots[s] >= 0) {
      next.slots[s] = slots[s];
      next.costumes[s] = costumes[s];
    }
  }
  g_plan = next;
  g_same_stage = same_stage && g_last_rights > 0;
  NewMatch();
  g_over = true;  // still at the result
  g_over_reported = true;
  g_over_at = over_at;
  Enter(Phase::Again);
}

bool MatchOver(int& winner, int& p1, int& p2, std::string& c1, std::string& c2) {
  if (!g_over || g_over_reported) return false;
  g_over_reported = true;
  p1 = g_won[0];
  p2 = g_won[1];
  winner = p1 > p2 ? 1 : p2 > p1 ? 2 : 0;  // 0: a draw (the last round timed out even, or both fell together)
  for (int s = 0; s < 2; s++) {
    const int character = select::CharacterOfSlot(g_picked_slot[s]);
    std::string& out = s ? c2 : c1;
    out = character >= 0 ? fmt::format("{}.{}", character, std::max(0, g_picked_costume[s])) : "-";
  }
  return true;
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (g_phase == Phase::Off) return;
  const int seq = game::SequenceId();
  const uint32_t battle = game::BattleState();
  if (seq != g_last_seq) {
    REXLOG_INFO("sc5 autopilot: sequence {} at frame {} ({})", game::SequenceName(seq), Now(), PhaseName(g_phase));
    g_last_seq = seq;
  }
  if (battle != g_last_battle) {
    // the match is over once a KO goes on to neither the next round nor its replay
    if (g_last_battle == game::kBattleKO && battle != game::kBattleFighting && battle != game::kBattleBetweenRounds &&
        (g_phase == Phase::Fight || g_phase == Phase::Leave) && g_fought && !g_over) {
      RoundsWon(g_won);
      g_over = true;
      g_over_at = Now();
      REXLOG_INFO("sc5 autopilot: the match is over at frame {}: {} - {} (battle state {:X})", Now(), g_won[0], g_won[1],
                  battle);
    }
    if (battle == game::kBattleFighting) g_fought = true;
    g_last_battle = battle;
  }
  g_pad = input::Pad{};
  g_local = false;
  g_allow = 0;
  g_both = false;
  switch (g_phase) {
    case Phase::Off:
    case Phase::Lost:
      return;
    case Phase::Boot:
      if (InHome(seq)) {
        Enter(Phase::Home);
        return;
      }
      // the logos and the opening movie: START skips; the title: START once it takes it
      if ((seq == game::kSeqLogo || seq == game::kSeqAttract) && In() % 60 == 30) Tap(input::kStart);
      if (seq == game::kSeqTitle && In() % 90 == 45) Tap(input::kStart);
      RunSteps();
      return;
    case Phase::Home:
      if (!InHome(seq)) {
        Enter(Phase::Boot);
        return;
      }
      // a plain game with the player's own save: no first-visit window, on to the plan's screen once the menu takes
      // presses
      if (!g_launcher && !g_fresh_save) {
        if (In() > 120) Enter(Phase::ToVersus);
        return;
      }
      // the first-visit window: A once it is up and takes it (15 s in), then the hold point (a plain game: on)
      if (In() == 900) Push(input::kA, 10, 120);
      if (!RunSteps() && In() > 900) {
        if (!g_launcher) {
          Enter(Phase::ToVersus);
          return;
        }
        Enter(Phase::Hold);
        REXLOG_INFO("sc5 autopilot: at the hold point (the main menu) at frame {}; {}", Now(), HomeState());
      }
      return;
    case Phase::Hold: {
      static bool scripted = false;
      if (!scripted) {
        scripted = true;
        if (const char* script = std::getenv("SC5_TEST_HOLD_SCRIPT")) {
          Enter(Phase::Script);
          PushScript(script);
          return;
        }
      }
      if (!InHome(seq)) {
        REXLOG_WARN("sc5 autopilot: the game left the main menu (sequence {}) at frame {}: back to it",
                    game::SequenceName(seq), Now());
        Enter(Phase::Boot);
      }
      return;
    }
    case Phase::Script: {
      // tests: the script's presses, the screens they reach logged
      static uint32_t last_select = 0;
      if (select::Object() != last_select) {
        last_select = select::Object();
        REXLOG_INFO("sc5 autopilot: script: select screen {:08X} at frame {}", last_select, Now());
      }
      const size_t left = g_steps.size();
      RunSteps();
      if (g_steps.size() != left)
        REXLOG_INFO("sc5 autopilot: script: {} presses left at frame {} (sequence {}, battle {:X}; {})", g_steps.size(),
                    Now(), game::SequenceName(seq), battle, HomeState());
      return;
    }
    case Phase::ToVersus: {
      // the main menu to Offline Play, A; its submenu to VS Battle (or Training for practice), A (one press at a time,
      // by the menu's cursors)
      if (RunSteps()) return;
      if (seq == game::kSeqVersus) {
        Enter(Phase::Settings);
        return;
      }
      if (seq == game::kSeqTraining) {  // Training opens on its select screen
        Enter(Phase::Select);
        return;
      }
      if (In() > 900) {
        REXLOG_WARN("sc5 autopilot: {} did not come (sequence {}; {})",
                    g_plan.mode == Mode::Practice ? "Training" : "VS Battle", game::SequenceName(seq), HomeState());
        Enter(Phase::Lost);
        return;
      }
      const Home h = ReadHome();
      if (!InHome(seq) || !h.ok) return;
      const int target = g_plan.mode == Mode::Practice ? kOfflineTraining : kOfflineVersus;
      if (h.depth == 0) {
        if (h.top != kHomeOffline) Push(h.top > kHomeOffline ? input::kDpadUp : input::kDpadDown, 6, 30);
        else Push(input::kA, 8, 90);
      } else if (h.depth == 1) {
        if (h.sub != target) Push(h.sub > target ? input::kDpadUp : input::kDpadDown, 6, 30);
        else Push(input::kA, 8, 120);
      } else {
        Push(input::kB, 6, 60);  // deeper than the autopilot knows: back out
      }
      RunSteps();
      return;
    }
    case Phase::Settings:
      // VS Battle's settings (Start Match, Player, Battle Count, Round Time, CPU Strength, Default), on Start Match: A
      // once it takes presses. The offline versus first sets Player to "Player VS Player" (Player VS CPU, CPU VS
      // Player, CPU VS CPU, Player VS Player: three to the right; it is there only with a second user signed in).
      if (select::Object()) {
        Enter(Phase::Select);
        return;
      }
      if (In() == 240) {
        if (g_plan.two_players) {
          Push(input::kDpadDown, 6, 40);
          for (int i = 0; i < 3; i++) Push(input::kDpadRight, 6, 40);
          Push(input::kDpadUp, 6, 40);
        }
        Push(input::kA, 10, 30);
      }
      if (In() == 900) Push(input::kA, 10, 30);
      if (In() > 1200) Enter(Phase::Lost);
      RunSteps();
      return;
    case Phase::Select:
      if (g_plan.hand_over) {
        // a plain game: the players pick on their own from here
        if (select::Object() && select::Age() > 30) {
          REXLOG_INFO("sc5 autopilot: the select screen is the players' now (frame {})", Now());
          Enter(Phase::Off);
        } else if (In() > 1200) {
          Enter(Phase::Lost);
        }
        return;
      }
      OnSelect();
      return;
    case Phase::Stage:
      // The same stage on both PCs, picked alike: from Random (the cursor's first place) RIGHT as often as the match's
      // seed says, A (Random would draw on each PC: the stage, and with it where the fighters start). Then A again
      // until the fight loads (should the first come before the screen takes it).
      if (battle == game::kBattleLoading || battle == game::kBattleIntro || battle == game::kBattleRoundCall ||
          battle == game::kBattleFighting) {  // (Training goes from its stage select to the round's call)
        Enter(Phase::Fight);
        return;
      }
      // The same stage on both PCs, picked alike once the screen surely takes presses (5 s in): from Random (the
      // cursor's first place) RIGHT as often as the match's seed says, a press every half second, then A; A again
      // every 1.5 s should it not have taken. (Random would draw on each PC; the stage select sets more up with its
      // choice than the stage the battle setup copies, so player 2's game taking player 1's stage there is not enough.)
      if (In() == 300) {
        // a rematch with the same two fighters: the last match's stage, as the game's own Rematch keeps it
        const int64_t seed = game::FightSeed();
        const bool same = g_same_stage;
        const int rights = same ? g_last_rights : 1 + int((seed >= 0 ? uint64_t(seed) : 0) % 8);
        g_last_rights = rights;
        g_same_stage = false;
        for (int i = 0; i < rights; i++) Push(input::kDpadRight, 6, 24);
        Push(input::kA, 10, 80);
        REXLOG_INFO("sc5 autopilot: the stage {} to the right of Random{}", rights, same ? " (the last match's)" : "");
      }
      if (In() > 700 && In() % 90 == 10) Tap(input::kA);
      if (In() > 2400) Enter(Phase::Lost);
      (void)ctx;
      (void)base;
      RunSteps();
      return;
    case Phase::Fight:
      if (g_plan.mode == Mode::Practice) {
        // tests: SC5_TEST_PRACTICE_SCRIPT=<presses> (as SC5_TEST_HOLD_SCRIPT) once practice runs
        static bool scripted = false;
        if (!scripted && g_fought) {
          scripted = true;
          if (PushScript(std::getenv("SC5_TEST_PRACTICE_SCRIPT"))) g_practice_script = true;
        }
        if (g_practice_script) {
          const size_t left = g_steps.size();
          g_practice_script = RunSteps();
          if (g_steps.size() != left)
            REXLOG_INFO("sc5 autopilot: practice script: {} presses left at frame {} (battle {:X})", g_steps.size(),
                        Now(), battle);
        }
        // practice goes on until park, or until the player leaves it for the main menu in the game
        if (InHome(seq)) {
          REXLOG_INFO("sc5 autopilot: the player left practice at frame {}", Now());
          Enter(Phase::Hold);
        }
        return;
      }
      // the way into an online fight: the VS screen (both fighters on their cards) while the fight loads, which START
      // ends as on the console once the fight has loaded (A, B, X, Y do nothing there), then the stage's fly-over and
      // the fighters' intros, which START skips too: START on both pads every third of a second while they are up
      // (2026-11-17: the VS screen 359 frames to 101, the intros 484 to 201, about 9 s less). The games meet at round
      // 1's call whatever time their way there took (the netplay's barrier). (In the one launcher test with the intros
      // skipped the two games took a KO's replay differently: the replay's own frame counter, which rollback did not
      // put back, battle.cpp; it parted the games with the intros played as well.)
      if (g_plan.online && !g_fought && (battle == game::kBattleLoading || battle == game::kBattleIntro)) {
        if (g_skip_logged != battle) {
          g_skip_logged = battle;
          REXLOG_INFO("sc5 autopilot: skipping the {} with START from frame {}",
                      battle == game::kBattleLoading ? "VS screen" : "fly-over and intros", Now());
        }
        if (In() % 20 == 0) Tap(input::kStart);
        RunSteps();
        g_both = true;
      } else if (g_plan.online && !g_fought && !g_steps.empty()) {
        g_steps.clear();  // a press still queued when round 1's call came: none for the result screen's menus
        g_step_t = 0;
      }
      if (g_over) Enter(Phase::Result);
      return;
    case Phase::Result:
      return;
    case Phase::Back:
      // B, a press a second, until the main menu
      if (InHome(seq)) {
        Enter(Phase::Hold);
        REXLOG_INFO("sc5 autopilot: back at the hold point at frame {}; {}", Now(), HomeState());
        return;
      }
      if (g_steps.empty() && In() % 60 == 20) Push(input::kB, 6, 30);
      if (In() > 1800) Enter(Phase::Lost);
      RunSteps();
      return;
    case Phase::Leave:
    case Phase::Again: {
      // the winner's pose, then the Results screen takes Next (A), then its menu (Rematch, Character Select, Return to
      // Astral Home), the cursor on Rematch
      const bool leave = g_phase == Phase::Leave;
      if (!g_menu_pressed && leave && g_plan.mode == Mode::Practice) {
        // Training: its first-visit help windows (a fresh save: "Training", then the control pages) close on A (A
        // only attacks once they are gone); its pause menu (Training Options: Return to Game ... Character Select,
        // Return to Astral Home) wraps, so UP from the top is the last; it asks again (Yes / No, on No)
        for (int i = 0; i < 5; i++) Push(input::kA, 8, 100);
        Push(input::kStart, 8, 90);
        Push(input::kDpadUp, 6, 40);
        Push(input::kA, 8, 90);
        Push(input::kDpadUp, 6, 40);
        Push(input::kA, 8, 60);
        g_menu_pressed = true;
        REXLOG_INFO("sc5 autopilot: leaving practice through Training's pause menu at frame {}", Now());
      }
      if (!g_menu_pressed && leave && !g_over && battle == game::kBattleFighting) {
        // a fight still on: its pause menu (Return to Game, Move List, Options, Character Select, Return to Astral
        // Home), which asks again (Yes / No, on No)
        Push(input::kStart, 8, 60);
        for (int i = 0; i < 4; i++) Push(input::kDpadDown, 6, 30);
        Push(input::kA, 8, 90);
        Push(input::kDpadUp, 6, 40);
        Push(input::kA, 8, 60);
        g_menu_pressed = true;
        REXLOG_INFO("sc5 autopilot: leaving the fight through its pause menu at frame {}", Now());
      }
      if (!g_menu_pressed) {
        if (!g_over || Now() < g_over_at + 720) return;  // the result screen first (or a fight's KO, intro)
        Push(input::kA, 10, 150);
        if (leave) {
          Push(input::kDpadDown, 8, 40);
          Push(input::kDpadDown, 8, 40);
        } else if (g_reselect) {
          Push(input::kDpadDown, 8, 40);
        }
        Push(input::kA, 10, 90);
        // Return to Astral Home asks again, the cursor on No: UP (Yes), A
        if (leave) {
          Push(input::kDpadUp, 8, 40);
          Push(input::kA, 10, 60);
        }
        g_menu_pressed = true;
      }
      if (RunSteps()) {
        g_since = Now();  // the time to get there counts from the last press
        return;
      }
      if (leave && InHome(seq)) {
        Enter(Phase::Hold);
        REXLOG_INFO("sc5 autopilot: back at the hold point at frame {}; {}", Now(), HomeState());
      } else if (!leave && g_reselect && select::Object()) {
        g_last_step[0] = g_last_step[1] = 0;
        g_over = g_over_reported = false;  // a new match from here
        g_fought = false;
        Enter(Phase::Select);
      } else if (!leave && !g_reselect &&
                 (battle == game::kBattleLoading || battle == game::kBattleIntro ||
                  battle == game::kBattleBetweenRounds || battle == game::kBattleRoundCall)) {
        // Rematch keeps the fight loaded: from the result straight to "between rounds"
        g_over = g_over_reported = false;
        g_fought = false;
        Enter(Phase::Fight);
      } else if (g_menu_pressed && In() > 900) {
        REXLOG_WARN("sc5 autopilot: {} did not come (sequence {}, battle {:X})",
                    leave ? "the main menu" : g_reselect ? "the select screen" : "the rematch's fight",
                    game::SequenceName(seq), battle);
        Enter(Phase::Lost);
      }
      return;
    }
  }
}

}  // namespace sc5::autopilot
