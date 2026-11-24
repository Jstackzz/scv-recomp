// The launcher's controls file, see controls.h.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sc5/controls.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "sc5/frame.h"
#include "sc5/input.h"
#include "sc5/net/netplay.h"

namespace sc5::controls {
namespace {

namespace fs = std::filesystem;
using sc5::input::Pad;

// the Xbox 360 buttons as the file names them
enum Control { kUp, kDown, kLeft, kRight, kA, kB, kX, kY, kLB, kRB, kLT, kRT, kStart, kBack, kLS, kRS, kCount };
const char* const kControlIds[kCount] = {"up", "down", "left", "right", "a",  "b",     "x",    "y",
                                         "lb", "rb",   "lt",   "rt",    "start", "back", "ls", "rs"};
// their XInput bits (the triggers are analog: 0)
const uint16_t kControlBits[kCount] = {input::kDpadUp,        input::kDpadDown, input::kDpadLeft, input::kDpadRight,
                                       input::kA,             input::kB,        input::kX,        input::kY,
                                       input::kLeftShoulder,  input::kRightShoulder, 0,           0,
                                       input::kStart,         input::kBack,     input::kLeftThumb, input::kRightThumb};

// a PC controller's buttons by where they sit: the SDK reports the first 16 in Xbox 360 positions; the rest only SDL
struct SourceButton {
  const char* id;
  uint16_t bit;    // the SDK's bit; 0: a trigger or an SDL-only button
  int trigger;     // 0 left, 1 right, -1 none
  int sdl;         // SDL_GamepadButton for an SDL-only button, -1 none
  int control;     // what it presses unless the file says otherwise (-1 nothing)
};
const SourceButton kSources[] = {
    {"south", input::kA, -1, -1, kA},
    {"east", input::kB, -1, -1, kB},
    {"west", input::kX, -1, -1, kX},
    {"north", input::kY, -1, -1, kY},
    {"right_shoulder", input::kRightShoulder, -1, -1, kRB},
    {"left_shoulder", input::kLeftShoulder, -1, -1, kLB},
    {"right_trigger", 0, 1, -1, kRT},
    {"left_trigger", 0, 0, -1, kLT},
    {"dpad_up", input::kDpadUp, -1, -1, kUp},
    {"dpad_down", input::kDpadDown, -1, -1, kDown},
    {"dpad_left", input::kDpadLeft, -1, -1, kLeft},
    {"dpad_right", input::kDpadRight, -1, -1, kRight},
    {"start", input::kStart, -1, -1, kStart},
    {"back", input::kBack, -1, -1, kBack},
    {"left_stick", input::kLeftThumb, -1, -1, kLS},
    {"right_stick", input::kRightThumb, -1, -1, kRS},
    {"misc", 0, -1, SDL_GAMEPAD_BUTTON_MISC1, -1},
    {"paddle1", 0, -1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, -1},
    {"paddle2", 0, -1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, -1},
    {"paddle3", 0, -1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, -1},
    {"paddle4", 0, -1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, -1},
    {"touchpad", 0, -1, SDL_GAMEPAD_BUTTON_TOUCHPAD, -1},
};
constexpr int kSourceCount = int(sizeof(kSources) / sizeof(kSources[0]));
constexpr uint8_t kTriggerPressed = 30;

struct Map {
  bool keyboard = true;
  std::array<std::vector<int>, kCount> keys;
  std::array<int, kSourceCount> presses{};
  std::string player2;  // player 2's controller by name; "" the second plugged in
  bool any_sdl = false;  // a button only SDL reports presses something
  std::array<int, kCount> fight;  // [fight]: the game's buttons a button presses in a fight (kGame*), -1 the game's own
  Map() { fight.fill(-1); }
};

// The buttons [fight] can set, and each one's bit in the game's own pad state (the pad manager at 0x82771080, + 128 a
// pad: what the battle's conversion sub_82171AE8 reads; its table at 0x8275C12C has the eight of the game's Control
// Settings, LT 0x2 LB 0x4 RT 0x10 B 0x20 A 0x40 RB 0x80 Y 0x100 X 0x200; the stick clicks, 0x1 and 0x8, press nothing
// there: the conversion lets through only the d-pad and START, 0xBC00)
struct FightButton {
  int control;
  uint32_t game_bit;
};
constexpr FightButton kFightButtons[] = {{kA, 0x40},  {kB, 0x20},  {kX, 0x200}, {kY, 0x100}, {kLB, 0x4},
                                         {kRB, 0x80}, {kLT, 0x2},  {kRT, 0x10}, {kLS, 0x1},  {kRS, 0x8}};
// Type A's face buttons for each of the game's buttons: X horizontal attack (A), Y vertical (B), B kick (K), A guard (G)
constexpr uint16_t kFaceOf[4] = {input::kX, input::kY, input::kB, input::kA};

bool IsFightControl(int c) {
  for (const FightButton& f : kFightButtons)
    if (f.control == c) return true;
  return false;
}

// "A+B+K+G" (any order, any case; "" nothing) -> kGame* bits, -1 when it is not that
int ComboFromText(const std::string& text) {
  int mask = 0;
  for (size_t i = 0; i < text.size(); i++) {
    const char ch = char(std::toupper(uint8_t(text[i])));
    if (ch == '+' || ch == ' ') continue;
    const char* letters = "ABKG";
    const char* at = std::strchr(letters, ch);
    if (!at || !ch) return -1;
    mask |= 1 << (at - letters);
  }
  return mask;
}

std::string ComboText(int mask) {
  std::string out;
  for (int i = 0; i < 4; i++)
    if (mask >> i & 1) out += std::string(out.empty() ? "" : "+") + "ABKG"[i];
  return out.empty() ? "nothing" : out;
}

// without the file (the launcher's own defaults, launcher/src/Controls.cpp): arrows or WASD move, J I L K are X Y B A
// (horizontal, vertical, kick, guard; Space guards too), U and O the bumpers, Y and P the triggers, Enter Start,
// Backspace Back
Map Defaults() {
  Map m;
  m.keys[kUp] = {VK_UP, 'W'};
  m.keys[kDown] = {VK_DOWN, 'S'};
  m.keys[kLeft] = {VK_LEFT, 'A'};
  m.keys[kRight] = {VK_RIGHT, 'D'};
  m.keys[kA] = {'K', VK_SPACE};
  m.keys[kB] = {'L'};
  m.keys[kX] = {'J'};
  m.keys[kY] = {'I'};
  m.keys[kLB] = {'U'};
  m.keys[kRB] = {'O'};
  m.keys[kLT] = {'Y'};
  m.keys[kRT] = {'P'};
  m.keys[kStart] = {VK_RETURN};
  m.keys[kBack] = {VK_BACK};
  for (int i = 0; i < kSourceCount; i++) m.presses[i] = kSources[i].control;
  return m;
}

std::mutex g_lock;
Map g_map = Defaults();
fs::file_time_type g_stamp{};
bool g_have_stamp = false;
std::chrono::steady_clock::time_point g_next_check{};

int ControlFromId(const std::string& id) {
  for (int c = 0; c < kCount; c++)
    if (_stricmp(id.c_str(), kControlIds[c]) == 0) return c;
  return -1;
}

// key names as the launcher writes them (launcher/src/Controls.cpp keyName)
int KeyFromName(const std::string& n) {
  if (n.size() == 1 && n[0] > 0 && std::isalnum(uint8_t(n[0]))) return std::toupper(uint8_t(n[0]));
  struct Named {
    const char* name;
    int vk;
  };
  static const Named kNamed[] = {
      {"Up", VK_UP},           {"Down", VK_DOWN},         {"Left", VK_LEFT},          {"Right", VK_RIGHT},
      {"Space", VK_SPACE},     {"Enter", VK_RETURN},      {"Return", VK_RETURN},      {"Backspace", VK_BACK},
      {"Tab", VK_TAB},         {"CapsLock", VK_CAPITAL},  {"Shift", VK_SHIFT},        {"LShift", VK_LSHIFT},
      {"RShift", VK_RSHIFT},   {"Ctrl", VK_CONTROL},      {"LCtrl", VK_LCONTROL},     {"RCtrl", VK_RCONTROL},
      {"Insert", VK_INSERT},   {"Delete", VK_DELETE},     {"Home", VK_HOME},          {"End", VK_END},
      {"PageUp", VK_PRIOR},    {"PageDown", VK_NEXT},     {"NumMultiply", VK_MULTIPLY}, {"NumAdd", VK_ADD},
      {"NumSubtract", VK_SUBTRACT}, {"NumDecimal", VK_DECIMAL}, {"NumDivide", VK_DIVIDE}, {"Semicolon", VK_OEM_1},
      {"Equals", VK_OEM_PLUS}, {"Comma", VK_OEM_COMMA},   {"Minus", VK_OEM_MINUS},    {"Period", VK_OEM_PERIOD},
      {"Slash", VK_OEM_2},     {"Grave", VK_OEM_3},       {"LBracket", VK_OEM_4},     {"Backslash", VK_OEM_5},
      {"RBracket", VK_OEM_6},  {"Apostrophe", VK_OEM_7},  {"Quote", VK_OEM_7},        {"Oem8", VK_OEM_8},
      {"Oem102", VK_OEM_102},
  };
  for (const Named& k : kNamed)
    if (_stricmp(k.name, n.c_str()) == 0) return k.vk;
  if (n.size() >= 2 && (n[0] == 'F' || n[0] == 'f') && std::isdigit(uint8_t(n[1]))) {
    const int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return VK_F1 + f - 1;
  }
  if (n.size() == 4 && _strnicmp(n.c_str(), "Num", 3) == 0 && std::isdigit(uint8_t(n[3]))) return VK_NUMPAD0 + (n[3] - '0');
  if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) {
    const int v = int(std::strtol(n.c_str() + 2, nullptr, 16));
    if (v > 0 && v < 0xFF) return v;
  }
  return 0;
}

std::string Trim(std::string s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string StripComment(const std::string& line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    if (line[i] == '"') quoted = !quoted;
    if (line[i] == '#' && !quoted) return line.substr(0, i);
  }
  return line;
}

// the quoted strings of a value ("K", ["W", "Up"]), with \" and \\ as the launcher writes them
std::vector<std::string> Quoted(const std::string& v) {
  std::vector<std::string> out;
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] != '"') continue;
    std::string s;
    size_t j = i + 1;
    for (; j < v.size() && v[j] != '"'; j++) {
      if (v[j] == '\\' && j + 1 < v.size()) j++;
      s += v[j];
    }
    out.push_back(s);
    i = j;
  }
  return out;
}

bool Load(const fs::path& path, Map& m) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  m = Defaults();
  std::string section, line;
  bool cleared[kCount] = {};
  bool first = true;
  while (std::getline(f, line)) {
    if (first && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
      line = line.substr(3);  // saved by Notepad
    first = false;
    line = Trim(StripComment(line));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = Trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (section.empty() && key == "keyboard") {
      m.keyboard = value != "false";
    } else if (section == "keyboard" && key == "enabled") {  // MK9 Online's older form
      m.keyboard = value != "false";
    } else if (section == "keys") {
      const int c = ControlFromId(key);
      if (c < 0) continue;
      if (!cleared[c]) {
        m.keys[c].clear();
        cleared[c] = true;
      }
      for (const std::string& name : Quoted(value)) {
        const int vk = KeyFromName(name);
        if (vk && m.keys[c].size() < 2 && std::find(m.keys[c].begin(), m.keys[c].end(), vk) == m.keys[c].end())
          m.keys[c].push_back(vk);
      }
    } else if (section == "buttons") {
      for (int i = 0; i < kSourceCount; i++) {
        if (_stricmp(key.c_str(), kSources[i].id) != 0) continue;
        const auto names = Quoted(value);
        m.presses[i] = names.empty() || names[0].empty() ? -1 : ControlFromId(names[0]);
      }
    } else if (section == "ports" && key == "player2") {
      const auto names = Quoted(value);
      m.player2 = names.empty() ? std::string() : names[0];
    } else if (section == "fight") {
      const int c = ControlFromId(key);
      const auto names = Quoted(value);
      if (c >= 0 && IsFightControl(c) && !names.empty()) m.fight[c] = ComboFromText(names[0]);
    }
  }
  for (int i = 0; i < kSourceCount; i++)
    if (kSources[i].sdl >= 0 && m.presses[i] >= 0) m.any_sdl = true;
  return true;
}

void Reassign();

void ReloadIfChanged() {
  const auto now = std::chrono::steady_clock::now();
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (now < g_next_check) return;
    g_next_check = now + std::chrono::seconds(1);
    const fs::path path = FilePath();
    std::error_code ec;
    const auto stamp = fs::last_write_time(path, ec);
    if (ec) {
      if (g_have_stamp) {
        g_map = Defaults();
        g_have_stamp = false;
        changed = true;
      }
    } else if (!g_have_stamp || stamp != g_stamp) {
      Map m;
      if (Load(path, m)) {
        changed = !g_have_stamp || m.player2 != g_map.player2;
        g_map = m;
        g_stamp = stamp;
        g_have_stamp = true;
        std::string fight;
        for (const FightButton& f : kFightButtons)
          if (m.fight[f.control] >= 0)
            fight += std::string(fight.empty() ? "" : " ") + kControlIds[f.control] + "=" +
                     ComboText(m.fight[f.control]);
        REXLOG_INFO("sc5 controls: from {} (keyboard {}, player 2's controller {}; in a fight {})", path.string(),
                    m.keyboard ? "on" : "off", m.player2.empty() ? "the second plugged in" : "\"" + m.player2 + "\"",
                    fight.empty() ? "the game's own buttons" : fight);
      }
    }
  }
  if (changed) Reassign();
}

bool GameInFront() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// --- which controller is which user's ([ports]) ---

class PortsAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    std::lock_guard<std::mutex> lock(lock_);
    devices_ = devices;
    Assign();
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    std::lock_guard<std::mutex> lock(lock_);
    if (user_index < users_.size())
      for (const auto& d : users_[user_index]) out.push_back(d.id);
  }
  void SetPlayer2(const std::string& name) {
    std::lock_guard<std::mutex> lock(lock_);
    player2_ = name;
    Assign();
  }
  // the user a controller of this name plays as (-1 none)
  int UserOfName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(lock_);
    for (size_t u = 0; u < users_.size(); u++)
      for (const auto& d : users_[u])
        if (!d.synthetic && d.name == name) return int(u);
    return -1;
  }

 private:
  // As the SDK's SlotAssignment (the n-th controller plugged in plays as user n; the keyboard's stand-ins as user 0),
  // but a controller named for player 2 plays as user 1 and every other keeps clear of it
  void Assign() {
    for (auto& u : users_) u.clear();
    std::vector<rex::input::DeviceInfo> real;
    for (const auto& d : devices_) {
      if (d.synthetic) users_[0].push_back(d);
      else real.push_back(d);
    }
    std::sort(real.begin(), real.end(), [](const auto& a, const auto& b) { return a.ordinal < b.ordinal; });
    int p2 = -1;
    if (!player2_.empty())
      for (size_t i = 0; i < real.size() && p2 < 0; i++)
        if (_stricmp(real[i].name.c_str(), player2_.c_str()) == 0) p2 = int(i);
    if (player2_.empty()) {
      for (size_t i = 0; i < real.size() && i < users_.size(); i++) users_[i].push_back(real[i]);
    } else {
      static const int kOthers[] = {0, 2, 3};
      size_t next = 0;
      for (size_t i = 0; i < real.size(); i++) {
        if (int(i) == p2) users_[1].push_back(real[i]);
        else if (next < 3) users_[size_t(kOthers[next++])].push_back(real[i]);
      }
    }
    std::string text;
    for (size_t u = 0; u < users_.size(); u++)
      for (const auto& d : users_[u])
        if (!d.synthetic) text += " user " + std::to_string(u) + ": " + d.name + ";";
    if (text != last_) REXLOG_INFO("sc5 controls: controllers{}", text.empty() ? " none" : text);
    last_ = text;
  }

  mutable std::mutex lock_;
  std::vector<rex::input::DeviceInfo> devices_;
  std::array<std::vector<rex::input::DeviceInfo>, rex::input::kMaxGuestUsers> users_;
  std::string player2_, last_;
};

PortsAssignment* g_ports = nullptr;

void Reassign() {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    name = g_map.player2;
  }
  if (g_ports) g_ports->SetPlayer2(name);
}

// the buttons only SDL reports, pressed on any controller that plays as this user
uint32_t SdlButtons(uint32_t user) {
  if (!g_ports || !SDL_WasInit(SDL_INIT_GAMEPAD)) return 0;
  uint32_t mask = 0;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; i++) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (!pad) continue;
    const char* name = SDL_GetGamepadName(pad);
    if (!name || g_ports->UserOfName(name) != int(user)) continue;
    for (int s = 0; s < kSourceCount; s++)
      if (kSources[s].sdl >= 0 && SDL_GetGamepadButton(pad, SDL_GamepadButton(kSources[s].sdl))) mask |= 1u << s;
  }
  SDL_free(ids);
  return mask;
}

}  // namespace

std::string FilePath() {
  if (const char* e = std::getenv("SC5_CONTROLS"); e && *e) return e;
  return (rex::filesystem::GetExecutableFolder() / "controls.toml").string();
}

void Install() {
  auto* ks = REX_KERNEL_STATE();
  auto* is = ks && ks->emulator() ? static_cast<rex::input::InputSystem*>(ks->emulator()->input_system()) : nullptr;
  if (!is) {
    REXLOG_WARN("sc5 controls: no input system; the SDK's assignment stays");
    return;
  }
  auto ports = std::make_unique<PortsAssignment>();
  g_ports = ports.get();
  ReloadIfChanged();
  Reassign();
  is->SetDeviceAssignment(std::move(ports));
}

void Apply(uint32_t user, Pad& pad) {
  ReloadIfChanged();
  Map m;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    m = g_map;
  }
  // a controller's buttons as the file maps them
  if (pad.connected) {
    const uint32_t sdl = m.any_sdl ? SdlButtons(user) : 0;
    uint16_t buttons = 0;
    uint8_t triggers[2] = {0, 0};
    for (int i = 0; i < kSourceCount; i++) {
      const SourceButton& s = kSources[i];
      bool down;
      uint8_t analog = 0xFF;
      if (s.trigger >= 0) {
        analog = s.trigger == 0 ? pad.left_trigger : pad.right_trigger;
        down = analog >= kTriggerPressed;
      } else if (s.sdl >= 0) {
        down = (sdl >> i & 1) != 0;
      } else {
        down = (pad.buttons & s.bit) != 0;
      }
      const int target = m.presses[i];
      if (!down || target < 0) continue;
      if (target == kLT || target == kRT) {
        uint8_t& t = triggers[target == kLT ? 0 : 1];
        t = std::max<uint8_t>(t, s.trigger >= 0 ? analog : 0xFF);
      } else {
        buttons |= kControlBits[target];
      }
    }
    pad.buttons = uint16_t((pad.buttons & input::kGuide) | buttons);
    pad.left_trigger = triggers[0];
    pad.right_trigger = triggers[1];
  }
  // the keyboard: player 1's, while the game's window is in front (a keyboard is always there: user 0 stays connected)
  if (user != 0 || !m.keyboard) return;
  pad.connected = true;
  if (!GameInFront()) return;
  for (int c = 0; c < kCount; c++) {
    bool down = false;
    for (int vk : m.keys[c]) down = down || (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) continue;
    if (c == kLT) pad.left_trigger = 0xFF;
    else if (c == kRT) pad.right_trigger = 0xFF;
    else pad.buttons |= kControlBits[c];
  }
}

namespace {

std::array<int, kCount> FightNow() {
  ReloadIfChanged();
  std::lock_guard<std::mutex> lock(g_lock);
  return g_map.fight;
}

}  // namespace

void ToDefaultLayout(Pad& pad) {
  const std::array<int, kCount> fight = FightNow();
  int game = 0;
  bool any = false;
  for (const FightButton& f : kFightButtons) {
    const int c = f.control;
    if (fight[c] < 0) continue;
    any = true;
    bool down;
    if (c == kLT) {
      down = pad.left_trigger >= kTriggerPressed;
      pad.left_trigger = 0;
    } else if (c == kRT) {
      down = pad.right_trigger >= kTriggerPressed;
      pad.right_trigger = 0;
    } else {
      down = (pad.buttons & kControlBits[c]) != 0;
      pad.buttons = uint16_t(pad.buttons & ~kControlBits[c]);
    }
    if (down) game |= fight[c];
  }
  if (!any) return;
  for (int i = 0; i < 4; i++)
    if (game >> i & 1) pad.buttons |= kFaceOf[i];
}

std::string FightText() {
  const std::array<int, kCount> fight = FightNow();
  std::string out;
  for (const FightButton& f : kFightButtons)
    if (fight[f.control] >= 0)
      out += std::string(out.empty() ? "" : " ") + kControlIds[f.control] + "=" + ComboText(fight[f.control]);
  return out;
}

// offline, player 1's pad: the bits of the buttons [fight] sets (`strip`) and the game's buttons they press (`add`)
void OfflineFight(uint32_t bits, uint32_t& strip, uint32_t& add) {
  const std::array<int, kCount> fight = FightNow();
  for (const FightButton& f : kFightButtons) {
    if (fight[f.control] < 0) continue;
    strip |= f.game_bit;
    if (bits & f.game_bit) add |= uint32_t(fight[f.control]);
  }
}

}  // namespace sc5::controls

// The battle's conversion of a pad's buttons into the game's (sub_82171AE8, from the battle's input task sub_8213DEC0,
// twice a player a frame: the buttons held and those newly pressed; r3 the pad, r4 its bits as the pad manager keeps
// them; r3 back: kGame* with the d-pad and START let through). Offline (no match of the netplay's, whose pads come in
// Type A already: ToDefaultLayout) player 1's pad (pad 0: the first controller and the keyboard) takes [fight]: each
// button set there presses its combination, every other button what the game's own Control Settings give it.
// Tests: SC5_DBG_FIGHT=1 logs each pad's game buttons newly pressed (its second call a frame).
REX_EXTERN(__imp__sub_82171AE8);
REX_HOOK_RAW(sub_82171AE8) {
  const uint32_t pad = ctx.r3.u32, bits = ctx.r4.u32;
  uint32_t strip = 0, add = 0;
  if (pad == 0 && !sc5::net::Active()) sc5::controls::OfflineFight(bits, strip, add);
  if (strip) ctx.r4.u64 = bits & ~strip;
  __imp__sub_82171AE8(ctx, base);
  if (strip) ctx.r3.u64 = ctx.r3.u32 | add;
  static const bool dbg = [] { const char* v = std::getenv("SC5_DBG_FIGHT"); return v && v[0] == '1'; }();
  if (dbg && pad < 4) {
    static int call[4] = {};
    if ((call[pad] ^= 1) == 0 && (ctx.r3.u32 & 0xF))
      REXLOG_INFO("sc5 controls: pad {} newly pressed game buttons {} (its bits {:08X}{}) at frame {}", pad,
                  sc5::controls::ComboText(int(ctx.r3.u32 & 0xF)), bits, (bits & strip) ? ", [fight]" : "",
                  sc5::frame::Count());
  }
}
