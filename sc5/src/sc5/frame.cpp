// The frame boundary, see frame.h.

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/rerun_profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REX_EXTERN(__imp__sub_82163C00);  // one frame (frame.h)
REX_EXTERN(__imp__sub_8249B8A8);  // the frame's phase 0x200 (drawing)
REX_EXTERN(__imp__sub_8249E548);  // phase 0x400 (drawing)
REX_EXTERN(__imp__sub_824945F8);  // phase 0x500 (the frame's end: present, wait for the display)
REX_EXTERN(__imp__sub_824943F0);  // the frame's simulation: 0 to 2 steps of sub_82493230 (see below)

namespace sc5::frame {
namespace {

constexpr uint32_t kLoopTop = 0x82189268;  // the return address of main's call of the frame (0x82189264)

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_main_tid{0};
Hook g_top = nullptr;
Hook g_end = nullptr;
Hook g_menu = nullptr;
std::atomic<bool> g_replaying{false};

uint32_t CurrentTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

// the frame rate every ten seconds of frames, and the longest gap between two frames' starts meanwhile
struct Pacing {
  using Steady = std::chrono::steady_clock;
  Steady::time_point window_start{}, last{};
  Steady::duration longest{};
  uint32_t frames = 0, late = 0;

  void Top() {
    const auto now = Steady::now();
    if (window_start == Steady::time_point{}) window_start = now;
    if (last != Steady::time_point{}) {
      const auto gap = now - last;
      if (gap > std::chrono::milliseconds(20)) late++;
      if (gap > longest) longest = gap;
    }
    last = now;
    if (++frames == 600) {
      const double secs = std::chrono::duration<double>(now - window_start).count();
      REXLOG_INFO("sc5: {:.1f} frames/s over the last 600 (frame {}), {} later than 20 ms, longest gap {:.1f} ms",
                  frames / secs, g_frames.load(), late, std::chrono::duration<double, std::milli>(longest).count());
      window_start = now;
      frames = late = 0;
      longest = {};
    }
  }
};
Pacing g_pacing;

}  // namespace

uint64_t Count() { return g_frames.load(std::memory_order_relaxed); }
uint32_t MainThreadId() { return g_main_tid.load(std::memory_order_relaxed); }
bool OnMainThread() { return g_main_tid.load(std::memory_order_relaxed) == CurrentTid(); }
void SetTopHook(Hook hook) { g_top = hook; }
void SetEndHook(Hook hook) { g_end = hook; }
void SetMenuHook(Hook hook) { g_menu = hook; }
bool Replaying() { return g_replaying.load(std::memory_order_relaxed); }
void SetCount(uint64_t n) { g_frames.store(n, std::memory_order_relaxed); }

void ProfileRerun(double whole_ms);  // below, SC5_TEST_RERUN_PROFILE

void RunAgain(PPCContext& ctx, uint8_t* base) {
  const auto t0 = std::chrono::steady_clock::now();
  g_replaying.store(true, std::memory_order_relaxed);
  const uint64_t lr = ctx.lr;
  ctx.lr = kLoopTop;
  __imp__sub_82163C00(ctx, base);
  ctx.lr = lr;
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  sc5::game::OnFrameEnd(n);
  g_replaying.store(false, std::memory_order_relaxed);
  ProfileRerun(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

namespace {
std::mutex g_posted_lock;
std::vector<Hook> g_posted;
}  // namespace

void Post(Hook hook) {
  std::lock_guard<std::mutex> lock(g_posted_lock);
  if (std::find(g_posted.begin(), g_posted.end(), hook) == g_posted.end()) g_posted.push_back(hook);
}

void Top(PPCContext& ctx, uint8_t* base) {
  if (g_main_tid.load(std::memory_order_relaxed) == 0) {
    g_main_tid.store(CurrentTid());
    REXLOG_INFO("sc5: first frame on host thread {}", CurrentTid());
  }
  g_pacing.Top();
  std::vector<Hook> posted;
  {
    std::lock_guard<std::mutex> lock(g_posted_lock);
    posted.swap(g_posted);
  }
  for (Hook h : posted) h(ctx, base);
  if (g_menu) g_menu(ctx, base);
  if (g_top) g_top(ctx, base);
}

void End(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  sc5::game::OnFrameEnd(n);
  if (g_end) g_end(ctx, base);
}

}  // namespace sc5::frame

// one frame (main's loop calls it until it returns false)
REX_HOOK_RAW(sub_82163C00) {
  const bool from_main = uint32_t(ctx.lr) == sc5::frame::kLoopTop;
  if (from_main) sc5::frame::Top(ctx, base);
  __imp__sub_82163C00(ctx, base);
  if (from_main) sc5::frame::End(ctx, base);
}

// A frame run again draws nothing and presents nothing: the next frame the player sees is the one after it.
// SC5_REPLAY_DRAW=1 (tests): it draws (phases 0x200 and 0x400), and only the present is left out.
namespace {
bool ReplayDraws() {
  static const bool draws = [] { const char* v = std::getenv("SC5_REPLAY_DRAW"); return v && v[0] == '1'; }();
  return draws;
}
}  // namespace
REX_HOOK_RAW(sub_8249B8A8) {
  if (sc5::frame::Replaying() && !ReplayDraws()) return;
  __imp__sub_8249B8A8(ctx, base);
}
REX_HOOK_RAW(sub_8249E548) {
  if (sc5::frame::Replaying() && !ReplayDraws()) return;
  __imp__sub_8249E548(ctx, base);
}
REX_HOOK_RAW(sub_824945F8) {
  if (sc5::frame::Replaying()) return;
  __imp__sub_824945F8(ctx, base);
}

// Nor does it work out the models' pose (sub_822BCF88: a model's bones, which the main thread hands worker threads as
// some 54 jobs a fighter, then the cloth and hair and what hangs on the bones, such as the weapons; its callers do not
// read what it returns). That was two thirds of a frame run again: 1.1 of 1.75 ms (SC5_TEST_RERUN_PROFILE). Nothing
// in the fight reads it: with every fight frame put back 4 frames and run again without it (SC5_SYNCTEST=4), the
// fight's state stayed byte for byte the same but for the poses and the buffers the pose fills for the drawing
// (battle.cpp, KindOf), and the KO came on the same frame. The frame shown after a rollback works its pose out afresh;
// the cloth and hair go on from where the rollback's first frame had them. SC5_RERUN_POSE=1 works it out (tests).
REX_EXTERN(__imp__sub_822BCF88);
namespace {
bool RerunPose() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_RERUN_POSE");
    return v && v[0] == '1';
  }();
  return on;
}
}  // namespace
REX_HOOK_RAW(sub_822BCF88) {
  if (sc5::frame::Replaying() && !RerunPose() && sc5::frame::OnMainThread()) return;
  __imp__sub_822BCF88(ctx, base);
}

// One simulation step a frame. sub_824943F0 runs the game's step (sub_82493230: pads, sequences, the battle) once,
// and in a fight (0x82771B78 set) once more for each whole step the float at 0x82927D48 says the game is behind, at
// most 2, keeping the fraction for later; the present (sub_824945F8) sets that from the frame times it measured (16
// kept): below 1 at 60 frames a second, 1 or 2 after a late frame, so that the game catches up. With that, how far a
// fight gets in a number of frames follows the clock, and a frame run again (no present) takes the catching up meant
// for a later one: a stall of a few frames changes how a fight goes. Rollback and netplay need a frame to be one
// step, so it is set to 0 before each frame's simulation (a slow PC slows the game down instead). SC5_FRAME_CATCHUP=1
// leaves the game's own count (tests). The frames whose count was 1 or more are logged (the first 20).
namespace {
constexpr uint32_t kStepsBehind = 0x82927D48;
const bool g_catchup = [] {
  const char* v = std::getenv("SC5_FRAME_CATCHUP");
  return v && v[0] == '1';
}();
uint64_t g_behind = 0;
}  // namespace
// SC5_TEST_RERUN_PROFILE=1: where a frame run again spends its time: the pads (sub_82493BC8), the step (sub_824943F0)
// and the tasks (sub_82494CA0), each frame's whole run beside them; logged every 2000 frames run again
REX_EXTERN(__imp__sub_82493BC8);
REX_EXTERN(__imp__sub_82494CA0);
namespace {
const bool g_profile = [] {
  const char* v = std::getenv("SC5_TEST_RERUN_PROFILE");
  return v && v[0] == '1';
}();
double g_prof_ms[3] = {};
template <int kPhase, typename F>
void Timed(F&& run) {
  if (!g_profile || !sc5::frame::Replaying()) return run();
  const auto t0 = std::chrono::steady_clock::now();
  run();
  g_prof_ms[kPhase] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace
REX_HOOK_RAW(sub_82493BC8) { Timed<0>([&] { __imp__sub_82493BC8(ctx, base); }); }
REX_HOOK_RAW(sub_82494CA0) { Timed<2>([&] { __imp__sub_82494CA0(ctx, base); }); }
namespace sc5::frame {
void ProfileRerun(double whole_ms) {
  if (!g_profile) return;
  static double whole = 0;
  static uint64_t frames = 0;
  whole += whole_ms;
  if (++frames % 2000) return;
  REXLOG_INFO("sc5 frame profile: a frame run again {:.3f} ms: pads {:.3f}, step {:.3f}, tasks {:.3f}, the rest {:.3f} "
              "({} frames)",
              whole / double(frames), g_prof_ms[0] / double(frames), g_prof_ms[1] / double(frames),
              g_prof_ms[2] / double(frames), (whole - g_prof_ms[0] - g_prof_ms[1] - g_prof_ms[2]) / double(frames),
              frames);
  sc5::rerun_profile::Report(frames);
}
}  // namespace sc5::frame

void Sc5Step(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_824943F0) {
  if (g_profile && sc5::frame::Replaying()) {
    static uint64_t steps = 0;
    if (steps++ % 500 == 0) sc5::rerun_profile::Scan();  // classes that came since are timed too
  }
  Timed<1>([&] { Sc5Step(ctx, base); });
}
void Sc5Step(PPCContext& ctx, uint8_t* base) {
  if (auto* ks = REX_KERNEL_STATE()) {
    uint32_t* behind = ks->memory()->TranslateVirtual<uint32_t*>(kStepsBehind);
    const uint32_t bits = __builtin_bswap32(*behind);
    float steps;
    std::memcpy(&steps, &bits, 4);
    if (steps >= 1.0f && !sc5::frame::Replaying() && g_behind++ < 20)
      REXLOG_INFO("sc5: frame {}: the game would catch up {:.2f} steps{}", sc5::frame::Count(), steps,
                  g_catchup ? "" : " (not done)");
    if (!g_catchup) *behind = 0;
  }
  __imp__sub_824943F0(ctx, base);
}
