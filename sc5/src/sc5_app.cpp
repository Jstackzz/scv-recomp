// sc5 - Soulcalibur V (Xbox 360), recompiled with ReXGlue.

#include "generated/default/sc5_init.h"

#include "sc5_app.h"

#include <cstdlib>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/system/kernel_state.h>

#include "sc5/autopilot.h"
#include "sc5/controls.h"
#include "sc5/display.h"
#include "sc5/dlc.h"
#include "sc5/link.h"
#include "sc5/system_ui.h"
#include "sc5/net/netplay.h"
#include "sc5/net/synctest.h"
#include "sc5/unlocks.h"
#include "sc5/testinput.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace fs = std::filesystem;

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, update_data_root);
REXCVAR_DECLARE(std::string, cache_root);
REXCVAR_DECLARE(bool, protect_zero);
REXCVAR_DECLARE(std::string, log_file);
#ifdef _WIN32
REXCVAR_DECLARE(int32_t, d3d12_adapter);
#endif

namespace {

fs::path EnvPath(const char* name) {
#ifdef _WIN32
  wchar_t buf[32768];
  wchar_t wname[64];
  size_t i = 0;
  for (; name[i] && i < 63; i++) wname[i] = wchar_t(name[i]);
  wname[i] = 0;
  DWORD n = GetEnvironmentVariableW(wname, buf, DWORD(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return fs::path(std::wstring(buf, n));
#else
  const char* v = std::getenv(name);
  return v ? fs::path(v) : fs::path();
#endif
}

#ifdef _WIN32
// Windows 11 may run a background-looking process on efficiency cores at low clocks: a fighting game
// must not (frame pacing). Same as MK9 Online.
void OptOutOfPowerThrottling() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  state.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

// A laptop with two graphics chips: the one Windows calls high-performance, unless the command line picked one.
void PickFastAdapter() {
  if (REXCVAR_GET(d3d12_adapter) >= 0) return;
  IDXGIFactory6* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  IDXGIAdapter1* fast = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&fast)))) {
    DXGI_ADAPTER_DESC1 want{};
    fast->GetDesc1(&want);
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; i++) {
      DXGI_ADAPTER_DESC1 d{};
      const bool same = SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.LowPart == want.AdapterLuid.LowPart &&
                        d.AdapterLuid.HighPart == want.AdapterLuid.HighPart;
      a->Release();
      if (!same) continue;
      if (i != 0) REXCVAR_SET(d3d12_adapter, int32_t(i));
      break;
    }
    fast->Release();
  }
  factory->Release();
}
#endif

}  // namespace

// Defaults, unless the command line (--game_data_root=...) or the environment says otherwise:
//   game data   <exe>\game\disc or <exe>\game, SC5_GAME_DIR (default.xex with the title update's default.xexp
//               beside it: the runtime applies it as it loads the executable, as codegen did)
//   update      <game data>\..\update, SC5_UPDATE_DIR: the title update's other files (patch.cpk), as update:
//   saves       <exe>\user, SC5_USER_DIR; the shader cache in <saves>\cache
void Sc5App::OnConfigurePaths(rex::PathConfig& paths) {
  // The game reads address 0 on purpose (a parameter getter's out-of-range default, sc5_codegen.toml): the
  // zero page must read as zeros, as on the console. Before the memory is set up, so this runs here.
  REXCVAR_SET(protect_zero, false);
  const fs::path exe = rex::filesystem::GetExecutableFolder();
  if (REXCVAR_GET(game_data_root).empty()) {
    if (auto env = EnvPath("SC5_GAME_DIR"); !env.empty()) {
      paths.game_data_root = env;
    } else if (fs::exists(exe / "game" / "disc" / "default.xex")) {
      paths.game_data_root = exe / "game" / "disc";
    } else if (fs::exists(exe / "game" / "default.xex")) {
      paths.game_data_root = exe / "game";
    }
  }
  if (REXCVAR_GET(update_data_root).empty()) {
    if (auto env = EnvPath("SC5_UPDATE_DIR"); !env.empty()) {
      paths.update_data_root = env;
    } else if (!paths.game_data_root.empty() && fs::exists(paths.game_data_root.parent_path() / "update")) {
      paths.update_data_root = paths.game_data_root.parent_path() / "update";
    }
  }
  if (REXCVAR_GET(user_data_root).empty()) {
    auto env = EnvPath("SC5_USER_DIR");
    paths.user_data_root = env.empty() ? exe / "user" : env;
  }
  if (REXCVAR_GET(cache_root).empty()) {
    // the launcher keeps the shader cache in the game folder (SC5_CACHE_DIR): online, the saves start empty each time
    auto env = EnvPath("SC5_CACHE_DIR");
    paths.cache_root = env.empty() ? paths.user_data_root / "cache" : env;
  }
  // the launcher's session log (SC5_LOG), unless the command line names one
  if (REXCVAR_GET(log_file).empty()) {
    if (auto env = EnvPath("SC5_LOG"); !env.empty()) REXCVAR_SET(log_file, env.string());
  }
  // the launcher's display settings (docs\PROTOCOL.md 3.1), before the window and the GPU exist
  sc5::display::ConfigureFromEnvironment();
}

void Sc5App::OnPreSetup(rex::RuntimeConfig& config) {
#ifdef _WIN32
  OptOutOfPowerThrottling();
  PickFastAdapter();
#endif
  // The game draws through the Xenos GPU emulation; the SDK leaves it off unless asked.
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }
}

void Sc5App::OnPostSetup() {
  // tests: SC5_TEST_HEAP_SHIFT=<MB> takes that much of the guest's heap at 0x40000000 before the game allocates any,
  // so that everything the game allocates there lies elsewhere than in another run (raw pointers then differ)
  if (const char* v = std::getenv("SC5_TEST_HEAP_SHIFT"); v && std::atoi(v) > 0) {
    auto* heap = REX_KERNEL_STATE()->memory()->LookupHeap(0x40000000);
    uint32_t at = 0;
    const uint32_t size = uint32_t(std::atoi(v)) << 20;
    const bool ok = heap && heap->Alloc(size, 0x10000,
                                        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &at);
    REXLOG_INFO("sc5: heap shifted by {} MB for a test ({:08X}){}", std::atoi(v), at, ok ? "" : ": failed");
  }
  REXLOG_INFO("sc5: game data {}, update {}, saves {}", game_data_root().string(), update_data_root().string(),
              user_data_root().string());
  // the player's own downloadable content (dlc.h): SC5_DLC_DIR, else the dlc folder beside the game's
  {
    fs::path dlc = EnvPath("SC5_DLC_DIR");
    if (dlc.empty() && !game_data_root().empty()) dlc = game_data_root().parent_path() / "dlc";
    sc5::dlc::Install(dlc, user_data_root());
  }
  sc5::controls::Install();
  // the display settings file, read again as it changes; the window is changed on the UI thread
  sc5::display::WindowHooks hooks;
  hooks.in_front = [this] {
    auto* w = window();
    return w && GetForegroundWindow() == static_cast<HWND>(w->GetNativeWindowHandle());
  };
  hooks.apply = [this](bool full, uint32_t width, uint32_t height) {
    app_context().CallInUIThread([this, full, width, height] {
      auto* w = window();
      if (!w) return;
      if (w->IsFullscreen() != full) w->SetFullscreen(full);
      if (!full && width && height) {
        HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
        RECT r{0, 0, LONG(width), LONG(height)};
        AdjustWindowRect(&r, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    });
  };
  sc5::display::Start(std::move(hooks));
  sc5::testinput::Init();
  sc5::system_ui::Init();
  sc5::unlocks::Init();
  sc5::synctest::Init();
  sc5::net::InitFromEnvironment();
  sc5::link::Init();
  // a plain game from the launcher's PRACTICE or VERSUS button (docs\PROTOCOL.md 3.1): the autopilot takes it to
  // Training's or the two-player VS Battle's select screen and hands it to the players there
  if (!sc5::link::On()) {
    const char* training = std::getenv("SC5_TRAINING");
    const char* versus = std::getenv("SC5_VERSUS");
    sc5::autopilot::Plan plan;
    if (training && training[0] == '1') {
      plan.mode = sc5::autopilot::Mode::Practice;
      sc5::autopilot::InitPlain(plan);
    } else if (versus && versus[0] == '1') {
      plan.mode = sc5::autopilot::Mode::Versus;
      plan.two_players = true;
      sc5::autopilot::InitPlain(plan);
    }
  }
}
