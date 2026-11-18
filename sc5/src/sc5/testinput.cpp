// Scripted controllers for tests, see testinput.h.

#include "sc5/testinput.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "sc5/frame.h"
#include "sc5/game.h"

namespace sc5::testinput {
namespace {

using sc5::input::Pad;

struct Step {
  uint64_t frame;
  Pad pad;
};

std::array<std::vector<Step>, 4> g_script;
std::array<std::vector<Step>, 4> g_fight_script;  // SC5_FIGHT_INPUT_Pn: by fight frame (game::FightFrame)
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

bool Source(uint32_t user, Pad& out) {
  if (user >= 4) return false;
  if (g_scripted[user]) {
    const int64_t fight = game::FightFrame();
    if (fight >= 0 && !g_fight_script[user].empty()) {
      out = ScriptAt(g_fight_script[user], user, uint64_t(fight));
    } else {
      out = ScriptAt(g_script[user], user, frame::Count());
    }
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
    for (int fight = 0; fight < 2; fight++) {
      char name[24];
      std::snprintf(name, sizeof name, fight ? "SC5_FIGHT_INPUT_P%d" : "SC5_INPUT_P%d", u + 1);
      const char* text = std::getenv(name);
      if (!text || !*text) continue;
      auto& script = fight ? g_fight_script[u] : g_script[u];
      if (!ParseScript(text, script)) {
        REXLOG_ERROR("sc5: {} could not be read: {}", name, text);
        script.clear();
        continue;
      }
      g_scripted[u] = true;
      g_active = true;
      REXLOG_INFO("sc5: user {} plays a script of {} steps{}", u, script.size(), fight ? " in fights" : "");
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
  const int64_t fight = game::FightFrame();
  out = fight >= 0 && !g_fight_script[user].empty() ? ScriptAt(g_fight_script[user], user, uint64_t(fight))
                                                     : ScriptAt(g_script[user], user, frame::Count());
  out.connected = true;
  return true;
}

}  // namespace sc5::testinput
