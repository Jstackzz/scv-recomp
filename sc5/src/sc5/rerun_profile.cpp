// The game's tasks timed one by one in frames run again (SC5_TEST_RERUN_PROFILE=1, tests).
//
// The step (sub_824A25C0) walks the game's task tree (the manager at [0x835B73D0], formed as 0x835B73C0 + 0x10; its first task at +12; a task's
// parent at +16, its links at +8 and +12) and calls each task's methods through its vtable (slots +0, +8, +12, +16,
// +24), which the recompiled code dispatches through the SDK's function table. Every method of the classes found in
// the tree has its table entry replaced with a wrapper of its own (a pool of template instances, one per method),
// which times it while a frame runs again (on the main thread; the time of the tasks it calls itself not counted)
// and calls the original. Calls by direct branches are not seen; only the step's (and other indirect) calls are.

#include "sc5/rerun_profile.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"

namespace sc5::rerun_profile {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kManagerAt = 0x835B73C0 + 16;
constexpr uint32_t kSlots[] = {0, 4, 8, 12, 16, 20, 24, 28};
constexpr size_t kPool = 1024;

struct Wrapped {
  uint32_t guest = 0, vtable = 0, slot = 0;
  PPCFunc* original = nullptr;
  double ms = 0;
  uint64_t calls = 0;
};
std::array<Wrapped, kPool> g_wrapped;
size_t g_count = 0;
std::unordered_map<uint32_t, size_t> g_by_guest;
std::unordered_set<uint32_t> g_vtables;
double g_child_ms = 0;  // main thread: the time the wrapped methods called by the one running took

template <size_t N>
void Wrapper(PPCContext& ctx, uint8_t* base) {
  Wrapped& w = g_wrapped[N];
  if (!sc5::frame::Replaying() || !sc5::frame::OnMainThread()) return w.original(ctx, base);
  const double outer_child = g_child_ms;
  g_child_ms = 0;
  const auto t0 = Clock::now();
  w.original(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  w.ms += ms - g_child_ms;
  w.calls++;
  g_child_ms = outer_child + ms;
}

template <size_t... I>
constexpr std::array<PPCFunc*, sizeof...(I)> MakePool(std::index_sequence<I...>) {
  return {&Wrapper<I>...};
}

// functions called directly (bl), timed by hooks of their own (SC5_RERUN_TIME below)
std::vector<Wrapped*>& Direct() {
  static std::vector<Wrapped*> list;
  return list;
}

}  // namespace

bool On();

namespace {

template <typename F>
void TimeDirect(Wrapped& w, F&& run) {
  if (!On() || !sc5::frame::Replaying() || !sc5::frame::OnMainThread()) return run();
  const double outer_child = g_child_ms;
  g_child_ms = 0;
  const auto t0 = Clock::now();
  run();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  w.ms += ms - g_child_ms;
  w.calls++;
  g_child_ms = outer_child + ms;
}
bool Register(Wrapped* w) {
  Direct().push_back(w);
  return true;
}

// SC5_TEST_RERUN_SKIP=<hex address>,...: those of the timed functions left out in frames run again (tests: what a
// frame run again can do without, checked with SC5_SYNCTEST)
bool Skipped(uint32_t guest) {
  static const std::unordered_set<uint32_t> skip = [] {
    std::unordered_set<uint32_t> out;
    if (const char* v = std::getenv("SC5_TEST_RERUN_SKIP")) {
      std::string s = v;
      size_t at = 0;
      while (at < s.size()) {
        const size_t comma = s.find(',', at);
        const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!item.empty()) out.insert(uint32_t(std::strtoul(item.c_str(), nullptr, 16)));
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
    }
    return out;
  }();
  return !skip.empty() && skip.count(guest) && sc5::frame::Replaying() && sc5::frame::OnMainThread();
}
const std::array<PPCFunc*, kPool> g_pool = MakePool(std::make_index_sequence<kPool>{});

// guest memory read only where the host has it committed
bool Readable(uint32_t address) {
  static std::unordered_map<uint32_t, bool> pages;
  const uint32_t page = address & ~0xFFFu;
  auto it = pages.find(page);
  if (it != pages.end()) return it->second;
  MEMORY_BASIC_INFORMATION mbi{};
  const auto* host = REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(page);
  const bool ok = VirtualQuery(host, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                  !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  pages[page] = ok;
  return ok;
}
bool Pointer(uint32_t a) { return a >= 0x40000000u && a < 0x90000000u && (a & 3) == 0 && Readable(a); }
uint32_t Rd(uint32_t a) {
  return __builtin_bswap32(*REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint32_t*>(a));
}

}  // namespace

bool On() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_TEST_RERUN_PROFILE");
    return v && v[0] == '1';
  }();
  return on;
}

void Scan() {
  if (!On()) return;
  auto* fd = REX_KERNEL_STATE()->function_dispatcher();
  if (!fd || !Pointer(kManagerAt)) return;
  const uint32_t manager = Rd(kManagerAt);
  if (!Pointer(manager)) return;
  std::vector<uint32_t> todo{Rd(manager + 12)};
  std::unordered_set<uint32_t> seen;
  size_t added = 0, tasks = 0;
  while (!todo.empty() && seen.size() < 50000) {
    const uint32_t n = todo.back();
    todo.pop_back();
    if (!Pointer(n) || !Pointer(n + 16) || !seen.insert(n).second) continue;
    const uint32_t vt = Rd(n);
    if (vt < 0x82000000u || vt >= 0x82A00000u || !Pointer(vt) || !fd->GetFunction(Rd(vt + 12))) continue;
    tasks++;
    if (g_vtables.insert(vt).second) {
      for (uint32_t slot : kSlots) {
        const uint32_t m = Rd(vt + slot);
        if (g_by_guest.count(m) || g_count >= kPool) continue;
        PPCFunc* original = fd->GetFunction(m);
        if (!original) continue;
        Wrapped& w = g_wrapped[g_count];
        w.guest = m;
        w.vtable = vt;
        w.slot = slot;
        w.original = original;
        if (!fd->SetFunction(m, g_pool[g_count])) continue;
        g_by_guest[m] = g_count++;
        added++;
      }
    }
    for (uint32_t link : {4u, 8u, 12u}) todo.push_back(Rd(n + link));
  }
  static bool told = false;
  if (added || !told) {
    told = true;
    const uint32_t first = Rd(manager + 12);
    REXLOG_INFO("sc5 rerun profile: {} tasks in the tree, {} classes, {} methods timed ({} new); manager {:08X}, first "
                "task {:08X} (vtable {:08X}), {} objects looked at",
                tasks, g_vtables.size(), g_count, added, manager, first, Pointer(first) ? Rd(first) : 0, seen.size());
  }
}

void Report(uint64_t frames) {
  if (!On() || !frames) return;
  std::vector<const Wrapped*> order;
  double total = 0;
  for (size_t i = 0; i < g_count; i++) {
    order.push_back(&g_wrapped[i]);
    total += g_wrapped[i].ms;
  }
  for (const Wrapped* w : Direct()) {
    order.push_back(w);
    total += w->ms;
  }
  std::sort(order.begin(), order.end(), [](const Wrapped* a, const Wrapped* b) { return a->ms > b->ms; });
  std::string top;
  for (size_t i = 0; i < order.size() && i < 30; i++) {
    const Wrapped& w = *order[i];
    if (w.ms <= 0) break;
    top += fmt::format("\n    {:08X} ({}) {:.3f} ms, {:.1f} calls", w.guest,
                       w.vtable ? fmt::format("vtable {:08X} +{}", w.vtable, w.slot) : std::string("called directly"),
                       w.ms / double(frames), double(w.calls) / double(frames));
  }
  REXLOG_INFO("sc5 rerun profile: the tasks' own time {:.3f} ms a frame run again ({} frames), the most:{}",
              total / double(frames), frames, top);
}

}  // namespace sc5::rerun_profile

// A function called directly, timed (and SC5_TEST_RERUN_SKIP) by a hook of its own:
#define SC5_RERUN_TIME(addr)                                                                         \
  REX_EXTERN(__imp__sub_##addr);                                                                     \
  namespace sc5::rerun_profile {                                                                     \
  namespace {                                                                                        \
  Wrapped g_direct_##addr{0x##addr, 0, 0, nullptr, 0, 0};                                            \
  const bool g_reg_##addr = Register(&g_direct_##addr);                                              \
  }                                                                                                  \
  }                                                                                                  \
  REX_HOOK_RAW(sub_##addr) {                                                                         \
    if (sc5::rerun_profile::Skipped(0x##addr)) return;                                               \
    sc5::rerun_profile::TimeDirect(sc5::rerun_profile::g_direct_##addr,                              \
                                   [&] { __imp__sub_##addr(ctx, base); });                           \
  }

// Example (tests): SC5_RERUN_TIME(822CBA00) on a line of its own here times that function in frames run again (the
// time of the timed functions it calls left out) and lets SC5_TEST_RERUN_SKIP leave it out there; a function hooked
// elsewhere already cannot take one. The pose was found this way (docs\GAME.md, "Where a frame run again spends its
// time"); none are timed in a normal build.
