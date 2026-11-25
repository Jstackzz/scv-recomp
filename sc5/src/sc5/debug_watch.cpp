// Temporary rollback diagnostics.
//  - SC5_DBG_POS=<a>,<b>,...: which write puts a snapshot's positions, and from where (first 3 times each).
//  - SC5_DBG_WATCH=<hex guest address>[:<len 1,2,4,8>],... (up to 4) with SC5_DBG_WATCH_FROM/TO=<fight frames>:
//    the CPU's debug registers on the main thread catch every write to those addresses in that window; each is
//    logged with the guest function that wrote it (the nearest one in the function table) and its callers.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/creation.h"
#include "sc5/frame.h"
#include "sc5/gameimage.h"
#include "sc5/net/netplay.h"
#include "sc5/game.h"
#include "sc5/rng.h"
#include "sc5/select.h"

namespace {
uint32_t Rd(uint32_t a) {
  auto* ks = REX_KERNEL_STATE();
  return __builtin_bswap32(*ks->memory()->TranslateVirtual<const uint32_t*>(a));
}
std::vector<uint32_t> Positions() {
  std::vector<uint32_t> out;
  if (const char* v = std::getenv("SC5_DBG_POS"))
    for (const char* p = v; *p;) {
      out.push_back(uint32_t(std::strtoul(p, const_cast<char**>(&p), 10)));
      while (*p == ',') p++;
    }
  return out;
}
std::map<uint32_t, int> g_seen;

// --- hardware write watch ---
struct Watch {
  uint32_t guest, len;
  uintptr_t host;
};
std::vector<Watch> g_watches;
int64_t g_from = 0, g_to = -1;
bool g_watch_read = false, g_armed = false;
int g_hits = 0;
std::vector<std::pair<uintptr_t, uint32_t>> g_funcs;  // host start -> guest address, sorted
uintptr_t g_funcs_end = 0;

void LoadFuncs() {
  for (const PPCFuncMapping* m = sc5::GameImage()->func_mappings; m->host; m++)
    g_funcs.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
  std::sort(g_funcs.begin(), g_funcs.end());
  if (!g_funcs.empty()) g_funcs_end = g_funcs.back().first + 0x10000;
}

std::string GuestOf(uintptr_t host) {
  if (g_funcs.empty() || host < g_funcs.front().first || host > g_funcs_end) return fmt::format("host+{:X}", host);
  auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), std::make_pair(host, UINT32_MAX));
  --it;
  return fmt::format("{:08X}+{:X}", it->second, host - it->first);
}

// a read watch leaves out the battle's snapshots that the netplay keeps and puts back (they read every word: battle.cpp)
bool g_quiet = false;
LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = e->ContextRecord;
  const DWORD64 dr6 = c->Dr6;
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  // the handler's own reads of the watched words (Rd below) come here again: let them through
  static thread_local bool inside = false;
  if ((g_quiet && g_watch_read) || inside) {
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  inside = true;
  if (g_hits++ < 600) {
    std::string chain;
    CONTEXT walk = *c;
    for (int i = 0; i < 8; i++) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
      if (f) {
        void* data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, f, &walk, &data, &establisher, nullptr);
      } else {
        walk.Rip = *reinterpret_cast<DWORD64*>(walk.Rsp);
        walk.Rsp += 8;
      }
      if (!walk.Rip) break;
      chain += " <- " + GuestOf(walk.Rip);
    }
    for (size_t i = 0; i < g_watches.size(); i++) {
      if (!(dr6 & (1ull << i))) continue;
      const Watch& w = g_watches[i];
      const uint32_t aligned = w.guest & ~3u;
      REXLOG_WARN("sc5 dbg: watch {:08X} frame {} fight {}{} now {:08X} {:08X} by {}{}", w.guest, sc5::frame::Count(),
                  sc5::game::FightFrame(), sc5::frame::Replaying() ? " (again)" : "", Rd(aligned), Rd(aligned + 4),
                  GuestOf(c->Rip), chain);
    }
  }
  c->Dr6 = 0;
  inside = false;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Debug registers cannot be set on the running thread itself: a helper thread suspends it, sets them, resumes it.
struct DrRequest {
  DWORD tid;
  bool on;
  bool ok;
};

DWORD WINAPI DrSetter(void* p) {
  auto* r = static_cast<DrRequest*>(p);
  HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, r->tid);
  if (!t) return 1;
  if (SuspendThread(t) != DWORD(-1)) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
      c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
      c.Dr7 = 0;
      if (r->on) {
        for (size_t i = 0; i < g_watches.size() && i < 4; i++) {
          const Watch& w = g_watches[i];
          const uint64_t len_bits = w.len == 1 ? 0 : w.len == 2 ? 1 : w.len == 8 ? 2 : 3;
          const uint64_t rw_bits = g_watch_read ? 3 : 1;  // 3: reads and writes, 1: writes
          switch (i) {
            case 0: c.Dr0 = w.host; break;
            case 1: c.Dr1 = w.host; break;
            case 2: c.Dr2 = w.host; break;
            case 3: c.Dr3 = w.host; break;
          }
          c.Dr7 |= (1ull << (i * 2)) | (rw_bits << (16 + i * 4)) | (len_bits << (18 + i * 4));
        }
      }
      c.Dr6 = 0;
      r->ok = SetThreadContext(t, &c) != 0;
    }
    ResumeThread(t);
  }
  CloseHandle(t);
  return 0;
}

void SetDebugRegisters(bool on) {
  DrRequest r{GetCurrentThreadId(), on, false};
  HANDLE h = CreateThread(nullptr, 0, &DrSetter, &r, 0, nullptr);
  if (!h) return;
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  REXLOG_INFO("sc5 dbg: watch {} at fight frame {}{}", on ? "armed" : "off", sc5::game::FightFrame(),
              r.ok ? "" : " (could not set the debug registers)");
}

bool WatchInit() {
  static int state = -1;
  if (state >= 0) return state == 1;
  state = 0;
  const char* v = std::getenv("SC5_DBG_WATCH");
  if (!v || !*v) return false;
  for (const char* p = v; *p && g_watches.size() < 4;) {
    Watch w{};
    w.guest = uint32_t(std::strtoul(p, const_cast<char**>(&p), 16));
    w.len = 4;
    if (*p == ':') w.len = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 10));
    w.guest &= ~(w.len - 1);
    w.host = reinterpret_cast<uintptr_t>(REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(w.guest));
    g_watches.push_back(w);
    while (*p == ',') p++;
  }
  if (const char* f = std::getenv("SC5_DBG_WATCH_FROM")) g_from = std::atoll(f);
  if (const char* t = std::getenv("SC5_DBG_WATCH_TO")) g_to = std::atoll(t);
  if (const char* r = std::getenv("SC5_DBG_WATCH_READS")) g_watch_read = r[0] == '1';
  LoadFuncs();
  AddVectoredExceptionHandler(1, &OnException);
  REXLOG_INFO("sc5 dbg: watching {} address(es) from fight frame {} to {}", g_watches.size(), g_from, g_to);
  state = 1;
  return true;
}
}  // namespace

namespace sc5::dbg {
void QuietWatch(bool on) { g_quiet = on; }
// --- wide diff (SC5_DBG_WIDEDIFF=<fight frame>[,<fight frame>...], SC5_DBG_WIDEDIFF_OUT=<file>): at the top of that
// fight frame, guest memory (0x40000000-0xC0000000, committed) before the synctest's rollback and after it ran the
// frame again; what differs is written to the file.
namespace {
struct Span {
  uint32_t guest, size;
};
std::vector<Span> g_spans;
std::vector<uint8_t> g_before;
std::vector<int64_t> g_wide_frames;
bool g_wide_init = false;
uint32_t g_wide_r1 = 0;
}  // namespace

bool WideWanted(int64_t fight_frame) {
  if (!g_wide_init) {
    g_wide_init = true;
    if (const char* v = std::getenv("SC5_DBG_WIDEDIFF"))
      for (const char* p = v; *p;) {
        g_wide_frames.push_back(std::strtoll(p, const_cast<char**>(&p), 10));
        while (*p == ',') p++;
      }
  }
  return std::find(g_wide_frames.begin(), g_wide_frames.end(), fight_frame) != g_wide_frames.end();
}

void WideBefore(uint32_t r1) {
  g_wide_r1 = r1;
  g_spans.clear();
  g_before.clear();
  uint8_t* membase = REX_KERNEL_STATE()->memory()->virtual_membase();
  for (uint64_t a = 0x40000000; a < 0xC0000000;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(membase + a, &mbi, sizeof(mbi))) break;
    const uint64_t start = reinterpret_cast<uint8_t*>(mbi.BaseAddress) - membase;
    uint64_t end = start + mbi.RegionSize;
    if (end > 0xC0000000) end = 0xC0000000;
    const DWORD prot = mbi.Protect & 0xFF;
    const bool readable = prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_EXECUTE_READWRITE ||
                          prot == PAGE_EXECUTE_READ || prot == PAGE_WRITECOPY;
    if (mbi.State == MEM_COMMIT && readable && !(mbi.Protect & PAGE_GUARD) && start >= a)
      g_spans.push_back({uint32_t(start), uint32_t(end - start)});
    a = end > a ? end : a + 0x1000;
  }
  size_t total = 0;
  for (const Span& sp : g_spans) total += sp.size;
  g_before.resize(total);
  size_t at = 0;
  for (const Span& sp : g_spans) {
    std::memcpy(g_before.data() + at, membase + sp.guest, sp.size);
    at += sp.size;
  }
  REXLOG_INFO("sc5 dbg: wide diff: kept {} MB in {} spans (guest r1 {:08X})", total >> 20, g_spans.size(), r1);
}

void WideAfter(int64_t fight_frame) {
  uint8_t* membase = REX_KERNEL_STATE()->memory()->virtual_membase();
  const char* outp = std::getenv("SC5_DBG_WIDEDIFF_OUT");
  const std::string path = std::string(outp ? outp : "widediff") + "_" + std::to_string(fight_frame) + ".txt";
  FILE* f = std::fopen(path.c_str(), "w");
  size_t at = 0, ranges = 0, bytes = 0;
  std::map<uint32_t, size_t> per_block;  // 64 KB block -> bytes differing
  for (const Span& sp : g_spans) {
    const uint8_t* a = g_before.data() + at;
    const uint8_t* b = membase + sp.guest;
    for (uint32_t i = 0; i < sp.size;) {
      if (a[i] == b[i]) {
        i++;
        continue;
      }
      uint32_t j = i + 1;
      // a range ends after 16 equal bytes
      for (uint32_t same = 0; j < sp.size && same < 16; j++) same = a[j] == b[j] ? same + 1 : 0;
      while (j > i + 1 && a[j - 1] == b[j - 1]) j--;
      const uint32_t g = sp.guest + i, n = j - i;
      ranges++;
      bytes += n;
      per_block[g & 0xFFFF0000u] += n;
      if (f && ranges <= 20000) {
        std::fprintf(f, "%08X %u", g, n);
        if (n <= 256) {
          std::fprintf(f, " before ");
          for (uint32_t k = 0; k < n; k++) std::fprintf(f, "%02X", a[i + k]);
          std::fprintf(f, " after ");
          for (uint32_t k = 0; k < n; k++) std::fprintf(f, "%02X", b[i + k]);
        }
        std::fprintf(f, "\n");
      }
      i = j;
    }
    at += sp.size;
  }
  if (f) {
    std::fprintf(f, "# per 64 KB block\n");
    for (auto& [blk, n] : per_block) std::fprintf(f, "# %08X %zu\n", blk, n);
    std::fclose(f);
  }
  REXLOG_INFO("sc5 dbg: wide diff at fight frame {}: {} ranges, {} bytes differ ({} blocks) -> {}", fight_frame, ranges,
              bytes, per_block.size(), path);
  g_before.clear();
  g_before.shrink_to_fit();
}

// SC5_DBG_PEEK=<frame>:<hex address>:<bytes>,...: guest memory logged at the end of those frames (absolute)
void Peek() {
  struct Item {
    uint64_t frame;
    uint32_t address, size;
  };
  static const std::vector<Item> items = [] {
    std::vector<Item> out;
    if (const char* v = std::getenv("SC5_DBG_PEEK"))
      for (const char* p = v; *p;) {
        Item it{};
        it.frame = std::strtoull(p, const_cast<char**>(&p), 10);
        if (*p == ':') it.address = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 16));
        if (*p == ':') it.size = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 10));
        out.push_back(it);
        while (*p == ',') p++;
      }
    return out;
  }();
  const uint64_t now = sc5::frame::Count();
  for (const Item& it : items) {
    if (it.frame != now || !it.size) continue;
    const uint8_t* q = REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(it.address);
    std::string hex;
    for (uint32_t i = 0; i < it.size && i < 256; i++) hex += fmt::format("{:02X}{}", q[i], (i % 4 == 3) ? " " : "");
    REXLOG_INFO("sc5 dbg: frame {} {:08X}: {}", now, it.address, hex);
  }
}

// TEMPORARY: the select screen object (set by its setup functions, hooked below) and SC5_DBG_SELDUMP=<frame>,...: its
// first 0x1000 bytes written into SC5_TEST_DUMP_DIR as sel_<frame>.bin at the end of those frames (absolute);
// SC5_DBG_MEMDUMP=<frame>:<hex address>:<hex size>,...: any guest memory the same way (mem_<frame>_<address>.bin)
void Dumps() {
  const uint32_t g_select_obj = sc5::select::Object();
  struct Item {
    uint64_t frame;
    uint32_t address, size;
  };
  static const std::vector<Item> items = [] {
    std::vector<Item> out;
    if (const char* v = std::getenv("SC5_DBG_SELDUMP"))
      for (const char* p = v; *p;) {
        out.push_back({std::strtoull(p, const_cast<char**>(&p), 10), 0, 0x2000});
        while (*p == ',') p++;
      }
    if (const char* v = std::getenv("SC5_DBG_MEMDUMP"))
      for (const char* p = v; *p;) {
        Item it{};
        it.frame = std::strtoull(p, const_cast<char**>(&p), 10);
        if (*p == ':') it.address = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 16));
        if (*p == ':') it.size = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 16));
        out.push_back(it);
        while (*p == ',') p++;
      }
    return out;
  }();
  // SC5_DBG_SPAN=<a>:<b>,...: every byte of guest memory that differs between the ends of frames a and b (absolute),
  // as the wide diff writes it (SC5_DBG_WIDEDIFF_OUT_<b>.txt)
  static const std::vector<std::pair<uint64_t, uint64_t>> spans = [] {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    if (const char* v = std::getenv("SC5_DBG_SPAN"))
      for (const char* p = v; *p;) {
        const uint64_t a = std::strtoull(p, const_cast<char**>(&p), 10);
        uint64_t b = a + 1;
        if (*p == ':') b = std::strtoull(p + 1, const_cast<char**>(&p), 10);
        out.emplace_back(a, b);
        while (*p == ',') p++;
      }
    return out;
  }();
  for (auto [a, b] : spans) {
    if (sc5::frame::Count() == a) WideBefore(0);
    if (sc5::frame::Count() == b) WideAfter(int64_t(b));
  }
  // SC5_DBG_SLOTDUMP=<frame>:<slot>,...: a creation slot of the save image (its header, record and picture; slots from
  // 0) written into SC5_TEST_DUMP_DIR as slot<slot>_<frame>.bin at the end of those frames (absolute)
  static const std::vector<std::pair<uint64_t, uint32_t>> slots = [] {
    std::vector<std::pair<uint64_t, uint32_t>> out;
    if (const char* v = std::getenv("SC5_DBG_SLOTDUMP"))
      for (const char* p = v; *p;) {
        const uint64_t f = std::strtoull(p, const_cast<char**>(&p), 10);
        uint32_t n = 0;
        if (*p == ':') n = uint32_t(std::strtoul(p + 1, const_cast<char**>(&p), 10));
        out.emplace_back(f, n);
        while (*p == ',') p++;
      }
    return out;
  }();
  for (auto [f, n] : slots) {
    const char* dir = std::getenv("SC5_TEST_DUMP_DIR");
    const uint32_t slot = sc5::creation::Slot(n);
    if (sc5::frame::Count() != f || !dir || !slot) continue;
    const std::string path = fmt::format("{}/slot{}_{}.bin", dir, n, f);
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      std::fwrite(REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(slot), 1, sc5::creation::kSlotSize, fp);
      std::fclose(fp);
      REXLOG_INFO("sc5 dbg: creation slot {} written to {}", n, path);
    }
  }
  if (items.empty()) return;
  const uint64_t now = sc5::frame::Count();
  const char* dir = std::getenv("SC5_TEST_DUMP_DIR");
  for (const Item& it : items) {
    if (it.frame != now) continue;
    const uint32_t a = it.address ? it.address : g_select_obj;
    if (!a) continue;
    const std::string path = fmt::format("{}/{}_{}{}.bin", dir ? dir : ".", it.address ? "mem" : "sel", now,
                                         it.address ? fmt::format("_{:08X}", a) : std::string());
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint8_t*>(a), 1, it.size, f);
      std::fclose(f);
    }
    REXLOG_INFO("sc5 dbg: frame {}: {:08X} ({} bytes) -> {}", now, a, it.size, path);
  }
}

// SC5_DBG_CHEAT=<a>:<b>:<c>:<d> (fight frames, ends of frames' first runs): words and bytes of guest memory
// that went 0 -> 1 from a to b, then were 1 at c and 2 at d (a counter of rounds, say)
void Cheat() {
  static const std::vector<int64_t> at = [] {
    std::vector<int64_t> out;
    if (const char* v = std::getenv("SC5_DBG_CHEAT"))
      for (const char* p = v; *p;) {
        out.push_back(std::strtoll(p, const_cast<char**>(&p), 10));
        while (*p == ':') p++;
      }
    return out;
  }();
  if (at.size() != 4 || sc5::frame::Replaying()) return;
  struct Cand { uint32_t a; int w; uint32_t c; };
  static std::vector<Cand> cands;
  static int64_t last = -1;
  const int64_t ff = sc5::game::FightFrame();
  if (ff == last) return;
  last = ff;
  uint8_t* mb = REX_KERNEL_STATE()->memory()->virtual_membase();
  auto val = [&](uint32_t a, int w) -> uint32_t {
    if (w == 1) return mb[a];
    if (w == 2) return (uint32_t(mb[a]) << 8) | mb[a + 1];
    return (uint32_t(mb[a]) << 24) | (uint32_t(mb[a + 1]) << 16) | (uint32_t(mb[a + 2]) << 8) | mb[a + 3];
  };
  auto num = [](uint32_t v, int w) -> int {  // 0, 1, 2 as an integer or a float (w 5: float)
    if (w == 5) { float f; std::memcpy(&f, &v, 4); return f == 0.f ? 0 : f == 1.f ? 1 : f == 2.f ? 2 : -1; }
    return v <= 2 ? int(v) : -1;
  };
  if (ff == at[0]) { WideBefore(0); REXLOG_INFO("sc5 dbg: cheat kept at {}", ff); }
  if (ff == at[1]) {
    size_t o = 0;
    for (const Span& sp : g_spans) {
      const uint8_t* old = g_before.data() + o;
      for (uint32_t i = 0; i + 4 <= sp.size; i++) {
        const uint32_t g = sp.guest + i;
        const uint8_t* n = mb + g;
        if (old[i] == 0 && n[0] == 1) cands.push_back({g, 1, 0});
        if ((i & 1) == 0 && old[i] == 0 && old[i + 1] == 0 && n[0] == 0 && n[1] == 1) cands.push_back({g, 2, 0});
        if ((i & 3) == 0) {
          const uint32_t ov = (uint32_t(old[i]) << 24) | (uint32_t(old[i + 1]) << 16) | (uint32_t(old[i + 2]) << 8) | old[i + 3];
          const uint32_t nv = val(g, 4);
          if (ov == 0 && nv == 1) cands.push_back({g, 4, 0});
          if (ov == 0 && nv == 0x3F800000u) cands.push_back({g, 5, 0});
        }
      }
      o += sp.size;
    }
    g_before.clear();
    g_before.shrink_to_fit();
    REXLOG_INFO("sc5 dbg: cheat at {}: {} candidates went 0 -> 1", ff, cands.size());
  }
  if (ff == at[2]) {
    size_t k = 0;
    for (Cand& c : cands) { c.c = val(c.a, c.w == 5 ? 4 : c.w); if (num(c.c, c.w) == 1) cands[k++] = c; }
    cands.resize(k);
    REXLOG_INFO("sc5 dbg: cheat at {}: {} still 1", ff, cands.size());
  }
  if (ff == at[3]) {
    int shown = 0;
    for (const Cand& c : cands) {
      const uint32_t v = val(c.a, c.w == 5 ? 4 : c.w);
      if (num(v, c.w) == 2 && shown++ < 400) REXLOG_INFO("sc5 dbg: cheat {:08X} w{} 1 -> 2", c.a, c.w);
    }
    REXLOG_INFO("sc5 dbg: cheat at {}: {} went 1 -> 2", ff, shown);
    cands.clear();
  }
}

// end of each frame, main thread: arms the watch for the next frame when it is in the window
void OnFrameEnd() {
  Cheat();
  Peek();
  Dumps();
  if (!WatchInit()) return;
  static const bool abs_frames = [] { const char* v = std::getenv("SC5_DBG_WATCH_ABS"); return v && v[0] == '1'; }();
  const int64_t ff = abs_frames ? int64_t(sc5::frame::Count()) : sc5::game::FightFrame();
  const bool want = ff >= g_from && ff < g_to;
  if (want != g_armed) {
    SetDebugRegisters(want);
    g_armed = want;
  }
}
}  // namespace sc5::dbg

namespace sc5::battle {
void OnSnapWrite(uint32_t stream, uint32_t pos, uint32_t size, uint32_t source);
}

void Sc5DbgSnapWrite(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  sc5::battle::OnSnapWrite(r3.u32, Rd(r3.u32 + 4), r5.u32, r4.u32);
  static const std::vector<uint32_t> want = Positions();
  if (want.empty() || Rd(r3.u32) != 0x820BA908) return;
  const uint32_t pos = Rd(r3.u32 + 4), size = r5.u32;
  for (uint32_t w : want) {
    if (w < pos || w >= pos + size) continue;
    if (g_seen[w]++ >= 3) continue;
    const uint32_t lr = Rd(r1.u32 + 0x70 - 8);
    const uint32_t back = Rd(r1.u32);
    const uint32_t lr2 = back ? Rd(back - 8) : 0;
    const uint32_t back2 = back ? Rd(back) : 0;
    const uint32_t lr3 = back2 ? Rd(back2 - 8) : 0;
    REXLOG_WARN("sc5 dbg: snapshot @{} = guest {:08X} (+{} of a {}-byte write at {}) by {:08X} <- {:08X} <- {:08X}", w,
                r4.u32 + (w - pos), w - pos, size, pos, lr, lr2, lr3);
  }
}

// SC5_DBG_DLC=1: the DLC manager's package table (sub_82396BC8: this+8 -> +40 an array of 344-byte package objects,
// +44 their count; an installed package is taken by the object whose wide name at +40 its display name contains),
// each package whose index was read (sub_82398158), and SC5_DBG_DLC_IDS=<hex>,..: the DLC check of those release ids
// (sub_82394D40) the first times it is asked.
namespace {
bool DlcDebug() {
  static const bool on = [] { const char* v = std::getenv("SC5_DBG_DLC"); return v && v[0] == '1'; }();
  return on;
}
std::string Wide(uint32_t a, int max = 64) {
  std::string s;
  auto* m = REX_KERNEL_STATE()->memory();
  for (int i = 0; i < max; i++) {
    const uint16_t c = __builtin_bswap16(*m->TranslateVirtual<const uint16_t*>(a + 2 * i));
    if (!c) break;
    s += c < 0x80 ? char(c) : '?';
  }
  return s;
}
std::vector<uint32_t> DlcIds() {
  std::vector<uint32_t> out;
  if (const char* v = std::getenv("SC5_DBG_DLC_IDS"))
    for (const char* p = v; *p;) {
      out.push_back(uint32_t(std::strtoul(p, const_cast<char**>(&p), 16)));
      while (*p == ',') p++;
    }
  return out;
}
}  // namespace

REX_EXTERN(__imp__sub_82394D40);
REX_EXTERN(__imp__sub_82396BC8);
REX_HOOK_RAW(sub_82396BC8) {
  if (!DlcDebug()) {
    __imp__sub_82396BC8(ctx, base);
    return;
  }
  const uint32_t t = Rd(ctx.r3.u32 + 8);
  REXLOG_INFO("sc5 dbg dlc: manager {:08X}, table {:08X}: state {}, {} packages at {:08X}", ctx.r3.u32, t, Rd(t + 8),
              Rd(t + 44), Rd(t + 40));
  __imp__sub_82396BC8(ctx, base);
  // the packages taken (the ids are checked once the game first asks: the manager is ready then, sub_82394D40 below)
  const uint32_t n = Rd(t + 44), arr = Rd(t + 40);
  for (uint32_t i = 0; i < n && i < 64; i++) {
    const uint32_t o = arr + i * 344;
    if (Rd(o + 340)) REXLOG_INFO("sc5 dbg dlc:   #{} '{}' mask {:08X}", i, Wide(o + 40), Rd(o + 340));
  }
}

REX_EXTERN(__imp__sub_82398158);
REX_HOOK_RAW(sub_82398158) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82398158(ctx, base);
  if (DlcDebug())
    REXLOG_INFO("sc5 dbg dlc: index of '{}' read: {} (mask {:08X})", Wide(obj + 40), ctx.r3.u32, Rd(obj + 340));
}

REX_EXTERN(sub_8246B318);  // the release state of an id (unlocks.cpp's hook first)

// SC5_DBG_RELEASE_OUT=<file>: once the DLC manager is ready and the save is in, every release id's raw state in the
// release table ([[0x8274EF10]] + 0x22C248, 4 bits each), the DLC check's answer and what sub_8246B318 answers (the
// hook of unlocks.cpp included): "id raw granted answer", one id a line
namespace {
void DumpRelease(PPCContext& keep, uint8_t* base) {
  static bool done = false;
  const char* path = std::getenv("SC5_DBG_RELEASE_OUT");
  if (done || !path || !*path) return;
  const uint32_t save = Rd(0x8274EF10);
  if (!save || Rd(save + 0x48) != 0x00010000) return;
  done = true;
  const uint32_t table = save + 0x22C248;
  auto* m = REX_KERNEL_STATE()->memory();
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  for (uint32_t id = 0; id < 0x1D02; id++) {
    const uint8_t byte = *m->TranslateVirtual<const uint8_t*>(table + (id >> 1));
    const uint32_t raw = (id & 1) ? byte >> 4 : byte & 0xF;
    PPCContext c = keep;
    c.r4.u64 = id;
    __imp__sub_82394D40(c, base);
    const uint32_t granted = c.r3.u32;
    c = keep;
    c.r3.u64 = table;
    c.r4.u64 = id;
    sub_8246B318(c, base);
    std::fprintf(f, "%04X %X %u %X\n", id, raw, granted, c.r3.u32);
  }
  std::fclose(f);
  REXLOG_INFO("sc5 dbg dlc: release table written to {}", path);
}
}  // namespace

REX_HOOK_RAW(sub_82394D40) {
  if (!DlcDebug()) {
    __imp__sub_82394D40(ctx, base);
    return;
  }
  const uint32_t id = ctx.r4.u32 & 0xFFFF, table = ctx.r3.u32;
  PPCContext keep = ctx;
  __imp__sub_82394D40(ctx, base);
  if (Rd(table + 8) >= 7 && Rd(table + 8) <= 11) DumpRelease(keep, base);
  static int calls = 0;
  if (calls++ < 5) REXLOG_INFO("sc5 dbg dlc: the game asked about id {:X}: {} (state {})", id, ctx.r3.u32, Rd(table + 8));
  // once the manager is ready: the ids asked for
  static bool checked = false;
  const uint32_t state = Rd(table + 8);
  if (checked || state < 7 || state > 11) return;
  checked = true;
  int yes = 0, no = 0;
  for (uint32_t want : DlcIds()) {
    PPCContext c = keep;
    c.r4.u64 = want;
    __imp__sub_82394D40(c, base);
    REXLOG_INFO("sc5 dbg dlc: id {:X} granted: {}", want, c.r3.u32);
    (c.r3.u32 ? yes : no)++;
  }
  REXLOG_INFO("sc5 dbg dlc: {} granted, {} not", yes, no);
}

// SC5_DBG_PICLOAD=1: every picture file the game loads (a created character's card, docs\GAME.md "The VS screen's
// card"; sub_8218B998: r3 its object, r4 its size, r5 the file, r7 an id), where it is (a creation slot's +offset), and
// how its upload ends (sub_8218BA70: the object's state, 2 done, 3 failed)
namespace {
bool PicDebug() {
  static const bool on = [] { const char* v = std::getenv("SC5_DBG_PICLOAD"); return v && v[0] == '1'; }();
  return on;
}
std::map<uint32_t, uint32_t> g_pic_state;
}  // namespace

REX_EXTERN(__imp__sub_8218B998);
REX_HOOK_RAW(sub_8218B998) {
  if (!PicDebug()) {
    __imp__sub_8218B998(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32, size = ctx.r4.u32, file = ctx.r5.u32, id = ctx.r7.u32, lr = uint32_t(ctx.lr);
  const uint32_t w0 = file ? Rd(file) : 0, w1 = file ? Rd(file + 4) : 0, w2 = file ? Rd(file + 8) : 0;
  const uint32_t first = sc5::creation::Slot(0);
  __imp__sub_8218B998(ctx, base);
  const bool in = first && file >= first && file < first + 50 * sc5::creation::kSlotSize;
  REXLOG_INFO("sc5 dbg pic: load obj {:08X} file {:08X} (slot {} +{}) size {} id {:08X} from {:08X}: {} head {:08X} "
              "{:08X} {:08X} state {} kind {}",
              obj, file, in ? int((file - first) / sc5::creation::kSlotSize) + 1 : 0,
              in ? int((file - first) % sc5::creation::kSlotSize) : -1, size, id, lr, ctx.r3.u32, w0, w1, w2, Rd(obj),
              Rd(obj + 8));
  g_pic_state[obj] = Rd(obj);
}

REX_EXTERN(__imp__sub_8218BA70);
REX_HOOK_RAW(sub_8218BA70) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_8218BA70(ctx, base);
  if (!PicDebug()) return;
  const uint32_t state = Rd(obj);
  const auto it = g_pic_state.find(obj);
  if (it == g_pic_state.end() || it->second != state) {
    REXLOG_INFO("sc5 dbg pic: upload obj {:08X}: state {} kind {} -> {}", obj, state, Rd(obj + 8), ctx.r3.u32);
    g_pic_state[obj] = state;
  }
}
