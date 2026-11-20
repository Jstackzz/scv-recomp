// The launcher's autopilot, see autopilot.h.
//
// Where the game is comes from its sequence (game::SequenceId: Title, AstralHome the main menu, VSStandard VS
// Battle), the battle's state machine (game::BattleState) and the select screen (sc5/select.h). The menus, with the
// fresh save every online session has (docs\GAME.md, "Sequences"):
//   Title: START. AstralHome: a first-visit window that A closes, the cursor on Story: the hold point.
//   From there, by the main menu's cursors (ReadHome): Offline Play, A; VS Battle, A: VSStandard, its settings menu on
//   Start Match: A. The select screen: player 1's side picks a character (A) and a costume (A), then player 2's side,
//   both with the first pad ("Player VS CPU": the battle setup makes player 2 the second pad's, sc5/versus.cpp). Then
//   the stage select (online one player picks the stage on their own PC and the other game follows, OnStageOnline;
//   offline RIGHT from Random as the seed says, A), loading, the fight.
//   After the last round: the winner's pose, the Results screen (A: Next), then VS Battle's menu (Rematch: the same
//   fight again, online when both players chose REMATCH; Character Select; Return to Astral Home). A fight's pause
//   menu (START) leaves it for park.
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

#include "sc5/creation.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/input.h"
#include "sc5/net/netplay.h"
#include "sc5/select.h"
#include "sc5/testinput.h"

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
// In each player's object, as the match's last round leaves them once the match is over (its KO going on to neither its
// replay nor "between rounds"): +0x75C the rounds that side won, +0x764 the drawn ones (a round that runs out of time
// even, or a double KO); +0x760 counts the lost ones. The game gives a drawn round's point to both players and ends the
// match once a side's points reach the rounds to win, a draw when a drawn round takes both there (its Results then say
// Draw for both players). +0x788 counts those points too, but a KO that ends the match plays its replay first and by
// the end only +0x75C has that round (a best of 1 won by a KO read 0 - 0 there), so the points are the won rounds and
// the drawn ones (docs\GAME.md, "The result": KOs ending a best of 1, 3 and 5, a time-out won on health, a final drawn
// round and a double KO checked, 2026-11-19).
constexpr uint32_t kRoundsWon = 0x75C, kRoundsDrawn = 0x764;

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
bool g_still = false;  // and not their stick
int g_picked_slot[2] = {-1, -1}, g_picked_costume[2] = {-1, -1};
int g_picked_costumes[2] = {0, 0};  // the costume panel's entries when the side picked
uint32_t g_picked_created[2] = {0, 0};  // the side's pick a created character: its checksum (0 not)
bool g_created_placed[2] = {false, false};  // the other PC's created character for that side is in its slot
PPCContext* g_ctx = nullptr;  // this frame's (OnFrame): the game's functions called from the autopilot
uint8_t* g_base = nullptr;
uint64_t g_created_wait[2] = {0, 0};        // the frame the side's created pick was first seen (the record's wait)
int g_last_step[2] = {0, 0};
int g_last_seq = -1;
uint32_t g_last_battle = 0xFFFFFFFF;
bool g_fought = false, g_over = false, g_over_reported = false;
uint64_t g_over_at = 0;
int g_won[2] = {0, 0};
bool g_reselect = false;     // a rematch goes through Character Select (else VS Battle's own Rematch)
bool g_menu_pressed = false;  // the after-result presses went out (Leave, Again)
uint32_t g_skip_logged = 0;   // the battle state whose skipping with A the log said last
bool g_both = false;          // this frame's press goes to both pads (each player skips their own intro)
int g_last_rights = 0;        // the last match's stage: that many to the right of Random on the stage select
bool g_same_stage = false;    // a rematch on the last match's stage (`stage=keep`)
int g_stage_chooser = 0;      // online: whose player picks the coming stage (0 player 1, 1 player 2; player 1's game's)
int g_stage_auto = -1;        // the chooser's game: the place the autopilot takes the cursor to and picks (Random drawn)
int g_stage_last = -1;        // the stage select's cursor the last frame it was up
uint64_t g_stage_up_at = 0;   // the frame the stage select came up (0: not yet)
uint64_t g_stage_gone_at = 0; // the frame it went (0: up, or not yet)
int g_stage_taken = -1;       // the place the stage select's pick took (the VS settings went into the battle setup)
uint64_t g_stage_a_at = 0;    // the frame the chooser's own A went through on a stage (their moves wait 3 s after)
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

// each side's round points in the match, from the players' objects: the rounds it won and the drawn ones
void RoundsWon(int out[2]) {
  for (int s = 0; s < 2; s++)
    out[s] = int(game::ReadU32(kPlayer[s] + kRoundsWon)) + int(game::ReadU32(kPlayer[s] + kRoundsDrawn));
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
  bool created = false;  // a created character from the other PC (its record in RemoteSlot(side) first)
};

std::string g_lost_why;  // why the autopilot gave up, when it knows

// The other PC's created character for a side: whole, checked and in its slot (true), or still on its way (false). A
// record the check refuses, one that is not the picked fighter's, or one that does not come in 20 s ends the match
// here.
bool PlaceCreated(int side, int slot) {
  if (!g_created_wait[side]) g_created_wait[side] = Now();
  creation::Record rec;
  std::string why;
  const int got = net::RemoteCreated(side, rec, why);
  if (got == 0 && Now() <= g_created_wait[side] + 1200) return false;
  if (got == 0) why = "it did not come in 20 s";
  if (got > 0) why = creation::Check(rec);
  if (got > 0 && why.empty() && creation::TypeOf(rec.data()) != select::IdOfSlot(slot))
    why = fmt::format("its character type {} is not the picked {}", creation::TypeOf(rec.data()),
                      select::IdOfSlot(slot));
  // on top of the check: the game's own repair pass on a copy
  if (got > 0 && why.empty()) why = g_ctx ? creation::GameRepairCheck(*g_ctx, g_base, rec) : "no game frame";
  if (why.empty() && !creation::PlaceRemote(side, rec)) why = "it could not be placed";
  // the select screen is up: the creation manager's copy of that slot too (its lists read the copies)
  const uint32_t manager = select::Object() ? game::ReadU32(select::Object() + 1256) : 0;
  if (why.empty() && (!g_ctx || !creation::FillEntry(*g_ctx, g_base, manager, creation::RemoteSlot(side))))
    why = "the select screen's list of created characters is not there";
  if (!why.empty()) {
    g_lost_why = fmt::format("player {}'s created character from the other PC was refused: {}", side + 1, why);
    REXLOG_ERROR("sc5 autopilot: {}", g_lost_why);
    Enter(Phase::Lost);
    return false;
  }
  g_created_placed[side] = true;
  return true;
}

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
  // the character once the costume is known too: a created character's record must be in its slot before the screen
  // lists the costumes (it lists them as A takes the character)
  t.pick = p.phase >= 2;
  t.costume_known = p.phase >= 2;
  t.costume = p.costume >= 0 ? p.costume : 0;
  t.created = p.phase >= 2 && p.costume == -2;
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
  // this PC's own tables for created characters (once): its player's characters that fail the check out, its
  // downloadable parts' fingerprint to the other game; the creation manager's copies of the remote slots: none from an
  // earlier match
  const uint32_t manager = game::ReadU32(select::Object() + 1256);
  if (g_ctx && creation::ReadKnown(*g_ctx, g_base, select::Object())) {
    creation::DropRefused(*g_ctx, g_base, manager);
    net::SetLocalDlc(creation::DlcFingerprint());
  }
  creation::SyncEntries(manager);
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
      g_picked_costumes[s] = select::Costumes(s);
      // a created character (the player's own, or the other PC's in its slot): which one, by its checksum
      const int n = creation::SlotOfPick(select::IdOfSlot(g_picked_slot[s]), g_picked_costume[s], g_picked_costumes[s]);
      creation::Record rec;
      g_picked_created[s] = n >= 0 && creation::ReadRecord(uint32_t(n), rec) ? creation::ChecksumOf(rec) : 0;
      REXLOG_INFO("sc5 autopilot: side {} costume {} at frame {}{}", s + 1, g_picked_costume[s] + 1, Now(),
                  g_picked_created[s] ? fmt::format(" (created character {:08X})", g_picked_created[s]) : "");
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
      if (step[me] >= 2) {
        // a created character: its record to the other game (creation.h), the pick marked (costume -2)
        const int n = creation::SlotOfPick(select::IdOfSlot(g_picked_slot[me]), g_picked_costume[me],
                                           g_picked_costumes[me]);
        creation::Record rec;
        if (n >= 0 && creation::ReadRecord(uint32_t(n), rec)) {
          net::SetLocalCreated(me, rec);
          p.costume = -2;
        }
      }
    }
    net::SetLocalPick(me, p, select::PickableMask());
  }
  if (select::Age() < 80) return;  // the screen comes in
  const int side = step[0] < 2 ? 0 : step[1] < 2 ? 1 : -1;
  if (side < 0) {
    Enter(Phase::Stage);
    return;
  }
  // This PC's player's pick made by the autopilot: their created character picked again (the plan's checksum: a
  // rematch through Character Select, or the launcher's pick for a ranked winner who keeps it; the other game follows
  // it as a pick of the player's own and takes the record again), or a test's (SC5_TEST_LOCAL_PICK=<slot>[.<costume>]
  // stands in for the player)
  static const int test_pick = [] { const char* v = std::getenv("SC5_TEST_LOCAL_PICK"); return v ? std::atoi(v) : -1; }();
  static const int test_costume = [] {
    const char* v = std::getenv("SC5_TEST_LOCAL_PICK");
    const char* dot = v ? std::strchr(v, '.') : nullptr;
    return dot ? std::atoi(dot + 1) : 0;
  }();
  int auto_slot = -1, auto_costume = 0;
  if (LocalPicks(side) && g_plan.created[side]) {
    const int n = creation::SlotOfChecksum(g_plan.created[side]);
    creation::Record rec;
    if (n >= 0 && creation::ReadRecord(uint32_t(n), rec)) {
      const int type = creation::TypeOf(rec.data());
      auto_slot = type == creation::kOriginal ? select::kCreationSlot : select::SlotOfCharacter(type);
      if (step[side] == 1) {
        if (select::Costume(side) < 0 || select::Costumes(side) <= 0) return;  // the costume panel comes in
        auto_costume = creation::CostumeOfSlot(type, uint32_t(n), select::Costumes(side));
      }
    }
    if (auto_slot < 0 || auto_costume < 0) {
      REXLOG_WARN("sc5 autopilot: side {}'s created character {:08X} is not here: the player picks", side + 1,
                  g_plan.created[side]);
      g_plan.created[side] = 0;
      auto_slot = -1;
    }
  } else if (LocalPicks(side) && test_pick >= 0) {
    auto_slot = test_pick;
    auto_costume = test_costume;
  }
  if (auto_slot >= 0) {
    if (!g_steps.empty()) {
      RunSteps();
      return;
    }
    if (In() % 30 != 0) return;  // a player's pace
    if (step[side] == 0) {
      const uint16_t move = Toward(select::CursorOf(side), auto_slot);
      if (move || creation::KnownRead() || auto_slot != select::kCreationSlot) Tap(move ? move : uint16_t(input::kA));
    } else if (step[side] == 1 && select::Costume(side) >= 0) {
      const int now = select::Costume(side);
      if (now == auto_costume && g_plan.created[side])
        REXLOG_INFO("sc5 autopilot: side {} takes its created character {:08X} again (costume {})", side + 1,
                    g_plan.created[side], auto_costume + 1);
      Tap(now < auto_costume ? input::kDpadRight : now > auto_costume ? input::kDpadLeft : input::kA);
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
      // the paid character only when the other game has him too; created characters once this PC's tables are read
      if (select::Pickable(slot) && (slot != 21 || (theirs >> 21 & 1)) &&
          (slot != select::kCreationSlot || creation::KnownRead()))
        g_allow |= input::kA;
    } else if (step[side] == 1) {
      // a created character once this PC has taken out its own that fail the check, and one with downloadable
      // parts only when the other PC has the same ones
      const int n = creation::SlotOfPick(select::IdOfSlot(g_picked_slot[side]), select::Costume(side),
                                         select::Costumes(side));
      creation::Record rec;
      if (n < 0 || !creation::ReadRecord(uint32_t(n), rec) ||
          (creation::KnownRead() && (!creation::UsesDlc(rec) || net::RemoteDlc() == creation::DlcFingerprint())))
        g_allow |= input::kA;
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
    else if (t.pick && t.created && !g_created_placed[side] && !PlaceCreated(side, t.slot)) {
      // the record on its way (or refused: Lost)
    } else if (t.pick && select::Pickable(t.slot)) Tap(input::kA);
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
    if (t.created) {
      // the other PC's created character: its slot's entry in this panel
      want = creation::CostumeOfSlot(select::IdOfSlot(t.slot), creation::RemoteSlot(side), count);
      if (want < 0) {
        g_lost_why = fmt::format("player {}'s created character is not on the costume panel", side + 1);
        REXLOG_ERROR("sc5 autopilot: {}", g_lost_why);
        Enter(Phase::Lost);
        return;
      }
    } else if (g_picked_slot[other] == g_picked_slot[side] && g_picked_costume[other] == want && step[other] >= 2) {
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

// The player's own controller. Tests: their script stands in for it (SC5_INPUT_P1 in player 1's game, _P2 in player
// 2's, by frame), as the netplay takes it in a match.
input::Pad OwnPad() {
  input::Pad pad;
  if (g_plan.online && g_plan.side < 2 && testinput::ScriptedPad(uint32_t(g_plan.side), pad)) return pad;
  return input::ReadPhysical(0);
}

// --- the stage select ---

// a press that moves the stage select's cursor toward a place (it wraps both ways)
uint16_t TowardPlace(int from, int to) {
  int d = to - from;
  if (d > select::kStagePlaces / 2) d -= select::kStagePlaces;
  if (d < -select::kStagePlaces / 2) d += select::kStagePlaces;
  return d > 0 ? input::kDpadRight : d < 0 ? input::kDpadLeft : 0;
}

// Random as the match's seed draws it, the same on both PCs: one of the 27 stages (the game's own Random would draw on
// each PC: the stage, and with it where the fighters start)
int SeedPlace() {
  const int64_t seed = game::FightSeed();
  return 1 + int((seed >= 0 ? uint64_t(seed) : 0) % uint64_t(select::kStagePlaces - 1));
}

// the cursor one press at a time to a place, then (`pick`) A, again every 1.5 s should it not take; the screen takes
// presses 5 s after it came up (a press before that was lost in one game and not the other)
void StageGo(int cursor, int place, bool pick) {
  if (!g_steps.empty() || Now() < g_stage_up_at + 300) return;
  static uint64_t last_a = 0;
  if (const uint16_t move = TowardPlace(cursor, place)) {
    Tap(move);
  } else if (pick && Now() >= last_a + 90) {
    Tap(input::kA);
    last_a = Now();
  }
}

// The stage select online: the player whose turn it is picks the stage on their own PC with their own controller
// (LEFT, RIGHT and A; A on Random draws from the match's seed, SeedPlace), and the other game follows: its cursor where
// theirs is, then their stage (net::StagePick, the STAGE packets; a spectator's game from the feed). Player 1's game
// says whose turn it is (g_stage_chooser): player 1's in a match from a start line, the last match's loser's in one
// from the result screens through Character Select. A player who has not picked after a minute gets the seed's.
void OnStageOnline() {
  const int cursor = select::StageCursor();
  if (cursor >= 0) {
    if (!g_stage_up_at) g_stage_up_at = Now();
    g_stage_last = cursor;
  }
  const net::StagePick theirs = net::RemoteStage();
  const int chooser = g_plan.side == 0 ? g_stage_chooser : theirs.chooser;  // player 2's and a spectator's: theirs
  net::StagePick mine;
  mine.chooser = chooser;
  if (g_stage_taken >= 0) {  // picked (OnSetup): the chooser's game says so until the barrier, no more presses
    if (chooser == g_plan.side) net::SetLocalStage({chooser, 1, g_stage_taken, g_stage_taken});
    return;
  }
  if (chooser < 0 || cursor < 0) {  // the screen comes in, or player 1's game has not said yet
    net::SetLocalStage(mine);
    return;
  }
  static uint64_t told = 0;
  if (told != g_stage_up_at) {
    told = g_stage_up_at;
    REXLOG_INFO("sc5 autopilot: player {} picks the stage ({})", chooser + 1,
                chooser == g_plan.side ? "this PC's player" : "the other game follows");
  }
  if (chooser != g_plan.side) {
    // the other PC's player picks: their cursor, then their stage (never A on Random: each game would draw its own)
    net::SetLocalStage(mine);
    const auto in = [](int v, int lo) { return v >= lo && v < select::kStagePlaces; };
    if (theirs.chooser == chooser && theirs.phase == 1 && in(theirs.stage, 1)) {
      StageGo(cursor, theirs.stage, true);
    } else if (theirs.chooser == chooser && theirs.phase == 0 && in(theirs.cursor, 0)) {
      StageGo(cursor, theirs.cursor, false);
    }
    RunSteps();
    return;
  }
  // this PC's player picks
  mine.phase = 0;
  mine.cursor = cursor;
  // tests: SC5_TEST_STAGE=<place>|random stands in for that player: a press every half second toward the place, then
  // A (on Random as the player's A there)
  static const int test_stage = [] {
    const char* v = std::getenv("SC5_TEST_STAGE");
    return !v ? -1 : std::strcmp(v, "random") == 0 ? 0 : std::clamp(std::atoi(v), 0, select::kStagePlaces - 1);
  }();
  const input::Pad own = OwnPad();
  if (g_stage_auto < 0) {
    static bool a_was = false;
    const bool a_now = (own.buttons & input::kA) != 0;
    const bool test_random = test_stage == 0 && cursor == 0 && Now() >= g_stage_up_at + 120;
    if (test_random || (test_stage < 0 && a_now && !a_was && cursor == 0 && Now() >= g_stage_up_at + 60)) {
      g_stage_auto = SeedPlace();
      REXLOG_INFO("sc5 autopilot: the player picked Random: place {} as the match's seed draws it", g_stage_auto);
    } else if (Now() > g_stage_up_at + 3600) {
      g_stage_auto = SeedPlace();
      REXLOG_INFO("sc5 autopilot: no stage picked in a minute: place {} as the match's seed draws it", g_stage_auto);
    }
    a_was = a_now;
  }
  if (g_stage_auto >= 0) {  // decided: the autopilot takes the cursor there and picks it
    mine.phase = 1;
    mine.stage = g_stage_auto;
    net::SetLocalStage(mine);
    StageGo(cursor, g_stage_auto, true);
    RunSteps();
    return;
  }
  net::SetLocalStage(mine);
  if (test_stage >= 0) {
    if (g_steps.empty() && Now() >= g_stage_up_at + 120 && In() % 30 == 0) {
      const uint16_t move = TowardPlace(cursor, test_stage);
      if (move || test_stage != 0) Tap(move ? move : uint16_t(input::kA));
    }
    RunSteps();
    return;
  }
  // The player's own controller: LEFT, RIGHT, and A on a stage (not on Random: the seed draws that). A with a move in
  // one frame could land on Random as the cursor wraps: while A is down it goes alone. The game takes the VS settings
  // (OnSetup: the place said is the cursor then) some 100 frames after the A it takes: moves wait 3 s after an A.
  g_local = true;
  static bool a_prev = false;
  const bool a_down = (own.buttons & input::kA) != 0;
  if (a_down && !a_prev && cursor != 0) g_stage_a_at = Now();
  a_prev = a_down;
  const bool hold = a_down || (g_stage_a_at && Now() < g_stage_a_at + 180);
  g_allow = hold ? 0 : input::kDpadLeft | input::kDpadRight;
  g_still = hold;
  if (cursor != 0) g_allow |= input::kA;
}

}  // namespace

// The stage select's pick taken (versus.cpp: the VS settings go into the battle setup). The chooser's game says which
// at once: player 2's game waits for player 1's setup in this very call, and player 1's game, should it follow, sends
// its setup only once it has picked the same stage.
void OnSetup() {
  if (!g_plan.online || g_phase != Phase::Stage || g_stage_last < 0 || g_stage_taken >= 0) return;
  g_stage_taken = g_stage_last;
  if (g_same_stage) return;
  const int chooser = g_plan.side == 0 ? g_stage_chooser : net::RemoteStage().chooser;
  if (chooser == g_plan.side) net::SetLocalStage({chooser, 1, g_stage_taken, g_stage_taken});
  REXLOG_INFO("sc5 autopilot: the stage select took place {}", g_stage_taken);
}

namespace {

void NewMatch() {
  // the other PC's created characters come again for each match (and are checked again)
  creation::ClearRemote();
  g_created_placed[0] = g_created_placed[1] = false;
  g_created_wait[0] = g_created_wait[1] = 0;
  g_picked_costumes[0] = g_picked_costumes[1] = 0;
  g_lost_why.clear();
  g_stage_auto = g_stage_last = g_stage_taken = -1;
  g_stage_up_at = g_stage_gone_at = g_stage_a_at = 0;
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
    input::Pad own = OwnPad();
    out.buttons = own.buttons & g_allow;
    out.lx = g_still ? 0 : own.lx;
    out.ly = g_still ? 0 : own.ly;
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
std::string LostWhy() { return g_phase == Phase::Lost ? g_lost_why : std::string(); }
bool Busy() { return g_phase != Phase::Off && g_phase != Phase::Hold && g_phase != Phase::Lost; }
bool Lost() { return g_phase == Phase::Lost; }
int PickedSlot(int side) { return side == 0 || side == 1 ? g_picked_slot[side] : -1; }
int PickedCostume(int side) { return side == 0 || side == 1 ? g_picked_costume[side] : -1; }

void Begin(const Plan& plan) {
  g_plan = plan;
  // player 1 picks the first match's stage (tests: SC5_TEST_STAGE_CHOOSER=2 in player 1's game, player 2)
  static const bool test_p2 = [] { const char* v = std::getenv("SC5_TEST_STAGE_CHOOSER"); return v && v[0] == '2'; }();
  g_stage_chooser = test_p2 ? 1 : 0;
  // a start line's match picks its stage: a series that ended on VS Battle's own Rematch (which never comes to the stage
  // select, where the flag is cleared) left `stage=keep` set, and this game alone would take the last stage again
  g_same_stage = false;
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
  // The same two fighters on the same stage (`stage=keep`: a casual rematch both players chose): VS Battle's own
  // Rematch, as on the console: the fight stays loaded and goes from the result straight to "between rounds", and the
  // games meet again at round 1's call (net/netplay.cpp). Else through Character Select, the same fighters picked again
  // by the autopilot on both PCs unless the launcher names others (or `-`: that side's player picks again): the fight
  // loads again and the games meet at its intro.
  const uint64_t over_at = g_over_at;
  const int slots[2] = {g_picked_slot[0], g_picked_slot[1]}, costumes[2] = {g_picked_costume[0], g_picked_costume[1]};
  const uint32_t created[2] = {g_picked_created[0], g_picked_created[1]};
  Plan next = plan;
  bool same_fighters = true;
  for (int s = 0; s < 2; s++) {
    if (!named[s] && created[s]) {
      // a created character: not by its place on the costume panel (each game lists it elsewhere) but by its
      // checksum, picked by its player's own game and followed by the other
      next.slots[s] = next.costumes[s] = -1;
      next.created[s] = created[s];
    } else if (!named[s] && slots[s] >= 0) {
      next.slots[s] = slots[s];
      next.costumes[s] = costumes[s];
      next.created[s] = 0;
    }
    if (named[s] && (plan.created[s] ? plan.created[s] != created[s]
                                     : plan.slots[s] != slots[s] || created[s] ||
                                           (plan.costumes[s] >= 0 && plan.costumes[s] != costumes[s])))
      same_fighters = false;
    if (next.slots[s] < 0 && !next.created[s]) same_fighters = false;  // that side's player picks again
  }
  g_reselect = !(same_stage && same_fighters);
  // through Character Select the last match's loser picks the stage (player 1 after a draw)
  g_stage_chooser = g_won[1] > g_won[0] ? 0 : g_won[0] > g_won[1] ? 1 : 0;
  g_plan = next;
  g_same_stage = same_stage && g_last_rights > 0;
  NewMatch();
  if (!g_reselect) {  // no select screen on the way: the fighters are the last match's (the result line's)
    for (int s = 0; s < 2; s++) {
      g_picked_slot[s] = slots[s];
      g_picked_costume[s] = costumes[s];
      g_picked_created[s] = created[s];
    }
  }
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
  winner = p1 > p2 ? 1 : p2 > p1 ? 2 : 0;  // 0: a draw (a drawn round took both to the rounds to win)
  for (int s = 0; s < 2; s++) {
    // a fighter as the game's id and costume; a created character as c.<its checksum> (the same in both games)
    const int character = select::CharacterOfSlot(g_picked_slot[s]);
    std::string& out = s ? c2 : c1;
    out = g_picked_created[s] ? fmt::format("c.{:08X}", g_picked_created[s])
          : character >= 0    ? fmt::format("{}.{}", character, std::max(0, g_picked_costume[s]))
                              : "-";
  }
  return true;
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (g_phase == Phase::Off) return;
  g_ctx = &ctx;
  g_base = base;
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
      REXLOG_INFO("sc5 autopilot: the match is over at frame {}: {} - {} (won {} - {}, drawn {}; battle state {:X})", Now(),
                  g_won[0], g_won[1], game::ReadU32(kPlayer[0] + kRoundsWon), game::ReadU32(kPlayer[1] + kRoundsWon),
                  game::ReadU32(kPlayer[0] + kRoundsDrawn), battle);
    }
    if (battle == game::kBattleFighting) g_fought = true;
    g_last_battle = battle;
  }
  g_pad = input::Pad{};
  g_local = false;
  g_allow = 0;
  g_still = false;
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
      creation::OnHoldPoint();  // tests: SC5_TEST_CREATION_SLOT
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
    case Phase::Stage: {
      if (battle == game::kBattleLoading || battle == game::kBattleIntro || battle == game::kBattleRoundCall ||
          battle == game::kBattleFighting) {  // (Training goes from its stage select to the round's call)
        if (g_plan.online && g_stage_last >= 0) {
          // the stage picked: the chooser's game says which (OnSetup said so first; a later `stage=keep` picks it again)
          const int place = g_stage_taken >= 0 ? g_stage_taken : g_stage_last;
          g_last_rights = place;
          const int chooser = g_plan.side == 0 ? g_stage_chooser : net::RemoteStage().chooser;
          if (!g_same_stage && chooser == g_plan.side) net::SetLocalStage({chooser, 1, place, place});
          REXLOG_INFO("sc5 autopilot: the stage: place {} of the stage select", place);
        }
        if (g_plan.online) g_same_stage = false;
        Enter(Phase::Fight);
        return;
      }
      if (g_plan.online && g_plan.mode != Mode::Practice) {
        if (g_same_stage) {
          // the last match's stage again (`stage=keep` through Character Select): both games take the cursor there
          const int cursor = select::StageCursor();
          if (cursor >= 0) {
            if (!g_stage_up_at) g_stage_up_at = Now();
            g_stage_last = cursor;
            if (g_stage_taken < 0) StageGo(cursor, g_last_rights, true);
          }
          RunSteps();
        } else {
          OnStageOnline();
        }
        // the stage select never came (30 s), or went and no fight loaded (40 s after a pick, 20 s without one)
        if (select::StageCursor() >= 0) g_stage_gone_at = 0;
        else if (g_stage_up_at && !g_stage_gone_at) g_stage_gone_at = Now();
        const uint64_t grace = g_stage_taken >= 0 ? 2400 : 1200;
        if ((!g_stage_up_at && In() > 1800) || (g_stage_gone_at && Now() > g_stage_gone_at + grace)) Enter(Phase::Lost);
        return;
      }
      // Offline (Training): once the screen surely takes presses (5 s in), from Random (the cursor's first place) RIGHT
      // as often as the seed says (or as for the last match), a press every half second, then A; A again every 1.5 s
      // should it not have taken. (The stage select sets more up with its choice than the stage the battle setup
      // copies, so player 2's game taking player 1's stage there is not enough online: both pick it alike.)
      if (In() == 300) {
        const bool same = g_same_stage;
        const int rights = same ? g_last_rights : SeedPlace();
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
    }
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
