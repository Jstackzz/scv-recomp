// The single-PC rollback check, see synctest.h.

#include "sc5/net/synctest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <tuple>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "sc5/battle.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/input.h"
#include "sc5/sound.h"

namespace sc5::dbg {  // TEMPORARY (debug_watch.cpp)
bool WideWanted(int64_t fight_frame);
void WideBefore(uint32_t r1);
void WideAfter(int64_t fight_frame);
}  // namespace sc5::dbg

namespace sc5::synctest {
namespace {

constexpr int kRing = 16;
int g_back = 0;  // frames put back each check
int g_every = 1;
std::array<uint32_t, kRing> g_ring{};
std::array<int64_t, kRing> g_ring_frame{};
uint32_t g_check = 0;
uint64_t g_tests = 0, g_failures = 0, g_refused = 0, g_logged = 0, g_snapshot_failures = 0;
double g_ms_total = 0, g_ms_worst = 0, g_save_ms_total = 0;  // a rollback (restore, frames run again, save), a save

// where two snapshots differ: the first offsets, as ranges
std::string Differences(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb, uint32_t& count) {
  std::string out;
  count = 0;
  const uint32_t n = std::min(na, nb);
  int ranges = 0;
  for (uint32_t i = 0; i < n;) {
    if (a[i] == b[i]) {
      i++;
      continue;
    }
    uint32_t j = i;
    while (j < n && a[j] != b[j]) j++;
    count += j - i;
    if (ranges++ < 12) out += fmt::format(" @{}+{}", i, j - i);
    i = j;
  }
  if (ranges > 12) out += fmt::format(" (+{} more ranges)", ranges - 12);
  if (na != nb) out += fmt::format(" (sizes {} and {})", na, nb);
  return out;
}

void OnTop(PPCContext& ctx, uint8_t* base) {
  if (game::BattleState() != game::kBattleFighting) {
    g_ring_frame.fill(-1);
    return;
  }
  const uint64_t f = frame::Count();
  const int slot = int(f % kRing);
  const auto save_t0 = std::chrono::steady_clock::now();
  const bool saved = battle::Save(ctx, base, g_ring[slot]) != 0;
  g_save_ms_total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - save_t0).count();
  if (!saved) {
    g_ring_frame[slot] = -1;
    return;
  }
  g_ring_frame[slot] = int64_t(f);
  input::KeepKeys(f);
  if (f % uint64_t(g_every)) return;
  if (g_back == 0) {
    // the round trip alone: put back what was just saved, save again, compare
    if (!battle::Restore(ctx, base, g_ring[slot])) {
      g_refused++;
      return;
    }
    battle::Save(ctx, base, g_check);
    g_tests++;
    const uint32_t want = battle::Size(g_ring[slot]), got = battle::Size(g_check);
    if (want != got || std::memcmp(battle::Data(g_ring[slot]), battle::Data(g_check), want) != 0) {
      g_failures++;
      if (g_logged++ < 20) {
        uint32_t count = 0;
        const std::string where = Differences(battle::Data(g_ring[slot]), want, battle::Data(g_check), got, count);
        REXLOG_WARN("sc5 synctest: fight frame {} round trip: {} bytes differ:{}", game::FightFrame(), count, where);
      }
    }
    if (g_tests % 300 == 0)
      REXLOG_INFO("sc5 synctest: {} round trips, {} differed, {} refused", g_tests, g_failures, g_refused);
    return;
  }
  const int back = int((f - uint64_t(g_back)) % kRing);
  if (g_ring_frame[back] != int64_t(f) - g_back) return;  // not that many frames of this fight saved yet
  const auto t0 = std::chrono::steady_clock::now();
  const int64_t wide_ff = game::FightFrame();
  const bool wide = dbg::WideWanted(wide_ff);  // TEMPORARY
  if (wide) dbg::WideBefore(ctx.r1.u32);
  if (!battle::Restore(ctx, base, g_ring[back])) {
    if (g_refused++ < 5) REXLOG_WARN("sc5 synctest: frame {}: the game would not take back frame {}", f, f - g_back);
    return;
  }
  frame::SetCount(f - uint64_t(g_back));
  input::KeysBack(f - uint64_t(g_back));
  sound::RollbackBegin(f - uint64_t(g_back));
  for (int i = 0; i < g_back; i++) frame::RunAgain(ctx, base);
  sound::RollbackEnd(ctx, base);
  if (wide) dbg::WideAfter(wide_ff);
  battle::Save(ctx, base, g_check);
  g_tests++;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_ms_total += ms;
  g_ms_worst = std::max(g_ms_worst, ms);
  const uint32_t want = battle::Size(g_ring[slot]), got = battle::Size(g_check);
  uint32_t raw_count = 0;
  const std::string raw_where = battle::RawDifferences(g_ring[slot], g_check, raw_count);
  const bool same =
      want == got && std::memcmp(battle::Data(g_ring[slot]), battle::Data(g_check), want) == 0 && raw_count == 0;
  static uint64_t raw_logged = 0;
  if (raw_count && raw_logged++ < 5)
    REXLOG_WARN("sc5 synctest: fight frame {}: {} bytes of the raw memory kept differ:{}", game::FightFrame(),
                raw_count, raw_where);
  // the game goes on from the frame as it ran again: that is what this frame's slot keeps from now on, so that the
  // next check runs a frame again from the state it first ran from
  std::swap(g_ring[slot], g_check);
  const bool snapshot_same = want == got && std::memcmp(battle::Data(g_check), battle::Data(g_ring[slot]), want) == 0;
  // TEMPORARY: the snapshots compared write by write (each block the game wrote, matched by where it came from, so a
  // block more or less in one does not shift the rest), the differences by kind: the players' poses (three buffers of
  // 6208 bytes at +0x3190, +0xB190, +0x13190), heap objects (effects such as the weapon's trail), the rest (the fight)
  if (!snapshot_same) {
    static uint64_t fight_checks = 0, fight_logged = 0, structure_logged = 0;
    const auto* wa = battle::Blocks(g_check);
    const auto* wb = battle::Blocks(g_ring[slot]);
    if (wa && wb) {
      const uint8_t* xa = battle::Data(g_check);
      const uint8_t* xb = battle::Data(g_ring[slot]);
      auto kind = [](uint32_t src) -> int {  // 0 pose, 1 heap or stack, 2 fight
        const battle::Kind k = battle::KindOf(src);
        return k == battle::Kind::Pose ? 0 : k == battle::Kind::Fight ? 2 : 1;
      };
      std::map<std::pair<uint32_t, uint32_t>, int> seen;  // (source, size) -> occurrences matched so far
      uint64_t bytes[3] = {}, unmatched = 0;
      std::string fight;
      std::vector<std::pair<uint32_t, uint32_t>> ranges;  // the fight's bytes that differ, as ranges (8 apart merged)
      for (const auto& w : *wb) {
        const int nth = seen[{w.source, w.size}]++;
        const battle::Block* m = nullptr;
        int k = 0;
        for (const auto& v : *wa)
          if (v.source == w.source && v.size == w.size && k++ == nth) {
            m = &v;
            break;
          }
        if (!m) {
          unmatched += w.size;
          continue;
        }
        for (uint32_t i = 0; i < w.size; i++) {
          if (m->pos + i >= want || w.pos + i >= got || xa[m->pos + i] == xb[w.pos + i]) continue;
          const int c = kind(w.source + i);
          bytes[c]++;
          if (c != 2) continue;
          const uint32_t at = w.source + i;
          if (!ranges.empty() && at >= ranges.back().first && at <= ranges.back().second + 8)
            ranges.back().second = std::max(ranges.back().second, at);
          else
            ranges.push_back({at, at});
        }
      }
      for (size_t r = 0; r < ranges.size() && r < 24; r++)
        fight += fmt::format(" {:08X}-{:08X}", ranges[r].first, ranges[r].second);
      if (ranges.size() > 24) fight += fmt::format(" (+{} ranges)", ranges.size() - 24);
      if (bytes[2]) {
        fight_checks++;
        if (fight_logged++ < 20)
          REXLOG_WARN("sc5 synctest: fight frame {}: {} bytes of the fight differ ({} pose, {} heap):{}",
                      game::FightFrame(), bytes[2], bytes[0], bytes[1], fight);
      }
      if (unmatched && structure_logged++ < 5)
        REXLOG_INFO("sc5 synctest: fight frame {}: {} bytes written in one run's snapshot only", game::FightFrame(),
                    unmatched);
      if (g_tests % 300 == 0 || fight_logged == 1)
        REXLOG_INFO("sc5 synctest: {} checks so far with differences in the fight itself", fight_checks);
    }
  }
  if (!snapshot_same) {
    g_snapshot_failures++;
  }
  if (!same) g_failures++;
  if (!snapshot_same) {
    // TEMPORARY: both snapshots of the first differences, to compare (SC5_TEST_DUMP_DIR)
    if (const char* dir = std::getenv("SC5_TEST_DUMP_DIR"); dir && *dir && g_snapshot_failures <= 3 &&
        !std::getenv("SC5_TEST_DUMP_OTHER")) {
      for (auto [name, stream, n] : {std::tuple{"first", g_check, want}, std::tuple{"again", g_ring[slot], got}}) {
        const std::string path = fmt::format("{}/synctest_{}_{}.bin", dir, game::FightFrame(), name);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(battle::Data(stream), 1, n, f);
          std::fclose(f);
        }
      }
    }
    if (g_logged++ < 20) {
      uint32_t count = 0;
      const std::string where = Differences(battle::Data(g_check), want, battle::Data(g_ring[slot]), got, count);
      REXLOG_WARN("sc5 synctest: fight frame {} after {} frames run again: {} bytes differ:{}", game::FightFrame(),
                  g_back, count, where);
    }
  }
  if (g_tests % 300 == 0)
    REXLOG_INFO("sc5 synctest: {} checks, {} differed ({} in the game's snapshot), {} refused; a rollback of {} "
                "frame(s) {:.2f} ms on average, {:.2f} ms at worst; a save {:.3f} ms", g_tests, g_failures,
                g_snapshot_failures, g_refused, g_back, g_ms_total / double(g_tests), g_ms_worst,
                g_save_ms_total / double(g_tests));
}

}  // namespace

void Init() {
  const char* v = std::getenv("SC5_SYNCTEST");
  if (!v || !*v) return;
  g_back = std::clamp(std::atoi(v), 0, kRing - 2);  // 0: the round trip alone
  if (const char* e = std::getenv("SC5_SYNCTEST_EVERY"); e && *e) g_every = std::max(1, std::atoi(e));
  for (auto& s : g_ring) s = battle::NewStream();
  g_check = battle::NewStream();
  if (!g_check || std::find(g_ring.begin(), g_ring.end(), 0u) != g_ring.end()) {
    REXLOG_ERROR("sc5 synctest: no guest memory for its snapshots");
    return;
  }
  g_ring_frame.fill(-1);
  frame::SetTopHook(&OnTop);
  REXLOG_INFO("sc5 synctest: every {} frame(s) of a fight put back {} frame(s) and run again", g_every, g_back);
}

}  // namespace sc5::synctest
