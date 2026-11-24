// The launcher's display and sound settings, see display.h.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sc5/display.h"
#include "sc5/framemeter.h"
#include "sc5/sound.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(int32_t, window_width);
REXCVAR_DECLARE(int32_t, window_height);
REXCVAR_DECLARE(bool, d3d12_allow_variable_refresh_rate_and_tearing);
// the GPU's own options (resolution_scale, swap_post_effect, anisotropic_override, vsync) live in the GPU plugin, which
// registers them as it loads: at the start through their REX_ environment variables, which the SDK applies then (the
// command line still wins), and later by name

namespace sc5::display {
namespace {

namespace fs = std::filesystem;

struct Settings {
  std::string mode = "windowed";  // windowed, borderless, fullscreen
  std::string resolution = "desktop";
  int render_scale = 1;
  std::string anti_aliasing = "off";  // off, fxaa, fxaa_extreme
  int anisotropic = 0;                // 0 the game's own, 2 4 8 16
  bool vsync = true, adaptive_sync = true;
  int brightness = 100;  // percent
  bool frame_data = true;  // the frame meter in practice (framemeter.h): the launcher's default
  int volume_master = 100, volume_music = 100, volume_effects = 100, volume_voices = 100;
};

// one key=value (the file's names, or the environment's without SC5_ and in lower case)
void Take(Settings& s, const std::string& key, const std::string& v) {
  auto num = [&](int lo, int hi, int def) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    return end && end != v.c_str() ? int(std::clamp<long>(n, lo, hi)) : def;
  };
  if (key == "mode" || key == "display") s.mode = v;
  else if (key == "resolution") s.resolution = v;
  else if (key == "render_scale") s.render_scale = num(1, 4, 1);
  else if (key == "anti_aliasing") s.anti_aliasing = v;
  else if (key == "anisotropic") s.anisotropic = num(0, 16, 0);
  else if (key == "vsync") s.vsync = v != "0";
  else if (key == "adaptive_sync") s.adaptive_sync = v != "0";
  else if (key == "brightness") s.brightness = num(50, 200, 100);
  else if (key == "frame_data" || key == "framedata") s.frame_data = v == "1";
  else if (key == "volume_master") s.volume_master = num(0, 150, 100);
  else if (key == "volume_music") s.volume_music = num(0, 150, 100);
  else if (key == "volume_effects") s.volume_effects = num(0, 150, 100);
  else if (key == "volume_voices") s.volume_voices = num(0, 150, 100);
}

bool FromEnvironment(Settings& s) {
  static const char* const kVars[] = {"DISPLAY",       "RESOLUTION",     "RENDER_SCALE",  "ANTI_ALIASING",
                                      "ANISOTROPIC",   "VSYNC",          "ADAPTIVE_SYNC", "BRIGHTNESS",
                                      "FRAMEDATA",     "VOLUME_MASTER",  "VOLUME_MUSIC",  "VOLUME_EFFECTS",
                                      "VOLUME_VOICES"};
  bool any = false;
  for (const char* name : kVars) {
    const std::string var = std::string("SC5_") + name;
    const char* v = std::getenv(var.c_str());
    if (!v) continue;
    std::string key = name;
    for (char& c : key) c = char(std::tolower(uint8_t(c)));
    Take(s, key, v);
    any = true;
  }
  return any;
}

bool FromFile(const fs::path& path, Settings& s) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  for (std::string line; std::getline(f, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    Take(s, line.substr(0, eq), line.substr(eq + 1));
  }
  return true;
}

// the command line set it: the launcher's setting does not
bool OnCommandLine(const wchar_t* name) { return std::wcsstr(GetCommandLineW(), name) != nullptr; }

// a GPU plugin option for the start: its REX_ variable (unless one is set already)
void SetForPlugin(const char* flag, const std::string& value) {
  std::string var = "REX_";
  for (const char* p = flag; *p; p++) var += char(std::toupper(uint8_t(*p)));
  if (std::getenv(var.c_str())) return;
  _putenv_s(var.c_str(), value.c_str());
  SetEnvironmentVariableA(var.c_str(), value.c_str());
}

int AnisotropicCvar(int times) {
  switch (times) {
    case 2: return 2;
    case 4: return 3;
    case 8: return 4;
    case 16: return 5;
    default: return -1;  // the game's own filtering
  }
}

// the window's size for a resolution: "WxH", or the screen's ("desktop"); a window too large for the screen (or at
// "desktop") is as large as fits on it with its frame, in the same shape
void WindowSize(const std::string& resolution, bool windowed, uint32_t& w, uint32_t& h) {
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int screen_w = GetSystemMetrics(SM_CXSCREEN), screen_h = GetSystemMetrics(SM_CYSCREEN);
  int want_w = 0, want_h = 0;
  if (std::sscanf(resolution.c_str(), "%dx%d", &want_w, &want_h) != 2 || want_w <= 0 || want_h <= 0) {
    want_w = screen_w;
    want_h = screen_h;
  }
  if (!windowed) {
    w = uint32_t(want_w);
    h = uint32_t(want_h);
    return;
  }
  // the frame around the client area
  RECT frame{0, 0, 100, 100};
  AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  const int extra_w = (frame.right - frame.left) - 100, extra_h = (frame.bottom - frame.top) - 100;
  const int room_w = (work.right - work.left) - extra_w, room_h = (work.bottom - work.top) - extra_h;
  if (want_w > room_w || want_h > room_h) {
    const double scale = std::min(double(room_w) / want_w, double(room_h) / want_h);
    want_w = int(want_w * scale);
    want_h = int(want_h * scale);
  }
  w = uint32_t(std::max(320, want_w));
  h = uint32_t(std::max(180, want_h));
}

std::atomic<float> g_master{1.0f};
std::atomic<int> g_brightness{100};
Settings g_now;
std::mutex g_lock;
WindowHooks g_hooks;
bool g_window_pending = false;

rex::graphics::CommandProcessor* Gpu() {
  auto* rt = rex::Runtime::instance();
  auto* gs = rt ? static_cast<rex::graphics::GraphicsSystem*>(rt->graphics_system()) : nullptr;
  return gs ? gs->command_processor() : nullptr;
}

rex::graphics::CommandProcessor::SwapPostEffect EffectOf(const std::string& aa) {
  using E = rex::graphics::CommandProcessor::SwapPostEffect;
  return aa == "fxaa" ? E::kFxaa : aa == "fxaa_extreme" ? E::kFxaaExtreme : E::kNone;
}

// --- brightness: a curve over the game's own gamma ramp (the GPU's display LUT), blacks and whites kept ---

struct Gamma {
  bool have = false;
  int applied_for = 100;
  uint32_t guest256[256] = {}, ours256[256] = {};
  uint32_t guest_pwl[128 * 3] = {}, ours_pwl[128 * 3] = {};
};
Gamma g_gamma;

// the command processor keeps its ramp and the re-upload behind protected members: reached through a member pointer
// named in a derived class (no SDK change)
struct GammaAccess : rex::graphics::CommandProcessor {
  static rex::graphics::reg::DC_LUT_30_COLOR* Table(rex::graphics::CommandProcessor* cp) {
    return const_cast<rex::graphics::reg::DC_LUT_30_COLOR*>((cp->*(&GammaAccess::gamma_ramp_256_entry_table))());
  }
  static rex::graphics::reg::DC_LUT_PWL_DATA* Pwl(rex::graphics::CommandProcessor* cp) {
    return const_cast<rex::graphics::reg::DC_LUT_PWL_DATA*>((cp->*(&GammaAccess::gamma_ramp_pwl_rgb))());
  }
  static void Written(rex::graphics::CommandProcessor* cp) {
    (cp->*(&GammaAccess::OnGammaRamp256EntryTableValueWritten))();
    (cp->*(&GammaAccess::OnGammaRampPWLValueWritten))();
  }
};

double Curve(double x, int brightness) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  return std::pow(x, 100.0 / double(brightness));
}

void UpdateGamma() {
  const int b = g_brightness.load();
  Gamma& g = g_gamma;
  // at 100% (as the game draws it) the game's own ramp is left alone: nothing is written unless brightness was changed
  // (and then the game's own values go back once)
  if (b == 100 && (!g.have || g.applied_for == 100)) return;
  auto* cp = Gpu();
  if (!cp) return;
  auto* table = GammaAccess::Table(cp);
  auto* pwl = GammaAccess::Pwl(cp);
  if (b == 100) {
    for (int i = 0; i < 256; i++) table[i].value = g.guest256[i];
    for (int i = 0; i < 128 * 3; i++) pwl[i].value = g.guest_pwl[i];
    for (int i = 0; i < 256; i++) g.ours256[i] = g.guest256[i];
    for (int i = 0; i < 128 * 3; i++) g.ours_pwl[i] = g.guest_pwl[i];
    g.applied_for = 100;
    GammaAccess::Written(cp);
    return;
  }
  // what the game wrote last: whatever differs from what this wrote is the game's (it set its ramp again)
  bool guest_changed = !g.have;
  for (int i = 0; i < 256 && !guest_changed; i++) guest_changed = table[i].value != g.ours256[i];
  for (int i = 0; i < 128 * 3 && !guest_changed; i++) guest_changed = pwl[i].value != g.ours_pwl[i];
  if (!guest_changed && g.applied_for == b) return;
  if (guest_changed) {
    for (int i = 0; i < 256; i++) g.guest256[i] = table[i].value;
    for (int i = 0; i < 128 * 3; i++) g.guest_pwl[i] = pwl[i].value;
    g.have = true;
  }
  // the 256-entry table: 10 bits per channel
  for (int i = 0; i < 256; i++) {
    rex::graphics::reg::DC_LUT_30_COLOR in, out;
    in.value = g.guest256[i];
    out.value = in.value;
    auto f = [&](uint32_t c) { return uint32_t(std::lround(Curve(c / 1023.0, b) * 1023.0)); };
    out.color_10_red = f(in.color_10_red);
    out.color_10_green = f(in.color_10_green);
    out.color_10_blue = f(in.color_10_blue);
    g.ours256[i] = out.value;
    table[i].value = out.value;
  }
  // the piecewise linear one: each segment's base and its rise to the next
  for (int i = 0; i < 128; i++)
    for (int c = 0; c < 3; c++) {
      rex::graphics::reg::DC_LUT_PWL_DATA in, out;
      in.value = g.guest_pwl[i * 3 + c];
      const double y0 = in.base / 65535.0, y1 = std::min(1.0, (double(in.base) + in.delta) / 65535.0);
      const double o0 = Curve(y0, b), o1 = Curve(y1, b);
      out.value = 0;
      out.base = uint32_t(std::lround(o0 * 65535.0)) & ~0x3Fu;
      out.delta = uint32_t(std::max(0L, std::lround((o1 - o0) * 65535.0)));
      g.ours_pwl[i * 3 + c] = out.value;
      pwl[i * 3 + c].value = out.value;
    }
  g.applied_for = b;
  GammaAccess::Written(cp);
}

// the settings now in force (the file's, or the start's): what applies at once
void Apply(const Settings& s, bool at_start) {
  g_master = float(s.volume_master) / 100.0f;
  sc5::sound::SetLevels(float(s.volume_music) / 100.0f, float(s.volume_effects) / 100.0f,
                        float(s.volume_voices) / 100.0f);
  g_brightness = s.brightness;
  sc5::framemeter::SetEnabled(s.frame_data);
  if (at_start) return;
  rex::cvar::SetFlagByName("vsync", s.vsync ? "true" : "false");
  rex::cvar::SetFlagByName("anisotropic_override", std::to_string(AnisotropicCvar(s.anisotropic)));
}

void Watch(fs::path file) {
  fs::file_time_type stamp{};
  bool have = false;
  for (;;) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    if (!ec && (!have || t != stamp)) {
      Settings s;
      if (FromFile(file, s)) {
        stamp = t;
        const bool first = !have;
        have = true;
        Settings before;
        {
          std::lock_guard<std::mutex> lock(g_lock);
          before = g_now;
          g_now = s;
          if (!first && (s.mode != before.mode || s.resolution != before.resolution)) g_window_pending = true;
        }
        Apply(s, false);
        if (!first)
          REXLOG_INFO("sc5 display: settings changed ({}{}{}{}): {}, {}, v-sync {}, {}, anisotropic {}, brightness {}%, "
                      "volume {}% (music {}%, effects {}%, voices {}%)",
                      s.render_scale != before.render_scale ? "the internal resolution at the next start, " : "",
                      s.adaptive_sync != before.adaptive_sync ? "adaptive sync at the next start, " : "",
                      g_window_pending ? "the window when it is in front, " : "", "the rest at once", s.mode,
                      s.resolution, s.vsync ? "on" : "off", s.anti_aliasing, s.anisotropic, s.brightness,
                      s.volume_master, s.volume_music, s.volume_effects, s.volume_voices);
      }
    }
    // the window's mode and size once the game's window is the foreground window (never pulled over the launcher)
    bool apply_window = false;
    Settings now;
    {
      std::lock_guard<std::mutex> lock(g_lock);
      now = g_now;
      apply_window = g_window_pending && g_hooks.in_front && g_hooks.in_front();
      if (apply_window) g_window_pending = false;
    }
    if (apply_window && g_hooks.apply) {
      uint32_t w = 0, h = 0;
      const bool windowed = now.mode == "windowed";
      WindowSize(now.resolution, windowed, w, h);
      g_hooks.apply(!windowed, windowed ? w : 0, windowed ? h : 0);
      REXLOG_INFO("sc5 display: the window {} {}x{}", windowed ? "in a window" : "full screen", w, h);
    }
    UpdateGamma();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace

void ConfigureFromEnvironment() {
  Settings s;
  if (!FromEnvironment(s)) return;  // a game started by hand keeps its own configuration
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_now = s;
  }
  const bool windowed = s.mode == "windowed";
  if (!OnCommandLine(L"--fullscreen")) REXCVAR_SET(fullscreen, !windowed);
  if (windowed && !OnCommandLine(L"--window_width") && !OnCommandLine(L"--window_height")) {
    uint32_t w = 0, h = 0;
    WindowSize(s.resolution, true, w, h);
    REXCVAR_SET(window_width, int32_t(w));
    REXCVAR_SET(window_height, int32_t(h));
  }
  if (!OnCommandLine(L"--draw_resolution_scale"))
    SetForPlugin("resolution_scale", std::to_string(std::clamp(s.render_scale, 1, 4)));
  SetForPlugin("swap_post_effect",
               s.anti_aliasing == "fxaa" || s.anti_aliasing == "fxaa_extreme" ? s.anti_aliasing : std::string("none"));
  SetForPlugin("anisotropic_override", std::to_string(AnisotropicCvar(s.anisotropic)));
  SetForPlugin("vsync", s.vsync ? "true" : "false");
  if (!OnCommandLine(L"--d3d12_allow_variable_refresh_rate_and_tearing"))
    REXCVAR_SET(d3d12_allow_variable_refresh_rate_and_tearing, s.adaptive_sync);
  Apply(s, true);
  REXLOG_INFO("sc5 display: {} at {}, internal resolution x{}, {}, anisotropic {}, v-sync {}{}, brightness {}%, "
              "volume {}% (music {}%, effects {}%, voices {}%)",
              s.mode, s.resolution, s.render_scale, s.anti_aliasing, s.anisotropic, s.vsync ? "on" : "off",
              s.adaptive_sync ? " (adaptive)" : "", s.brightness, s.volume_master, s.volume_music, s.volume_effects,
              s.volume_voices);
}

void Start(WindowHooks hooks) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_hooks = std::move(hooks);
  }
  // what the GPU took (its plugin's options, from the start's REX_ variables or the command line)
  REXLOG_INFO("sc5 display: in force: resolution_scale {}, swap_post_effect {}, anisotropic_override {}, vsync {}, "
              "fullscreen {}, window {}x{}",
              rex::cvar::GetFlagByName("resolution_scale"), rex::cvar::GetFlagByName("swap_post_effect"),
              rex::cvar::GetFlagByName("anisotropic_override"), rex::cvar::GetFlagByName("vsync"),
              rex::cvar::GetFlagByName("fullscreen"), rex::cvar::GetFlagByName("window_width"),
              rex::cvar::GetFlagByName("window_height"));
  const char* file = std::getenv("SC5_DISPLAY_FILE");
  // without the file: brightness from the start's settings still needs its curve kept over the game's ramp
  std::thread([path = file && *file ? fs::path(file) : fs::path()] {
    if (!path.empty()) Watch(path);
    for (;;) {
      UpdateGamma();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }).detach();
}

float MasterVolume() { return g_master.load(); }

}  // namespace sc5::display

// The master volume: the frames the game's audio hands the system (6 channels x 256 samples, big-endian floats),
// scaled in place before the SDK's mixer takes them (as its own XAudioSubmitRenderDriverFrame does)
namespace {
uint32_t Sc5XAudioSubmitRenderDriverFrame(mapped_void driver_ptr, mapped_void samples_ptr) {
  const float gain = sc5::display::MasterVolume();
  const uint32_t samples = samples_ptr.guest_address();
  static bool told = false;
  if (!told && gain != 1.0f) {
    told = true;
    REXLOG_INFO("sc5 display: the game's sound at {:.0f}% (master volume)", gain * 100.0f);
  }
  // SC5_TEST_AUDIO_RMS=1: the loudness of what the game hands the system, each second (before the master volume)
  static const bool rms_test = [] {
    const char* v = std::getenv("SC5_TEST_AUDIO_RMS");
    return v && v[0] == '1';
  }();
  if (rms_test && samples) {
    static double sum = 0, peak = 0;
    static uint32_t frames = 0;
    const auto* p = REX_KERNEL_STATE()->memory()->TranslateVirtual<const uint32_t*>(samples);
    for (int i = 0; i < 6 * 256; i++) {
      const uint32_t bits = __builtin_bswap32(p[i]);
      float f;
      std::memcpy(&f, &bits, 4);
      sum += double(f) * f;
      peak = std::max(peak, double(std::fabs(f)));
    }
    if (++frames == 188) {  // 48000 samples a second, 256 a frame
      const double rms = std::sqrt(sum / (188.0 * 6 * 256));
      REXLOG_INFO("sc5 audio test: loudness {:.1f} dB (peak {:.1f} dB)", 20 * std::log10(rms + 1e-9),
                  20 * std::log10(peak + 1e-9));
      sum = peak = 0;
      frames = 0;
    }
  }
  if (gain != 1.0f && samples) {
    auto* p = REX_KERNEL_STATE()->memory()->TranslateVirtual<uint32_t*>(samples);
    for (int i = 0; i < 6 * 256; i++) {
      uint32_t bits = __builtin_bswap32(p[i]);
      float f;
      std::memcpy(&f, &bits, 4);
      f *= gain;
      std::memcpy(&bits, &f, 4);
      p[i] = __builtin_bswap32(bits);
    }
  }
  auto* audio = static_cast<rex::audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples);
  return 0;
}
}  // namespace

REX_HOOK(__imp__XAudioSubmitRenderDriverFrame, Sc5XAudioSubmitRenderDriverFrame)
