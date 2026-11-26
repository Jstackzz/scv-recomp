// The frame meter, see framemeter.h.
//
// What it reads (found in Training, Mitsurugi against Siegfried, docs\GAME.md "Frame data in practice"), per fighter
// object (player 1's at 0x8294AC10, player 2's at 0x829A8740):
//   +0x177A u16  what the fighter is doing: 1 nothing of its own (standing, walking, holding guard), 2 an action of its
//                own (an attack, a throw, a step, a jump), 3 reacting to a hit (stunned, thrown, in the air, down), 6
//                guarding a hit (guard stun)
//   +0x0578 u32  1 while it may act (attack, guard, move), 0 while an action or a reaction holds it
//   +0x228BC u32 its attack's record (a heap address; +0x14DB is set while the attack runs). sub_8237F928 compares the
//                move's frame with the record's impact window (+0x36 its first frame, +0x38 its last, counted from 0)
//                and keeps the attack's phase at +0x177C u16: 1 startup, 2 impact (the attack can hit), 3 recovery.
//                With no record it leaves +0x177C as it was (a throw that caught keeps 2 through its animation).
//   record +0x33 u8   where the attack hits and what guards it: 0x80 a throw; without 0x02 high (it passes over a
//                crouching fighter); 0x08 standing guard blocks it, 0x10 crouching guard: mid 0x08, low 0x10, special
//                low both (seen: high 0x49, mid 0x4B and 0x6F, low 0x37, special low 0x1B, throw 0x81)
//   record +0x3A s16  its damage before any scaling (counter hits and combos change what the health loses)
//   +0x2268C f32 its health (full at +0x22688: 240 in VS Battle and Training)
//   0x82949D54 u32 the fight's frame count: one a frame while the fight runs, standing still while the game is paused,
//                set back with the rest of the fight by a rollback (in the fight's memory that the snapshot keeps)
//
// Each fight frame's reading (both fighters: what each cell shows, their health, the attack record's level and damage)
// is kept in a ring by that count. A frame run again replaces its earlier run's reading, a paused game adds none, and
// a count that goes back without a rollback (a new round) clears the ring. The meter and the numbers are worked out
// again from the ring after every frame and handed to the overlay, which draws on the UI thread.
//
// The meter, as SF6's: it starts when either player leaves "free to act" and runs until both have been free for
// kGap frames; each run of cells of one kind gets its length at its last cell; past kCells frames it wraps, a white
// line after the newest frame. A player's line is about their last move: startup (the frame its attack first can hit,
// SCV's "impact frame", i13), impact (the frames it can hit), recovery (until it may act again), total; advantage once
// both are free again after a hit or a guard: how many frames sooner this player could act than the other.
// The damage panel: per attacker, the last hit's damage (and its part of the attack's own damage), the combo's (every
// hit since the other player was last free) and the last attack's level.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "sc5/framemeter.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/mirror.h"
#include "sc5/sparring.h"

REX_EXTERN(__imp__sub_824A25C0);  // the game's step (frame.cpp): pads, sequences, the battle

namespace sc5::framemeter {
namespace {

// --- the game's memory ---
constexpr uint32_t kPlayer[2] = {0x8294AC10, 0x829A8740};
constexpr uint32_t kFree = 0x578, kState = 0x177A, kPhase = 0x177C, kAttacking = 0x14DB, kAttack = 0x228BC,
                   kHealth = 0x2268C;
constexpr uint32_t kRecordLevel = 0x33, kRecordDamage = 0x3A;
constexpr uint32_t kFightFrame = 0x82949D54;

// --- the meter ---
enum Kind : uint8_t { kFreeToAct, kStartup, kImpact, kRecovery, kStun, kMoving };
constexpr int kCells = 64;   // frames across (SF6: 80; fewer and larger here)
constexpr int kRing = 4096;  // frames kept (68 s): the damage panel's last hit stays that long
constexpr int kGap = 20;     // frames both players stay free before the next action starts a new meter
constexpr int kMaxNumbers = 96;

struct Side {
  uint8_t kind = kFreeToAct;
  uint8_t level = 0;  // the attack record's +0x33 (0: no record)
  int16_t base = 0;   // its damage before scaling
  float hp = 0;
};
struct Sample {
  int64_t key = -1;  // the fight frame (-1: none kept)
  Side p[2];
};

struct Stats {
  int startup = -1, impact = -1, recovery = -1, total = -1;  // -1: not known (yet)
  bool has_adv = false;
  int adv = 0;
};
struct Damage {
  float last = -1;  // the attacker's last hit (-1: none kept)
  int scale = -1;   // its part of the attack's own damage, percent (-1: not known)
  float combo = 0;
  int hits = 0;
  int level = -1;   // the attacker's last attack's level byte (-1: none kept)
};
struct Number {
  uint8_t side, cell;
  uint16_t value;
};
struct View {
  bool show = false;
  bool training = false;  // Training's own displays: the damage panel goes below them
  int cells = 0;    // cells filled
  int cursor = -1;  // a wrapped meter: the cell after the newest frame
  uint8_t kind[2][kCells] = {};
  Number numbers[kMaxNumbers] = {};
  int n_numbers = 0;
  Stats stats[2];
  Damage dmg[2];
};

std::atomic<bool> g_on{true};  // SC5_FRAMEDATA / display.txt's frame_data, F7
// tests: SC5_FRAMEMETER_TRACE=1 (or <from>:<to>, fight frames) logs every reading, a frame run again's too
std::atomic<bool> g_trace{false};
int64_t g_trace_from = 0, g_trace_to = INT64_MAX;

// main thread
Sample g_ring[kRing];
int64_t g_newest = -1;    // the newest fight frame kept
int64_t g_last_key = -1;  // the fight frame of the last frame run first (not again)
int g_still = 0;          // frames the fight's count has stood still
bool g_active = false;
int64_t g_logged[2] = {-1, -1};  // the last meter logged: its first and last frame

std::mutex g_view_lock;
View g_view;  // under g_view_lock
std::chrono::steady_clock::time_point g_notice_until;  // under g_view_lock
bool g_notice_on = false;                              // under g_view_lock

ImFont* g_font = nullptr;
constexpr float kFontPixels = 64.0f;

const uint8_t* Mem(uint32_t a) { return REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(a); }
uint32_t U32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Mem(a), 4);
  return __builtin_bswap32(v);
}
uint16_t U16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Mem(a), 2);
  return __builtin_bswap16(v);
}
float F32(uint32_t a) {
  const uint32_t bits = U32(a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

const char* LevelName(int b) {
  if (b < 0) return "--";
  if (b & 0x80) return "Throw";
  if (!(b & 0x02)) return "High";
  const bool standing = b & 0x08, crouching = b & 0x10;
  if (standing && crouching) return (b & 0x40) ? "Special Mid" : "Special Low";
  if (standing) return "Mid";
  if (crouching) return "Low";
  return "Unblockable";
}

const char* KindName(uint8_t k) {
  switch (k) {
    case kStartup: return "startup";
    case kImpact: return "impact";
    case kRecovery: return "recovery";
    case kStun: return "stun";
    case kMoving: return "moving";
    default: return "free";
  }
}

// a practice fight: Training's (offline), or Online Practice's in sync
bool InPractice() {
  if (game::BattleState() != game::kBattleFighting) return false;
  return game::SequenceId() == game::kSeqTraining || sparring::On();
}

Side Read(int s) {
  const uint32_t p = kPlayer[s];
  Side out;
  out.hp = F32(p + kHealth);
  const uint16_t state = U16(p + kState);
  const bool free = U32(p + kFree) != 0;
  const uint32_t record = U32(p + kAttack);
  // the attack's record is the game's own while its attack runs (+0x14DB), in the heap
  const bool attacking = *Mem(p + kAttacking) != 0 && record >= 0x40000000u && record < 0x80000000u;
  if (attacking) {
    out.level = *Mem(record + kRecordLevel);
    out.base = int16_t(U16(record + kRecordDamage));
  }
  if (state >= 3) {
    out.kind = kStun;  // a hit (stun, thrown, in the air, down) or a guard: from the frame it lands
  } else if (state == 2 && attacking) {
    switch (U16(p + kPhase)) {
      case 1: out.kind = kStartup; break;
      case 2: out.kind = kImpact; break;
      case 3: out.kind = free ? kFreeToAct : kRecovery; break;
      default: out.kind = free ? kFreeToAct : kMoving; break;
    }
  } else {
    out.kind = free ? kFreeToAct : kMoving;  // a step, a jump, a throw's hold, getting up
  }
  return out;
}

const Sample* At(int64_t k) {
  if (k < 0) return nullptr;
  const Sample& s = g_ring[k % kRing];
  return s.key == k ? &s : nullptr;
}
uint8_t KindAt(int64_t k, int side) {
  const Sample* s = At(k);
  return s ? s->p[side].kind : uint8_t(kFreeToAct);
}
bool BothFree(const Sample& s) { return s.p[0].kind == kFreeToAct && s.p[1].kind == kFreeToAct; }

void Clear() {
  for (Sample& s : g_ring) s.key = -1;
  g_newest = g_last_key = -1;
  g_still = 0;
}

// a player's last move in the meter [first, last]: from the start of its last startup run (a string's next hit
// starts one again), else of its last impact run
Stats MoveStats(int s, int64_t first, int64_t last, bool over) {
  Stats st;
  uint8_t head = kStartup;
  int64_t f = last;
  while (f >= first && KindAt(f, s) != kStartup) f--;
  if (f < first) {
    head = kImpact;
    f = last;
    while (f >= first && KindAt(f, s) != kImpact) f--;
    if (f < first) return st;
  }
  int64_t a = f;
  while (a - 1 >= first && KindAt(a - 1, s) == head) a--;
  int startup = 0, impact = 0, recovery = 0, moving = 0;
  bool ended = false;
  int64_t k = a;
  for (; k <= last; k++) {
    const uint8_t c = KindAt(k, s);
    if (c == kStartup && !impact && !recovery && !moving) startup++;
    else if (c == kImpact && !recovery && !moving) impact++;
    else if (c == kRecovery && impact && !moving) recovery++;
    else if (c == kMoving && impact) moving++;  // a throw holding the other player after it caught
    else {
      ended = c == kFreeToAct;  // free again; else hit, guarding, or another move
      break;
    }
  }
  if (k > last) ended = over;  // the meter's end: free again if both are
  if (impact) st.startup = startup + 1;
  if (impact && (recovery || moving || ended)) st.impact = impact;
  if (ended) {
    st.recovery = moving && !recovery ? -1 : recovery;
    st.total = startup + impact + recovery + moving;
  }
  return st;
}

// the damage panel, from every frame kept
void Panel(View& v) {
  const int64_t lo = std::max<int64_t>(0, g_newest - kRing + 1);
  for (int a = 0; a < 2; a++) {
    const int d = 1 - a;
    Damage& out = v.dmg[a];
    // the attacker's last hit: the other player's health went down (Training's and Online Practice's refills raise it)
    int64_t hit = -1;
    for (int64_t k = g_newest; k > lo && hit < 0; k--) {
      const Sample* s = At(k);
      const Sample* p = At(k - 1);
      if (s && p && s->p[d].hp < p->p[d].hp - 0.01f) hit = k;
    }
    if (hit >= 0) {
      const Sample* s = At(hit);
      const Sample* p = At(hit - 1);
      out.last = p->p[d].hp - s->p[d].hp;
      if (s->p[a].kind == kImpact && s->p[a].base > 0) out.scale = int(std::lround(out.last * 100.0f / s->p[a].base));
      // the combo: every hit since the other player was last free
      for (int64_t k = hit; k > lo; k--) {
        const Sample* x = At(k);
        const Sample* y = At(k - 1);
        if (!x || !y) break;
        if (x->p[d].hp < y->p[d].hp - 0.01f) {
          out.combo += y->p[d].hp - x->p[d].hp;
          out.hits++;
        }
        if (y->p[d].kind == kFreeToAct) break;
      }
    }
    for (int64_t k = g_newest; k >= lo; k--) {
      const Sample* s = At(k);
      if (s && s->p[a].kind == kImpact && s->p[a].level) {
        out.level = s->p[a].level;
        break;
      }
    }
  }
}

std::string Num(int v) { return v < 0 ? std::string("--") : std::to_string(v); }

std::string Amount(float v) {
  char buf[32];
  if (std::fabs(v - std::round(v)) < 0.05f) std::snprintf(buf, sizeof buf, "%.0f", v);
  else std::snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

void Log(const View& v, int64_t first, int64_t last) {
  if (g_logged[0] == first && g_logged[1] == last) return;
  g_logged[0] = first;
  g_logged[1] = last;
  std::string line;
  for (int s = 0; s < 2; s++) {
    const Stats& st = v.stats[s];
    line += "P" + std::to_string(s + 1) + " startup " + Num(st.startup) + " impact " + Num(st.impact) + " recovery " +
            Num(st.recovery) + " total " + Num(st.total);
    if (st.has_adv) line += std::string(" advantage ") + (st.adv > 0 ? "+" : "") + std::to_string(st.adv);
    const Damage& d = v.dmg[s];
    if (d.last >= 0)
      line += ", damage " + Amount(d.last) + (d.scale >= 0 ? " (" + std::to_string(d.scale) + "%)" : "") +
              " combo " + Amount(d.combo) + " (" + std::to_string(d.hits) + (d.hits == 1 ? " hit)" : " hits)");
    if (d.level >= 0) line += std::string(", ") + LevelName(d.level);
    line += s ? "" : " | ";
  }
  REXLOG_INFO("sc5 framemeter: fight frames {}-{}: {}", first, last, line);
}

void Analyze(View& v) {
  if (g_newest < 0) return;
  const int64_t lo = std::max<int64_t>(0, g_newest - kRing + 1);
  // back over the frames both players have been free since
  int64_t last = g_newest;
  while (last >= lo && At(last) && BothFree(*At(last))) last--;
  const bool over = last < g_newest;
  if (last >= lo && At(last)) {
    // the meter's first frame: back until both stayed free for kGap frames
    int64_t first = last;
    int idle = 0;
    for (int64_t k = last - 1; k >= lo; k--) {
      const Sample* s = At(k);
      if (!s) break;
      if (BothFree(*s)) {
        if (++idle >= kGap) break;
      } else {
        idle = 0;
        first = k;
      }
    }
    const int64_t n = last - first + 1;
    const int64_t shown = std::max(first, last - kCells + 1);
    for (int64_t k = shown; k <= last; k++)
      for (int s = 0; s < 2; s++) v.kind[s][(k - first) % kCells] = KindAt(k, s);
    v.cells = int(std::min<int64_t>(n, kCells));
    v.cursor = n > kCells ? int(n % kCells) : -1;
    // each run's length at its last cell (free cells have none)
    for (int s = 0; s < 2; s++) {
      int64_t run = first;
      for (int64_t k = first; k <= last; k++) {
        const uint8_t c = KindAt(k, s);
        if (k < last && KindAt(k + 1, s) == c) continue;
        if (c != kFreeToAct && k >= shown && v.n_numbers < kMaxNumbers)
          v.numbers[v.n_numbers++] = {uint8_t(s), uint8_t((k - first) % kCells),
                                      uint16_t(std::min<int64_t>(k - run + 1, 999))};
        run = k + 1;
      }
    }
    for (int s = 0; s < 2; s++) v.stats[s] = MoveStats(s, first, last, over);
    // the advantage, once both are free after a hit or a guard: who could act first, by how many frames
    if (over) {
      int64_t busy[2] = {-1, -1};
      bool stun = false;
      for (int64_t k = first; k <= last; k++)
        for (int s = 0; s < 2; s++) {
          const uint8_t c = KindAt(k, s);
          if (c != kFreeToAct) busy[s] = k;
          stun = stun || c == kStun;
        }
      if (stun && busy[0] >= 0 && busy[1] >= 0) {
        v.stats[0].has_adv = v.stats[1].has_adv = true;
        v.stats[0].adv = int(busy[1] - busy[0]);
        v.stats[1].adv = -v.stats[0].adv;
      }
    }
    Panel(v);
    if (over && !frame::Replaying()) Log(v, first, last);
    return;
  }
  Panel(v);
}

void AfterStep() {
  if (!g_on.load(std::memory_order_relaxed) || !InPractice()) {
    if (g_active) {
      g_active = false;
      Clear();
      std::lock_guard<std::mutex> lock(g_view_lock);
      g_view = View{};
    }
    return;
  }
  g_active = true;
  const int64_t key = U32(kFightFrame);
  const bool again = frame::Replaying();
  if (!again) {
    if (key == g_last_key) {  // the fight stands still: Training's menus and windows pause it
      if (++g_still == 2) {
        std::lock_guard<std::mutex> lock(g_view_lock);
        g_view.show = false;
      }
      return;
    }
    g_still = 0;
    if (g_newest >= 0 && key < g_newest) Clear();  // the count went back with no rollback: a new round
    g_last_key = key;
  }
  Sample& s = g_ring[key % kRing];
  s.key = key;
  s.p[0] = Read(0);
  s.p[1] = Read(1);
  if (key > g_newest) {
    for (int64_t k = std::max(g_newest + 1, key - kRing + 1); k < key; k++) g_ring[k % kRing].key = -1;
    g_newest = key;
  }
  if (g_trace.load(std::memory_order_relaxed) && key >= g_trace_from && key <= g_trace_to)
    REXLOG_INFO("sc5 framemeter: fight frame {}{}: P1 {} {:.1f}, P2 {} {:.1f}", key, again ? " (again)" : "",
                KindName(s.p[0].kind), s.p[0].hp, KindName(s.p[1].kind), s.p[1].hp);
  if (again) return;  // the frame now (run after a rollback's) works it all out again
  View v;
  Analyze(v);
  v.show = true;
  v.training = game::SequenceId() == game::kSeqTraining;
  std::lock_guard<std::mutex> lock(g_view_lock);
  g_view = v;
}

// --- drawing (UI thread) ---

ImU32 KindColor(uint8_t k) {
  switch (k) {
    case kStartup: return IM_COL32(0, 196, 160, 255);   // SF6's (0, 183, 151), a little brighter
    case kImpact: return IM_COL32(226, 52, 115, 255);   // (203, 43, 102)
    case kRecovery: return IM_COL32(16, 128, 222, 255);  // (1, 111, 190)
    case kStun: return IM_COL32(255, 244, 40, 255);     // (254, 248, 48)
    case kMoving: return IM_COL32(150, 150, 160, 255);
    default: return IM_COL32(44, 44, 44, 255);           // free to act, inside the meter (SF6: 26)
  }
}
constexpr ImU32 kEmpty = IM_COL32(18, 18, 18, 255);
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);
constexpr ImU32 kAhead = IM_COL32(110, 196, 255, 255);   // SF6: blue when ahead
constexpr ImU32 kBehind = IM_COL32(255, 104, 104, 255);  // red when behind

ImFont* Font() { return g_font ? g_font : ImGui::GetFont(); }

float TextWidth(float px, const char* text) { return Font()->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x; }

// text with a black outline, readable over any stage
void Text(ImDrawList* dl, float px, ImVec2 at, ImU32 color, const char* text) {
  const float o = std::max(1.5f, px / 16.0f);
  static const float kDirs[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  for (const auto& d : kDirs) dl->AddText(Font(), px, ImVec2(at.x + d[0] * o, at.y + d[1] * o), kBlack, text);
  dl->AddText(Font(), px, at, color, text);
}

// a player's line: "Startup 13F / Impact 3F / Recovery 25F / Total 40F / Advantage +2F", the advantage in color
void Line(ImDrawList* dl, float px, float x, float y, float room, const Stats& st) {
  auto value = [](int v) { return v < 0 ? std::string("--") : std::to_string(v) + "F"; };
  const std::string head = "Startup " + value(st.startup) + " / Impact " + value(st.impact) + " / Recovery " +
                           value(st.recovery) + " / Total " + value(st.total) + " / Advantage ";
  std::string adv = "--";
  ImU32 adv_color = kWhite;
  if (st.has_adv) {
    adv = (st.adv > 0 ? "+" : "") + std::to_string(st.adv) + "F";
    adv_color = st.adv > 0 ? kAhead : st.adv < 0 ? kBehind : kWhite;
  }
  const float w = TextWidth(px, head.c_str()) + TextWidth(px, adv.c_str());
  if (w > room) px *= room / w;  // a narrow window: smaller, never cut
  Text(dl, px, ImVec2(x, y), kWhite, head.c_str());
  Text(dl, px, ImVec2(x + TextWidth(px, head.c_str()), y), adv_color, adv.c_str());
}

class MeterDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MeterDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    View v;
    bool notice = false, notice_on = false;
    {
      std::lock_guard<std::mutex> lock(g_view_lock);
      v = g_view;
      notice = std::chrono::steady_clock::now() < g_notice_until;
      notice_on = g_notice_on;
    }
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W < 64 || H < 64) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float ts = std::clamp(H / 20.0f, 16.0f, 110.0f);  // text height (54 px at 1080 lines)
    const bool shown = v.show && g_on.load(std::memory_order_relaxed);
    // the meter: P1's line, P1's row, P2's row, P2's line; its bottom at 80% of the height in Training (above its input
    // history, which stacks a press's direction and button from 81%, and its button help), at 86% in Online Practice
    // (above the gauges in the corners)
    const float bar_w = W * 0.86f, x0 = (W - bar_w) * 0.5f;
    const float cell_w = bar_w / kCells;
    const float cell_h = std::max(cell_w * 1.7f, ts * 0.95f);
    const float gap = std::max(1.0f, cell_w * 0.14f);
    const float bottom = H * (v.training ? 0.80f : 0.86f);
    const float y_p2_line = bottom - ts, y_p2_row = y_p2_line - gap * 2 - cell_h, y_p1_row = y_p2_row - gap * 3 - cell_h;
    const float y_p1_line = y_p1_row - gap * 2 - ts;
    if (shown) {
      const float pad = ts * 0.35f;
      dl->AddRectFilled(ImVec2(x0 - pad, y_p1_line - pad), ImVec2(x0 + bar_w + pad, bottom + pad),
                        IM_COL32(0, 0, 0, 160), pad);
      const float rows[2] = {y_p1_row, y_p2_row};
      for (int s = 0; s < 2; s++) {
        dl->AddRectFilled(ImVec2(x0, rows[s] - gap), ImVec2(x0 + bar_w, rows[s] + cell_h + gap), kBlack);
        for (int i = 0; i < kCells; i++) {
          const ImU32 c = i < v.cells ? KindColor(v.kind[s][i]) : kEmpty;
          dl->AddRectFilled(ImVec2(x0 + i * cell_w + gap * 0.5f, rows[s]),
                            ImVec2(x0 + (i + 1) * cell_w - gap * 0.5f, rows[s] + cell_h), c);
        }
      }
      // each run's length, a digit a cell, ending at the run's last cell
      const float digit_px = std::min(cell_h * 0.92f, cell_w * 1.55f);
      for (int i = 0; i < v.n_numbers; i++) {
        const Number& n = v.numbers[i];
        const std::string digits = std::to_string(n.value);
        for (size_t j = 0; j < digits.size(); j++) {
          const int cell = int(n.cell) - int(digits.size() - 1 - j);
          if (cell < 0) continue;
          const char d[2] = {digits[j], 0};
          const float dw = TextWidth(digit_px, d);
          Text(dl, digit_px, ImVec2(x0 + cell * cell_w + (cell_w - dw) * 0.5f, rows[n.side] + (cell_h - digit_px) * 0.5f),
               kWhite, d);
        }
      }
      if (v.cursor >= 0) {
        const float x = x0 + v.cursor * cell_w;
        dl->AddRectFilled(ImVec2(x - gap, y_p1_row - gap * 2), ImVec2(x + gap, y_p2_row + cell_h + gap * 2), kWhite);
      }
      const float label_w = TextWidth(ts, "Player 2") + ts;
      Line(dl, ts, x0, y_p1_line, bar_w - label_w, v.stats[0]);
      Line(dl, ts, x0, y_p2_line, bar_w - label_w, v.stats[1]);
      Text(dl, ts, ImVec2(x0 + bar_w - TextWidth(ts, "Player 1"), y_p1_line), kWhite, "Player 1");
      Text(dl, ts, ImVec2(x0 + bar_w - TextWidth(ts, "Player 2"), y_p2_line), kWhite, "Player 2");
      // the damage panel, as SF6's: player 1's values, the row's name, player 2's; each player's values on the side
      // of the screen their fighter is shown on, so swapped while this game shows the fight mirrored (sc5/mirror.h)
      const bool mirrored = sc5::mirror::Mirrored();
      const float pt = ts * 0.8f;
      const char* names[3] = {"Damage", "Combo Damage", "Attack Type"};
      std::string vals[2][3];
      for (int s = 0; s < 2; s++) {
        const Damage& d = v.dmg[s];
        vals[s][0] = d.last < 0 ? "--" : Amount(d.last) + (d.scale >= 0 ? " (" + std::to_string(d.scale) + "%)" : "");
        vals[s][1] = d.hits <= 0 ? "--" : Amount(d.combo) + " (" + std::to_string(d.hits) + (d.hits == 1 ? " hit)" : " hits)");
        vals[s][2] = LevelName(d.level);
      }
      const float name_w = TextWidth(pt, "Combo Damage") + pt * 1.5f;
      float val_w = TextWidth(pt, "000.0 (100%)");
      for (auto& side : vals)
        for (auto& s : side) val_w = std::max(val_w, TextWidth(pt, s.c_str()));
      const float panel_w = name_w + 2 * (val_w + pt);
      // below the health bars; in Training below its own displays too (the dummy's settings, top right, to 23%)
      const float px0 = (W - panel_w) * 0.5f, py0 = H * (v.training ? 0.245f : 0.15f), row_h = pt * 1.25f;
      dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px0 + panel_w, py0 + row_h * 3), IM_COL32(0, 0, 0, 170), pt * 0.3f);
      for (int r = 0; r < 3; r++) {
        const float y = py0 + r * row_h + (row_h - pt) * 0.5f;
        if (r) dl->AddLine(ImVec2(px0, py0 + r * row_h), ImVec2(px0 + panel_w, py0 + r * row_h), IM_COL32(255, 255, 255, 70));
        const float name_x = px0 + val_w + pt + (name_w - TextWidth(pt, names[r])) * 0.5f;
        Text(dl, pt, ImVec2(name_x, y), IM_COL32(255, 230, 140, 255), names[r]);
        const std::string& left = vals[mirrored ? 1 : 0][r];
        const std::string& right = vals[mirrored ? 0 : 1][r];
        Text(dl, pt, ImVec2(px0 + val_w + pt * 0.5f - TextWidth(pt, left.c_str()), y), kWhite, left.c_str());
        Text(dl, pt, ImVec2(px0 + val_w + pt * 1.5f + name_w, y), kWhite, right.c_str());
      }
    }
    if (notice) {
      const char* text = notice_on ? "Frame meter on  (F7 hides it)" : "Frame meter off  (F7 shows it)";
      const float w = TextWidth(ts, text);
      const float y = shown ? y_p1_line - ts * 1.8f : H * 0.75f;
      dl->AddRectFilled(ImVec2((W - w) * 0.5f - ts * 0.4f, y - ts * 0.2f), ImVec2((W + w) * 0.5f + ts * 0.4f, y + ts * 1.2f),
                        IM_COL32(0, 0, 0, 190), ts * 0.25f);
      Text(dl, ts, ImVec2((W - w) * 0.5f, y), kWhite, text);
    }
  }
};

// never deleted: the runtime keeps its drawer until the window closes and deletes none of its dialogs
MeterDialog* g_dialog = nullptr;

}  // namespace

void SetEnabled(bool on) {
  if (g_on.exchange(on) != on) REXLOG_INFO("sc5 framemeter: {} (the launcher's setting)", on ? "shown" : "hidden");
}

void AddFonts(ImFontAtlas* atlas) {
  // a large, bold Windows font: the meter is read from across a room (the runtime's own font is 10 px)
  char path[MAX_PATH];
  const UINT n = GetWindowsDirectoryA(path, MAX_PATH);
  if (!atlas || !n || n > MAX_PATH - 32) return;
  for (const char* name : {"\\Fonts\\segoeuib.ttf", "\\Fonts\\arialbd.ttf"}) {
    std::string file = std::string(path) + name;
    if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 1;
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0};
    g_font = atlas->AddFontFromFileTTF(file.c_str(), kFontPixels, &config, kRanges);
    if (g_font) break;
  }
  if (!g_font) REXLOG_WARN("sc5 framemeter: no Segoe UI Bold or Arial Bold: the runtime's small font");
}

ImFont* LargeFont() { return Font(); }

void CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (const char* v = std::getenv("SC5_FRAMEMETER_TRACE"); v && *v && *v != '0') {
    g_trace = true;
    if (const char* colon = std::strchr(v, ':')) {
      g_trace_from = std::atoll(v);
      g_trace_to = std::atoll(colon + 1);
    }
  }
  if (!g_dialog) g_dialog = new MeterDialog(drawer);
  // F7 shows or hides it (the launcher keeps F6-F12 for the game's practice tools): the runtime's own F7, its
  // achievements overlay (SCV has none), goes
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::RegisterBind("bind_frame_meter", "F7", "Show or hide the frame meter in practice", [] {
    const bool on = !g_on.load();
    g_on = on;
    REXLOG_INFO("sc5 framemeter: {} (F7)", on ? "shown" : "hidden");
    std::lock_guard<std::mutex> lock(g_view_lock);
    g_notice_on = on;
    g_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  });
}

}  // namespace sc5::framemeter

// After the game's step (every run of a frame, a rollback's included): the fighters read for the meter
REX_HOOK_RAW(sub_824A25C0) {
  __imp__sub_824A25C0(ctx, base);
  sc5::framemeter::AfterStep();
}
