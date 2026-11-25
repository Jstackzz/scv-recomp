// Sound and rollback. The game plays its sounds through CRI AtomEx: it gives a player a cue (criAtomExPlayer_SetCueId,
// sub_8257C298: r3 the player, r4 the cue sheet, r5 the cue) and starts it (criAtomExPlayer_Start, sub_8257CBA8: r3
// the player; the playback id back, -1 when nothing started). Its sound handles keep the playback id, and two of its
// three ways to start a sound (sub_821D6BB8, sub_821D6E58) stop the handle's player first (criAtomExPlayer_Stop,
// sub_8257C318, through sub_821D6CE8); it stops one sound with criAtomExPlayback_Stop (sub_8257C0F8: r3 the playback
// id) and asks after one with criAtomExPlayback_GetStatus (sub_8257CB10: 1 preparing, 2 playing, 3 gone).
//
// A rollback runs frames again that started their sounds already. As Timestone (the UMVC3 rollback mod) plans it,
// every start is kept with its frame, player and cue, and a rollback (RollbackBegin .. RollbackEnd) reconciles them:
// - a start in a frame run again that matches one of the first run's (the same player and cue, two frames apart at
//   most) is left out, and the game gets that sound's playback id: the sound plays on, and the game stops it or asks
//   after it as before (it used to get -1: "gone");
// - a start that matches none starts then (the corrected frames did what the guess had missed: late by the rollback's
//   length at most);
// - a sound the first run started in those frames that no frame run again started again came from a wrong guess: it
//   is stopped when the rollback is done;
// - a player's stop in a frame run again (a handle about to start its next sound) leaves what the first run started on
//   that player from two frames before on and is not matched yet (it used to stop it, and the start after it was left
//   out as playing already: the sound went silent): the rest on that player is stopped one by one.
// What stays: a sound a wrong guess cut short (a handle's next sound that did not come) does not come back.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"
#include "sc5/sound.h"

REX_EXTERN(__imp__sub_8257CBA8);
REX_EXTERN(__imp__sub_8257C298);
REX_EXTERN(__imp__sub_8257C318);
REX_EXTERN(__imp__sub_8257C0F8);
REX_EXTERN(__imp__sub_8257B428);

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Cue {
  uint32_t sheet = 0, cue = kNone;
};
struct Rec {
  uint64_t frame;
  uint32_t player, sheet, cue, id;
  bool open;  // a rollback's: started in its frames' first run and not started again by them yet
};
// main thread: the starts of the last ten seconds, and the cue each player was given last
std::deque<Rec> g_recs;
std::unordered_map<uint32_t, Cue> g_cue;
bool g_rolling_back = false;
uint64_t g_back_to = 0;  // the rollback's first frame
uint64_t g_matched = 0, g_late = 0, g_wrong = 0, g_narrowed = 0;

bool Spared(const Rec& r, uint32_t player) { return r.open && r.player == player; }

void Prune(uint64_t frame) {
  if (g_recs.size() < 512) return;
  g_recs.erase(std::remove_if(g_recs.begin(), g_recs.end(), [&](const Rec& r) { return r.frame + 600 < frame; }),
               g_recs.end());
}

}  // namespace

REX_HOOK_RAW(sub_8257C298) {
  if (sc5::frame::OnMainThread()) g_cue[ctx.r3.u32] = Cue{ctx.r4.u32, ctx.r5.u32};
  __imp__sub_8257C298(ctx, base);
}

REX_HOOK_RAW(sub_8257CBA8) {
  if (!sc5::frame::OnMainThread()) return __imp__sub_8257CBA8(ctx, base);
  const uint64_t frame = sc5::frame::Count();
  const uint32_t player = ctx.r3.u32;
  const auto c = g_cue.find(player);
  const Cue cue = c != g_cue.end() ? c->second : Cue{};
  if (g_rolling_back && sc5::frame::Replaying()) {
    // the first run's start of the same cue in the rollback's frames: this frame's first, else the nearest. Not by
    // player: the game deals its players out in turn, and the turn is not part of what rollback puts back. Not this
    // frame's only: the game starts some sounds when the one before has ended (CRI's status, sub_8257CB10), which is
    // the audio's time: a frame run again later sees it ended frames earlier than the first run did.
    Rec* best = nullptr;
    uint64_t best_d = 0;
    for (Rec& r : g_recs) {
      if (!r.open || r.sheet != cue.sheet || r.cue != cue.cue) continue;
      const uint64_t d = r.frame > frame ? r.frame - frame : frame - r.frame;
      if (best && d >= best_d) continue;
      best = &r;
      best_d = d;
    }
    if (best) {
      best->open = false;
      best->frame = frame;
      if (g_matched++ % 1000 == 0)
        REXLOG_INFO("sc5 sound: in frames run again {} starts left out (playing already), {} started late, {} "
                    "stopped (wrong guesses), {} player stops narrowed",
                    g_matched, g_late, g_wrong, g_narrowed);
      ctx.r3.u64 = best->id;
      return;
    }
    static const bool dbg = [] {
      const char* v = std::getenv("SC5_DBG_SOUND");
      return v && v[0] == '1';
    }();
    if (dbg && g_late < 30) {
      std::string kept;
      for (const Rec& r : g_recs)
        if (r.frame + 4 >= frame && r.frame <= frame + 4)
          kept += fmt::format(" [{}{} p{:X} {}:{}]", r.frame, r.open ? " open" : "", r.player, r.sheet, r.cue);
      REXLOG_INFO("sc5 dbg sound: frame {} run again starts {}:{} on player {:X}, unmatched; kept near it:{}", frame,
                  cue.sheet, cue.cue, player, kept);
    }
    __imp__sub_8257CBA8(ctx, base);
    g_recs.push_back(Rec{frame, player, cue.sheet, cue.cue, ctx.r3.u32, false});
    g_late++;
    return;
  }
  __imp__sub_8257CBA8(ctx, base);
  g_recs.push_back(Rec{frame, player, cue.sheet, cue.cue, ctx.r3.u32, false});
  Prune(frame);
}

// criAtomExPlayer_Stop (r3 the player) in a frame run again: what is still to be matched on that player is spared
REX_HOOK_RAW(sub_8257C318) {
  if (!g_rolling_back || !sc5::frame::Replaying() || !sc5::frame::OnMainThread())
    return __imp__sub_8257C318(ctx, base);
  const uint32_t player = ctx.r3.u32;
  bool spare = false;
  for (const Rec& r : g_recs) spare = spare || Spared(r, player);
  if (!spare) return __imp__sub_8257C318(ctx, base);
  const PPCContext saved = ctx;
  for (const Rec& r : g_recs) {
    if (r.player != player || r.id == kNone || Spared(r, player)) continue;
    ctx = saved;
    ctx.r3.u64 = r.id;
    __imp__sub_8257C0F8(ctx, base);
  }
  ctx = saved;
  g_narrowed++;
}

namespace sc5::sound {

void RollbackBegin(uint64_t first_frame) {
  g_rolling_back = true;
  g_back_to = first_frame;
  for (Rec& r : g_recs) r.open = r.frame >= first_frame;
}

void RollbackEnd(PPCContext& ctx, uint8_t* base) {
  if (!g_rolling_back) return;
  g_rolling_back = false;
  // what the frames run again did not start again: from a wrong guess
  const PPCContext saved = ctx;
  for (auto it = g_recs.begin(); it != g_recs.end();) {
    if (!it->open) {
      ++it;
      continue;
    }
    if (it->id != kNone) {
      ctx = saved;
      ctx.r3.u64 = it->id;
      __imp__sub_8257C0F8(ctx, base);
    }
    g_wrong++;
    it = g_recs.erase(it);
  }
  ctx = saved;
  static uint64_t rollbacks = 0;
  if (++rollbacks % 100 == 0)
    REXLOG_INFO("sc5 sound: {} rollbacks: {} starts left out (playing already), {} started late, {} stopped (wrong "
                "guesses), {} player stops narrowed",
                rollbacks, g_matched, g_late, g_wrong, g_narrowed);
}

}  // namespace sc5::sound

// The music, effects and voices levels. The game sets its sound categories' volumes in one place: sub_82346FA8 applies
// its sound options (the object at 0x82921808, levels 0 to 15) through sub_821D4E30 (r3 the category, f1 the volume, 0
// to 1), which scales the volume by the category's base level (the floats at 0x82916188) and hands it to CRI
// (criAtomExCategory_SetVolumeById, sub_8257B428: r3 the category's id, f1 its volume; nothing else calls it). Each
// level scales what the game hands CRI there, after the game's own clamp (so above 100% is louder than the game's
// loudest), and nothing of the game's own changes: its options and its records of them stay as the player set them.
// A change applies at the top of the next frame (the main thread, where the game sets them too): each category again
// at the volume the game asked for last, scaled anew. CRI's category table is not part of what rollback puts back.
//
// Test switches: SC5_TEST_SOUND_CYCLE=1 mutes one category after another (none, 0, 1, ..., every 6 s, logged; with
// SC5_TEST_AUDIO_RMS=1 the mix's loudness is logged each second, display.cpp); SC5_DBG_SOUND=1 logs the sound
// configuration's tables that name categories (CRI's @UTF tables in the game's memory) the first time a volume is set.
namespace {

// The sound configuration (the ACF, SC5_DBG_SOUND) has 23 categories in two groups; a cue is in one of each group's.
// Group 0 is the four the game sets: id 0 SE, 1 SYSTEMSE (the menus' sounds), 2 VOICE (the fighters and the announcer),
// 3 BGM (the main menu's music goes silent with it muted). Group 1 (SE_hit_weapon, Voice_chara, BGM_battle, ...) the
// game leaves at 1. Its options: +20 sets SE and SYSTEMSE, +28 VOICE, +16 BGM.
constexpr uint32_t kCategories = 4;
// the slider each category follows: 0 music, 1 effects, 2 voices
constexpr int kSliderOf[kCategories] = {1, 1, 2, 0};
const char* const kSliderName[3] = {"music", "effects", "voices"};

std::atomic<float> g_level[3] = {1.0f, 1.0f, 1.0f};
std::atomic<int> g_test_mute{-1};

std::mutex g_volume_lock;
bool g_asked_any[kCategories] = {};
double g_asked[kCategories] = {};    // what the game handed CRI last
float g_applied[kCategories] = {};  // the factor it went with

float FactorOf(uint32_t cat) {
  if (int(cat) == g_test_mute.load(std::memory_order_relaxed)) return 0.0f;
  const int slider = kSliderOf[cat];
  return slider < 0 ? 1.0f : g_level[slider].load(std::memory_order_relaxed);
}

// the top of a frame (frame::Post): the categories whose factor changed, again
void Reapply(PPCContext& ctx, uint8_t* base) {
  for (uint32_t cat = 0; cat < kCategories; cat++) {
    double volume;
    float factor;
    {
      std::lock_guard<std::mutex> lock(g_volume_lock);
      factor = FactorOf(cat);
      if (!g_asked_any[cat] || g_applied[cat] == factor) continue;
      g_applied[cat] = factor;
      volume = g_asked[cat];
    }
    const PPCContext saved = ctx;
    ctx.r3.u64 = cat;
    ctx.f1.f64 = volume * factor;
    __imp__sub_8257B428(ctx, base);
    ctx = saved;
    REXLOG_INFO("sc5 sound: category {} at {:.0f}% of the game's {:.2f}", cat, factor * 100.0f, volume);
  }
}

#if defined(_WIN32)
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// SC5_DBG_SOUND: CRI's @UTF tables in the game's memory whose strings name a category (the sound configuration's
// category table among them: its names in row order)
void LogCategoryTables() {
  auto* mem = REX_KERNEL_STATE()->memory();
  int found = 0;
  uint64_t a = 0x00010000;
  while (a < 0xC0000000ull && found < 12) {
    uint8_t* h = mem->TranslateVirtual<uint8_t*>(uint32_t(a));
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(h, &mbi, sizeof mbi)) break;
    const uint64_t left = uint64_t(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - h);
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                          PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
    if (readable && left > 32) {
      for (uint64_t i = 0; i + 32 < left && found < 12; i++) {
        const uint8_t* p = h + i;
        if (p[0] != '@' || p[1] != 'U' || p[2] != 'T' || p[3] != 'F') continue;
        const uint32_t size = Be32(p + 4);
        if (size < 0x18 || size > 0x1000000 || i + 8 + size > left) continue;
        const uint8_t* t = p + 8;
        const uint32_t strings = Be32(t + 4), data = Be32(t + 8), name = Be32(t + 12);
        if (strings >= data || data > size || strings + name >= data) continue;
        const char* pool = reinterpret_cast<const char*>(t + strings);
        const size_t pool_len = data - strings;
        bool categories = false;
        for (size_t k = 0; k + 5 <= pool_len && !categories; k++)
          categories = (pool[k] == 'C' || pool[k] == 'c') && std::memcmp(pool + k + 1, "ateg", 4) == 0;
        if (!categories) continue;
        std::string list;
        int n = 0;
        for (size_t k = 0; k < pool_len && n < 80; n++) {
          const size_t len = strnlen(pool + k, pool_len - k);
          list += std::string(pool + k, len);
          list += '|';
          k += len + 1;
        }
        REXLOG_INFO("sc5 dbg sound: @UTF at {:08X} ({} bytes, {} columns, {} rows), table {}: {}", uint32_t(a + i),
                    size, Be16(t + 16), Be32(t + 20), std::string(pool + name, strnlen(pool + name, pool_len - name)),
                    list);
        found++;
        // its rows (the small ones): each column's flags (storage 0x10 none, 0x30 constant, 0x50 per row; type 0-7
        // integers of 1, 2, 4, 8 bytes, 8 float, 0xA string, 0xB data) and name, a constant's value after it
        const uint32_t columns = Be16(t + 16), rows = Be32(t + 20), rows_at = Be16(t + 2), width = Be16(t + 18);
        if (rows > 64 || columns > 16) continue;
        auto str = [&](uint32_t off) { return off < pool_len ? std::string(pool + off, strnlen(pool + off, pool_len - off)) : std::string("?"); };
        static const int kSize[16] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8, 0, 0, 0, 0};
        auto value = [&](const uint8_t* v, int type) -> std::string {
          switch (type) {
            case 0: case 1: return std::to_string(v[0]);
            case 2: case 3: return std::to_string(Be16(v));
            case 4: case 5: return std::to_string(Be32(v));
            case 0xA: return str(Be32(v));
            default: return "#";
          }
        };
        for (uint32_t r = 0; r < rows; r++) {
          const uint8_t* s = t + 0x18;
          const uint8_t* row = t + rows_at + r * width;
          std::string line;
          for (uint32_t c = 0; c < columns; c++) {
            const int flags = s[0], type = flags & 0x0F, storage = flags & 0xF0;
            const std::string col = str(Be32(s + 1));
            s += 5;
            if (storage == 0x30) {
              line += col + "=" + value(s, type) + "(c) ";
              s += kSize[type];
            } else if (storage == 0x50) {
              line += col + "=" + value(row, type) + " ";
              row += kSize[type];
            }
          }
          REXLOG_INFO("sc5 dbg sound:   row {}: {}", r, line);
        }
      }
    }
    a += std::max<uint64_t>(left, 0x1000);
  }
  REXLOG_INFO("sc5 dbg sound: {} category tables found", found);
}
#endif

}  // namespace

REX_HOOK_RAW(sub_8257B428) {
  const uint32_t cat = ctx.r3.u32;
  static bool dbg_once = [] {
    const char* v = std::getenv("SC5_DBG_SOUND");
    return v && v[0] == '1';
  }();
  if (dbg_once) {
    dbg_once = false;
#if defined(_WIN32)
    LogCategoryTables();
#endif
  }
  if (cat < kCategories) {
    float factor;
    {
      std::lock_guard<std::mutex> lock(g_volume_lock);
      g_asked[cat] = ctx.f1.f64;
      g_asked_any[cat] = true;
      factor = FactorOf(cat);
      g_applied[cat] = factor;
    }
    static int told = 0;
    if (told++ < 16)
      REXLOG_INFO("sc5 sound: the game sets category {} to {:.2f}{}", cat, ctx.f1.f64,
                  factor != 1.0f ? " (scaled by the launcher's level)" : "");
    ctx.f1.f64 *= factor;
  }
  __imp__sub_8257B428(ctx, base);
}

// The game's movies (CRI Mana; sub_825B25C0: r3 the movie player, f1 its volume) play at the game's music level (the
// float sub_82346FA8 keeps at 0x827610B8; sub_821D2710 hands it over, the only caller): the music level scales them too.
REX_EXTERN(__imp__sub_825B25C0);
REX_HOOK_RAW(sub_825B25C0) {
  ctx.f1.f64 *= g_level[0].load(std::memory_order_relaxed);
  __imp__sub_825B25C0(ctx, base);
}

namespace sc5::sound {

void SetLevels(float music, float effects, float voices) {
  const float want[3] = {music, effects, voices};
  bool changed = false;
  for (int i = 0; i < 3; i++) {
    const float v = want[i] < 0.0f ? 0.0f : want[i] > 1.5f ? 1.5f : want[i];
    if (g_level[i].exchange(v) != v) {
      changed = true;
      REXLOG_INFO("sc5 sound: {} at {:.0f}%", kSliderName[i], v * 100.0f);
    }
  }
  if (changed) sc5::frame::Post(&Reapply);
  static bool cycling = [] {
    const char* v = std::getenv("SC5_TEST_SOUND_CYCLE");
    if (!v || v[0] != '1') return false;
    std::thread([] {
      for (int step = 0;; step++) {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        const int phases = int(kCategories) + 1;
        const int mute = step % phases == 0 ? -1 : step % phases - 1;
        g_test_mute.store(mute);
        REXLOG_INFO("sc5 sound test: {}", mute < 0 ? std::string("no category muted")
                                                    : "category " + std::to_string(mute) + " muted");
        // the category muted before gets its volume back too
        std::lock_guard<std::mutex> lock(g_volume_lock);
        for (float& f : g_applied) f = -1.0f;
        sc5::frame::Post(&Reapply);
      }
    }).detach();
    return true;
  }();
  (void)cycling;
}

}  // namespace sc5::sound
