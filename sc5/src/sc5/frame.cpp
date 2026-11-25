// The frame boundary, see frame.h.

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/rerun_profile.h"
#include "sc5/rng.h"

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

REX_EXTERN(__imp__sub_82164070);  // one frame (frame.h)
REX_EXTERN(__imp__sub_824AAC38);  // the frame's phase 0x200 (drawing)
REX_EXTERN(__imp__sub_824AD398);  // phase 0x400 (drawing)
REX_EXTERN(__imp__sub_824A3988);  // phase 0x500 (the frame's end: present, wait for the display)
REX_EXTERN(__imp__sub_824A3780);  // the frame's simulation: 0 to 2 steps of sub_824A25C0 (see below)

namespace sc5::frame {
namespace {

constexpr uint32_t kLoopTop = 0x82189A40;  // the return address of main's call of the frame (0x82189A3C)

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

// The game's count of frames shown (0x82785644): the present adds one when the GPU took the frame, and the battle
// throttles by it (sub_821561B0 and others: "4 or more since the stamp"). A frame run again presents nothing, so after
// a rollback put the count back it stood still, and the throttles did otherwise than in the frames' first runs (the
// synctest's KO 188 frames earlier than without rollback, 2026-11-18): a frame run again counts as shown. (In a match
// the netplay sets it from the match's frame anyway, net/netplay.cpp.)
constexpr uint32_t kFramesShown = 0x82785644;

void RunAgain(PPCContext& ctx, uint8_t* base) {
  const auto t0 = std::chrono::steady_clock::now();
  g_replaying.store(true, std::memory_order_relaxed);
  const uint64_t lr = ctx.lr;
  ctx.lr = kLoopTop;
  __imp__sub_82164070(ctx, base);
  ctx.lr = lr;
  if (auto* ks = REX_KERNEL_STATE()) {
    uint32_t* shown = ks->memory()->TranslateVirtual<uint32_t*>(kFramesShown);
    *shown = __builtin_bswap32(__builtin_bswap32(*shown) + 1);
  }
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
REX_HOOK_RAW(sub_82164070) {
  const bool from_main = uint32_t(ctx.lr) == sc5::frame::kLoopTop;
  if (from_main) sc5::frame::Top(ctx, base);
  __imp__sub_82164070(ctx, base);
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
// The drawing and the present draw their random numbers from the visual states (sc5/rng.h), as the other visual
// systems do: a frame's first run draws and its rerun does not, so any number they took from the fight's generators
// would leave those wherever the frame's last run had (two games' TT800 and Mersenne Twister came apart under
// rollback, Elysium against Edge Master with 40 ms each way, 2026-11-18; net/netplay.cpp ReseedAtStateChange).
REX_HOOK_RAW(sub_824AAC38) {
  if (sc5::frame::Replaying() && !ReplayDraws()) return;
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_824AAC38(ctx, base);
  sc5::rng::LeaveVisual();
}
// ... but phase 0x400 runs when sub_824AD5B0 calls it: a request a task made of the drawing (sub_824ADEB8: set up,
// then waited for, then its answer read) waits there in a loop of phase 0x400 and a short sleep until the drawing has
// taken it. Left out in a frame run again, the request was never taken and the game waited for good (Elysium against
// Edge Master with 40 ms each way, fight frame 160 in a frame run again: sub_82361C70's task through sub_82363FC8 and
// sub_824ADF60, the main thread asleep in sub_824D8008 from then on, 2026-11-18).
constexpr uint32_t kDrawRequestWait = 0x824AD5F8;  // where sub_824AD5B0's call of phase 0x400 returns
REX_HOOK_RAW(sub_824AD398) {
  if (sc5::frame::Replaying() && !ReplayDraws() && uint32_t(ctx.lr) != kDrawRequestWait) return;
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_824AD398(ctx, base);
  sc5::rng::LeaveVisual();
}
REX_HOOK_RAW(sub_824A3988) {
  if (sc5::frame::Replaying()) return;
  sc5::rng::EnterVisual(ctx, base);
  __imp__sub_824A3988(ctx, base);
  sc5::rng::LeaveVisual();
}

// It does work out the models' pose (sub_822C9B38: a model's bones, which the main thread hands worker threads as some
// 54 jobs a fighter, then the cloth and hair and what hangs on the bones), though that is two thirds of a frame run
// again (1.1 of 1.75 ms, SC5_TEST_RERUN_PROFILE). Left out (2026-11-17), the fight stayed the same for Siegfried
// against Siegfried, but the next frame builds its skeleton from the pose before it (three buffers taken in turn,
// each player's +0x3190: the pose's last stage scales some bones, sub_822C44C8), and sub_82146688 takes an angle of
// bone 2 into the fight (+0x3070, its sectors at +0x3066 / +0x3068, which the move scripts read: sub_8215D210): after
// a frame run again without its pose, that bone came out a bit or two apart, by how far each game had rolled back
// (Aeon Calcos against Siegfried with 40 ms each way parted at frame 210, P1+0x3078). SC5_RERUN_POSE=0 leaves it out
// (tests: what it costs).
REX_EXTERN(__imp__sub_822C9B38);
namespace {
bool RerunPose() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_RERUN_POSE");
    return !(v && v[0] == '0');
  }();
  return on;
}
}  // namespace
REX_HOOK_RAW(sub_822C9B38) {
  if (sc5::frame::Replaying() && !RerunPose() && sc5::frame::OnMainThread()) return;
  __imp__sub_822C9B38(ctx, base);
}

// One simulation step a frame. sub_824A3780 runs the game's step (sub_824A25C0: pads, sequences, the battle) once,
// and in a fight (0x82785678 set) once more for each whole step the float at 0x8293B794 says the game is behind, at
// most 2, keeping the fraction for later; the present (sub_824A3988) sets that from the frame times it measured (16
// kept): below 1 at 60 frames a second, 1 or 2 after a late frame, so that the game catches up. With that, how far a
// fight gets in a number of frames follows the clock, and a frame run again (no present) takes the catching up meant
// for a later one: a stall of a few frames changes how a fight goes. Rollback and netplay need a frame to be one
// step, so it is set to 0 before each frame's simulation (a slow PC slows the game down instead). SC5_FRAME_CATCHUP=1
// leaves the game's own count (tests). The frames whose count was 1 or more are logged (the first 20).
namespace {
constexpr uint32_t kStepsBehind = 0x8293B794;
const bool g_catchup = [] {
  const char* v = std::getenv("SC5_FRAME_CATCHUP");
  return v && v[0] == '1';
}();
uint64_t g_behind = 0;
}  // namespace
// SC5_TEST_RERUN_PROFILE=1: where a frame run again spends its time: the pads (sub_824A2F58), the step (sub_824A3780)
// and the tasks (sub_824A4030), each frame's whole run beside them; logged every 2000 frames run again
REX_EXTERN(__imp__sub_824A2F58);
REX_EXTERN(__imp__sub_824A4030);
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
REX_HOOK_RAW(sub_824A2F58) { Timed<0>([&] { __imp__sub_824A2F58(ctx, base); }); }
REX_HOOK_RAW(sub_824A4030) { Timed<2>([&] { __imp__sub_824A4030(ctx, base); }); }
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
REX_HOOK_RAW(sub_824A3780) {
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
  __imp__sub_824A3780(ctx, base);
}

// Two blends of the post-effects run every frame as step tasks: the effects' parameters (sub_82353358 calls
// sub_823534F0 for each of two effects: the effect in r5, its start at +60, fade-in +48, hold +52, fade-out +56) and the
// colour grading (sub_82355F78 calls sub_82356060: start +56, fade-in +44, hold +48, fade-out +52). Each weighs an
// effect by elapsed = the frame count at 0x82785648 - its start: elapsed / fade-in while elapsed < fade-in, so with a
// fade-in of 0 a count behind the effect's start gives -inf, and NaN goes on into the blended parameters, the colour
// grading and the final picture's colour matrix: the scene black under the HUD until the effect is set again. On the
// console the count never goes back; here a rollback puts it back (battle.cpp's kRaw) while the post-effects, not in the
// snapshot, keep their starts, as did player 2's game taking player 1's counters at the battle setup (versus.cpp) and,
// before 0.1.3, at the barrier (net/netplay.cpp, CountsUp). An effect whose start is still to come weighs nothing yet:
// for the call its start is now and its fade-in at least 1 (0 / 1), then both as they were (neither blend writes
// them). Drawing only: nothing in the fight reads the post-effects.
namespace {
template <class Blend>
void BlendFromNow(PPCContext& ctx, uint32_t start_at, uint32_t fade_in_at, Blend blend) {
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint32_t effect = ctx.r5.u32;
  uint32_t* start = mem->TranslateVirtual<uint32_t*>(effect + start_at);
  uint32_t* fade_in = mem->TranslateVirtual<uint32_t*>(effect + fade_in_at);
  const uint32_t now = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(0x82785648));
  const uint32_t was_start = __builtin_bswap32(*start), was_fade_in = __builtin_bswap32(*fade_in);
  const bool ahead = int32_t(now - was_start) < 0;
  if (ahead) {
    static int told = 0;
    if (told++ < 20 || int32_t(was_fade_in) < 1)
      REXLOG_INFO("sc5: frame {}: a post-effect ({:X}) starts {} frames past the count, fade-in {}: it weighs nothing yet",
                  sc5::frame::Count(), start_at, int32_t(was_start - now), int32_t(was_fade_in));
    *start = __builtin_bswap32(now);
    if (int32_t(was_fade_in) < 1) *fade_in = __builtin_bswap32(1u);
  }
  blend();
  if (ahead) {
    *start = __builtin_bswap32(was_start);
    *fade_in = __builtin_bswap32(was_fade_in);
  }
}
}  // namespace
REX_EXTERN(__imp__sub_823534F0);
REX_HOOK_RAW(sub_823534F0) { BlendFromNow(ctx, 60, 48, [&] { __imp__sub_823534F0(ctx, base); }); }
REX_EXTERN(__imp__sub_82356060);
REX_HOOK_RAW(sub_82356060) { BlendFromNow(ctx, 56, 44, [&] { __imp__sub_82356060(ctx, base); }); }
