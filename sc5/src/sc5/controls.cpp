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
#include <fmt/format.h>

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
};

// The game's Control Settings (Options): the live copy at 0x82925630, 60 bytes a pad: four 15-byte records (the types
// A, B, C and the player's own), each the option of A B X Y LB LT RB RT in the menu's order, then at byte 12 the type
// chosen (the same in all four records); copied into the save image's +0x90 for a save (ownsave.cpp). An option is an
// index into the table at 0x82768E44, the game's buttons it presses: 0 nothing, A, B, K, G, A+G, B+G, A+B, K+G, A+K,
// B+K, A+B+K, A+B+K+G (13 of the 16 combinations). The battle's input task (sub_8213E538) copies the chosen record of
// a pad into the battle's layout (0x82911094 + 12 a pad, in the order of the bits of the table at 0x8276FC1C: LT 0x2,
// LB 0x4, RT 0x10, B 0x20, A 0x40, RB 0x80, Y 0x100, X 0x200), which its conversion sub_82172068 reads.
constexpr uint32_t kControlSettings = 0x82925630, kPadSettings = 60, kRecordSize = 15, kTypeAt = 12;
constexpr uint8_t kCombination[13] = {0x0, 0x1, 0x2, 0x4, 0x8, 0x9, 0xA, 0x3, 0xC, 0x5, 0x6, 0x7, 0xF};
// the record's buttons in the menu's order
constexpr int kMenuOrder[8] = {kA, kB, kX, kY, kLB, kLT, kRB, kRT};
// Type A, the console's default and a fresh save's, as the battle's layout keeps it (the table's order: LT B+G, LB A+G,
// RT A+B+K, B K, A G, RB A+B, Y B, X A)
constexpr uint8_t kTypeA[8] = {6, 5, 11, 3, 4, 7, 2, 1};
constexpr uint32_t kBattleLayout = 0x82911094, kBattleLayoutSize = 12;
// Type A's face buttons for each of the game's buttons: X horizontal attack (A), Y vertical (B), B kick (K), A guard (G)
constexpr uint16_t kFaceOf[4] = {input::kX, input::kY, input::kB, input::kA};

uint8_t* Guest(uint32_t a) { return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(a); }

// the record of the type the player chose for their own controller (pad 0: the first controller and the keyboard)
const uint8_t* OwnRecord(int* type_out = nullptr) {
  const uint8_t* pad = Guest(kControlSettings);
  int type = pad[kTypeAt];
  if (type < 0 || type > 3) type = 0;
  if (type_out) *type_out = type;
  return pad + type * kRecordSize;
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
    }
    // ([fight], the launcher's buttons in a fight until 0.1.7, is read no more: the game's own Control Settings say
    // what a button presses, online too)
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
        REXLOG_INFO("sc5 controls: from {} (keyboard {}, player 2's controller {})", path.string(),
                    m.keyboard ? "on" : "off", m.player2.empty() ? "the second plugged in" : "\"" + m.player2 + "\"");
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

void ToDefaultLayout(Pad& pad) {
  // the game's own buttons the player's eight pressed, by their own Control Settings
  const uint8_t* rec = OwnRecord();
  int game = 0;
  for (int i = 0; i < 8; i++) {
    const int c = kMenuOrder[i];
    const bool down = c == kLT   ? pad.left_trigger >= kTriggerPressed
                      : c == kRT ? pad.right_trigger >= kTriggerPressed
                                 : (pad.buttons & kControlBits[c]) != 0;
    if (down && rec[i] < 13) game |= kCombination[rec[i]];
  }
  // ... and nothing else of theirs: the face buttons that make those in Type A
  for (const int c : kMenuOrder)
    if (c != kLT && c != kRT) pad.buttons = uint16_t(pad.buttons & ~kControlBits[c]);
  pad.left_trigger = pad.right_trigger = 0;
  for (int i = 0; i < 4; i++)
    if (game >> i & 1) pad.buttons |= kFaceOf[i];
}

std::string FightText() {
  int type = 0;
  const uint8_t* rec = OwnRecord(&type);
  std::string out = fmt::format("Control Settings type {}:", type < 3 ? char('A' + type) : 'D');
  for (int i = 0; i < 8; i++)
    out += fmt::format(" {}={}", kControlIds[kMenuOrder[i]], rec[i] < 13 ? ComboText(kCombination[rec[i]]) : "?");
  return out;
}

}  // namespace sc5::controls

// The battle's conversion of a pad's buttons into the game's (sub_82172068, from the battle's input task sub_8213E538,
// twice a player a frame: the buttons held and those newly pressed; r3 the pad, r4 its bits as the pad manager keeps
// them; r3 back: kGame* with the d-pad and START let through). In a match of the netplay's (a spectator's too) every pad
// is converted with Type A, the console's default layout, whatever the Control Settings of this game's save give that
// pad: both players' inputs come in Type A's face buttons already (ToDefaultLayout, each player's own buttons by their
// own settings on their own PC), and the two games, whose saves may hold other settings, must convert them alike.
// The battle's layout is Type A for the call only (its own is back after it): offline the game's settings are as the
// player left them. Tests: SC5_DBG_FIGHT=1 logs each pad's game buttons newly pressed (its second call a frame).
REX_EXTERN(__imp__sub_82172068);
REX_HOOK_RAW(sub_82172068) {
  using namespace sc5::controls;
  const uint32_t pad = ctx.r3.u32, bits = ctx.r4.u32;
  uint8_t own[8];
  uint8_t* layout = pad < 4 ? Guest(kBattleLayout + pad * kBattleLayoutSize) : nullptr;
  const bool canonical = layout && sc5::net::Active();
  if (canonical) {
    std::memcpy(own, layout, 8);
    std::memcpy(layout, kTypeA, 8);
  }
  __imp__sub_82172068(ctx, base);
  if (canonical) std::memcpy(layout, own, 8);
  static const bool dbg = [] { const char* v = std::getenv("SC5_DBG_FIGHT"); return v && v[0] == '1'; }();
  if (dbg && pad < 4) {
    static int call[4] = {};
    if ((call[pad] ^= 1) == 0 && (ctx.r3.u32 & 0xF))
      REXLOG_INFO("sc5 controls: pad {} newly pressed game buttons {} (its bits {:08X}{}) at frame {}", pad,
                  ComboText(int(ctx.r3.u32 & 0xF)), bits, canonical ? ", Type A" : "", sc5::frame::Count());
  }
}
