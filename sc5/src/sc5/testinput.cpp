// Scripted controllers for tests, see testinput.h.

#include "sc5/testinput.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/select.h"

namespace sc5::testinput {
namespace {

using sc5::input::Pad;

struct Step {
  uint64_t frame;
  Pad pad;
};

std::array<std::vector<Step>, 4> g_script;
std::array<std::vector<Step>, 4> g_fight_script;  // SC5_FIGHT_INPUT_Pn: by fight frame (game::FightFrame)
std::array<std::vector<Step>, 4> g_select_script;  // SC5_SELECT_INPUT_Pn: by the select screen's frames (select::Age)
std::array<std::vector<Step>, 4> g_device_script;  // SC5_TEST_DEVICE_Pn: the PC's own controller in fights
std::array<bool, 4> g_scripted{};
int g_pads = 0;
bool g_no_physical = false;
bool g_active = false;

bool ParseButtons(const std::string& s, Pad& pad) {
  pad = Pad{};
  pad.connected = true;
  if (s == "-" || s.empty()) return true;
  std::stringstream ss(s);
  std::string name;
  while (std::getline(ss, name, '+')) {
    struct Named {
      const char* name;
      uint16_t bit;
    };
    static const Named kNames[] = {
        {"UP", input::kDpadUp},       {"DOWN", input::kDpadDown},       {"LEFT", input::kDpadLeft},
        {"RIGHT", input::kDpadRight}, {"A", input::kA},                 {"B", input::kB},
        {"X", input::kX},             {"Y", input::kY},                 {"LB", input::kLeftShoulder},
        {"RB", input::kRightShoulder}, {"START", input::kStart},        {"BACK", input::kBack},
        {"LS", input::kLeftThumb},    {"RS", input::kRightThumb},
    };
    bool known = false;
    for (const Named& n : kNames) {
      if (_stricmp(n.name, name.c_str()) == 0) {
        pad.buttons |= n.bit;
        known = true;
      }
    }
    if (_stricmp(name.c_str(), "LT") == 0) {
      pad.left_trigger = 0xFF;
      known = true;
    }
    if (_stricmp(name.c_str(), "RT") == 0) {
      pad.right_trigger = 0xFF;
      known = true;
    }
    // an analog value as the netplay carries it: LX=, LY=, RX=, RY= a stick's top byte (-128..127, up and right
    // positive), LT=, RT= a trigger (0..255): a match's net log played back as it was
    if (name.size() > 3 && name[2] == '=') {
      const std::string axis = name.substr(0, 2);
      const int v = std::atoi(name.c_str() + 3);
      int16_t* stick = _stricmp(axis.c_str(), "LX") == 0   ? &pad.lx
                       : _stricmp(axis.c_str(), "LY") == 0 ? &pad.ly
                       : _stricmp(axis.c_str(), "RX") == 0 ? &pad.rx
                       : _stricmp(axis.c_str(), "RY") == 0 ? &pad.ry
                                                           : nullptr;
      if (stick && v >= -128 && v <= 127) {
        *stick = int16_t(v * 256);
        known = true;
      } else if ((_stricmp(axis.c_str(), "LT") == 0 || _stricmp(axis.c_str(), "RT") == 0) && v >= 0 && v <= 255) {
        (_stricmp(axis.c_str(), "LT") == 0 ? pad.left_trigger : pad.right_trigger) = uint8_t(v);
        known = true;
      }
    }
    if (!known) return false;
  }
  return true;
}

bool ParseScript(const char* text, std::vector<Step>& out) {
  std::stringstream ss(text);
  std::string item;
  uint64_t last = 0;
  while (std::getline(ss, item, ',')) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) return false;
    const std::string when = item.substr(0, colon);
    uint64_t frame = 0;
    if (!when.empty() && when[0] == '+') {
      frame = last + std::strtoull(when.c_str() + 1, nullptr, 10);
    } else {
      frame = std::strtoull(when.c_str(), nullptr, 10);
    }
    Step step{frame, {}};
    if (!ParseButtons(item.substr(colon + 1), step.pad)) return false;
    out.push_back(step);
    last = frame;
  }
  return true;
}

std::array<std::atomic<int>, 4> g_step_logged{-1, -1, -1, -1};

Pad ScriptAt(const std::vector<Step>& script, uint32_t user, uint64_t now) {
  Pad pad{};
  pad.connected = true;
  int step = -1;
  for (const Step& s : script) {
    if (s.frame > now) break;
    pad = s.pad;
    step++;
  }
  // each step once in the log, when the game first reads it
  if (step >= 0 && g_step_logged[user].exchange(step) != step) {
    REXLOG_INFO("sc5: user {} script step {} at frame {}: buttons {:04X}", user, step, now, pad.buttons);
  }
  return pad;
}

// SC5_INPUT_LIVE_Pn=<file>: a script read again whenever the file changes, its frames counted from when it is read
// ("0:A,+6:-" presses A for 6 frames now): someone (a tool) driving the menus as they go
std::array<std::string, 4> g_live_path;
std::array<std::vector<Step>, 4> g_live;
std::array<uint64_t, 4> g_live_checked{};
std::array<std::filesystem::file_time_type, 4> g_live_time{};
std::mutex g_live_lock;

Pad Live(uint32_t user) {
  std::lock_guard<std::mutex> lock(g_live_lock);
  const uint64_t now = frame::Count();
  if (now != g_live_checked[user]) {
    g_live_checked[user] = now;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(g_live_path[user], ec);
    if (!ec && t != g_live_time[user]) {
      g_live_time[user] = t;
      std::ifstream in(g_live_path[user]);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
      std::vector<Step> steps;
      if (!text.empty() && ParseScript(text.c_str(), steps)) {
        for (Step& s : steps) s.frame += now;
        g_live[user] = std::move(steps);
        REXLOG_INFO("sc5: user {} live script at frame {}: {}", user, now, text);
      }
    }
  }
  return ScriptAt(g_live[user], user, now);
}

// a scripted user's pad now: its fight script in a fight, its select script at a select screen, else its menu script
Pad Scripted(uint32_t user) {
  if (!g_live_path[user].empty()) return Live(user);
  const int64_t fight = game::FightFrame();
  if (fight >= 0 && !g_fight_script[user].empty()) return ScriptAt(g_fight_script[user], user, uint64_t(fight));
  if (!g_select_script[user].empty() && select::Object())
    return ScriptAt(g_select_script[user], user, select::Age());
  return ScriptAt(g_script[user], user, frame::Count());
}

bool Source(uint32_t user, Pad& out) {
  if (user >= 4) return false;
  if (g_scripted[user]) {
    out = Scripted(user);
    return true;
  }
  if (int(user) < g_pads) {
    out = g_no_physical ? Pad{} : input::ReadPhysical(user);
    out.connected = true;
    return true;
  }
  if (g_no_physical) {
    out = Pad{};
    return true;
  }
  return false;
}

}  // namespace

void Init() {
  for (int u = 0; u < 4; u++) {
    for (int fight = 0; fight < 3; fight++) {
      char name[24];
      std::snprintf(name, sizeof name, fight == 2 ? "SC5_SELECT_INPUT_P%d" : fight ? "SC5_FIGHT_INPUT_P%d" : "SC5_INPUT_P%d",
                    u + 1);
      const char* text = std::getenv(name);
      if (!text || !*text) continue;
      auto& script = fight == 2 ? g_select_script[u] : fight ? g_fight_script[u] : g_script[u];
      if (!ParseScript(text, script)) {
        REXLOG_ERROR("sc5: {} could not be read: {}", name, text);
        script.clear();
        continue;
      }
      g_scripted[u] = true;
      g_active = true;
      REXLOG_INFO("sc5: user {} plays a script of {} steps{}", u, script.size(),
                  fight == 2 ? " at select screens" : fight ? " in fights" : "");
    }
  }
  for (int u = 0; u < 4; u++) {
    char name[24];
    std::snprintf(name, sizeof name, "SC5_TEST_DEVICE_P%d", u + 1);
    const char* text = std::getenv(name);
    if (!text || !*text) continue;
    if (!ParseScript(text, g_device_script[u])) {
      REXLOG_ERROR("sc5: {} could not be read: {}", name, text);
      g_device_script[u].clear();
      continue;
    }
    REXLOG_INFO("sc5: user {}'s own controller plays a script of {} steps in fights", u, g_device_script[u].size());
  }
  for (int u = 0; u < 4; u++) {
    char name[24];
    std::snprintf(name, sizeof name, "SC5_INPUT_LIVE_P%d", u + 1);
    if (const char* p = std::getenv(name); p && *p) {
      g_live_path[u] = p;
      g_scripted[u] = true;
      g_active = true;
      REXLOG_INFO("sc5: user {} plays what {} says as it changes", u, p);
    }
  }
  if (const char* p = std::getenv("SC5_PADS"); p && *p) {
    g_pads = std::atoi(p);
    g_active = true;
  }
  if (const char* p = std::getenv("SC5_NO_PHYSICAL"); p && *p == '1') {
    g_no_physical = true;
    g_active = true;
  }
  if (g_active) input::SetSource(&Source);
}

bool Active() { return g_active; }

bool ScriptedPad(uint32_t user, Pad& out) {
  if (user >= 4 || !g_scripted[user]) return false;
  out = Scripted(user);
  out.connected = true;
  return true;
}

bool DevicePad(uint32_t user, Pad& out) {
  if (user >= 4 || g_device_script[user].empty()) return false;
  const int64_t fight = game::FightFrame();
  if (fight < 0) return false;
  out = ScriptAt(g_device_script[user], user, uint64_t(fight));
  return true;
}

}  // namespace sc5::testinput
