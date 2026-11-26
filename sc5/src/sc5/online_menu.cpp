// The game's own online menus, see online_menu.h. The screens, the player's pad and the asks; online_draw.cpp draws
// them.
//
// The entry. AstralHome's top-level list has five entries in a fixed order (Offline Play 0, Xbox LIVE 1, Story 2,
// Creation 3, Options 4: the menu's cursor at +0x94, sc5/autopilot.cpp ReadHome), and the count is in its update's code
// (sub_8220B6A0: DOWN stops at 5, UP at 0) and in its movie, whose entries are the movie clips mc_sel_*: a sixth entry
// would mean a new clip in the menu's data and the update's limits patched. Xbox LIVE cannot work on a PC, so its place
// is ours: the label is table 1's string 2 ("$mXbox LIVE": $m is the menu's own style) and the description table 1's
// string 14 ("$z0.875/0.875/Play the game on Xbox LIVE ..."; $z the text's scale), both asked for through sub_82347670
// when the menu is made and its cursor comes to the entry (seen with SC5_DBG_TEXT_ALL=1, 2026-11-26); here they are
// ours while the game is at the main menu. A on the entry (the menu up, its state 6, the top level, the cursor on 1)
// never reaches the game: our menus open instead.
//
// The pad. While the menus are up the game gets a still pad (the autopilot's override at its hold point, HoldPad), and
// the menus read the player's own: the first controller and the keyboard as the launcher's controls give them
// (input::ReadPhysical(0)), or a test's script. A button that opened or closed the menus is held back from the game
// until it is let go. D-pad or left stick move (held: again after a third of a second, then ten times a second), A
// chooses, B goes back, X and Y as the legend says.
//
// The sounds are the game's own: sub_821DAD90(handle, id) plays a menu sound, as AstralHome's update does (8 the
// cursor, 10 a choice, 11 back).

#include "sc5/online_menu.h"

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "sc5/autopilot.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/testinput.h"
#include "sc5/ui_link.h"

REX_EXTERN(
    sub_821DAD90);  // a menu sound (r3 a 16-byte handle the call fills, r4 the sound: 8 cursor, 10 choice, 11 back)

namespace sc5::online {

const char* const kName = "SCV Online";

namespace {

// the main menu's entry: Xbox LIVE's place in AstralHome's list, and its texts (table 1: 2 the label, 14 the
// description)
constexpr int kEntry = 1;
constexpr uint32_t kTextTable = 1, kTextLabel = 2, kTextDesc = 14;
const char* const kEntryDesc =
    "Fight other players online with rollback netcode. Ranked and casual matches, lobbies, the leaderboard and "
    "online practice.";

enum class Screen { Top, Search, Lobbies, Create, Room, Board, After, Password };
enum TopItem { kRanked, kCasual, kLobbies, kPractice, kBoard, kSide, kBack, kTopItems };

bool g_on = false;
std::atomic<bool> g_open{false};
std::atomic<bool> g_swallow{false};  // the pad held back from the game until every button is let go
std::atomic<bool> g_showing{false};  // drawn this frame (the game at the main menu, the autopilot at its hold point)
bool g_parked = false;               // a park came: the series once back
bool g_test_opened = false;
Screen g_screen = Screen::Top;
int g_cursor = 0;
int g_top_cursor = 0;
std::string g_kind;       // the search asked for: ranked or casual
uint64_t g_asked_at = 0;  // the frame the last ask that changes screens went
// the frames the player left a search (cancel, decline) and a lobby: ui.json says so only once the launcher has taken
// the ask, so for a while its old word is not followed back into them
uint64_t g_left_search_at = 0, g_left_lobby_at = 0;
constexpr uint64_t kGrace = 150;
int g_side_pending = -1;  // 0 off, 1 left, 2 right: asked, ui.json not there yet
uint64_t g_side_at = 0;
int g_ready_pending = -1;  // the lobby's ready asked
uint64_t g_ready_at = 0;
int64_t g_message_seen = 0;
bool g_message_primed = false;  // the message ui.json had when the menus first read it is old news
std::string g_join;             // a lobby asked for (its code), until the lobby is there
uint64_t g_join_at = 0;
bool g_board_jump = false;  // the leaderboard asked for: the cursor goes to the player's own row
bool g_popup = false, g_popup_error = false;
std::string g_popup_head, g_popup_text;
// Create a Lobby's settings
int g_name_i = 0, g_mode_i = 0, g_koth = 0, g_private = 0, g_rounds_i = 2;
constexpr int kRounds[3] = {1, 3, 5};  // best of (lobby_create's rounds; 5 the launcher's default, first to 3)
// a locked lobby's password, entered with the pad: four digits, the one being changed, the lobby, to watch or play
int g_digits[4] = {0, 0, 0, 0};
int g_digit_at = 0;
std::string g_pw_code, g_pw_name;
bool g_pw_watch = false;
const char* const kLobbyNames[] = {
    "", "Casual Sets", "First to Three", "Training Room", "Newcomers Welcome", "Late Night Fights", "King of the Hill"};
constexpr int kLobbyNameCount = int(std::size(kLobbyNames));
enum CreateItem { kCName, kCMode, kCRounds, kCKoth, kCPrivate, kCCreate, kCBack, kCreateItems };

// the pad: each direction pressed, then again after 20 frames and every 6 after
struct Pressed {
  bool up = false, down = false, left = false, right = false, a = false, b = false, x = false, y = false;
};
int g_hold[4] = {0, 0, 0, 0};
bool g_was[4] = {true, true, true, true};  // A B X Y the frame before (held at first: no press)

std::mutex g_view_lock;
View g_view;

// the sound handle's guest memory (16 bytes)
uint32_t g_se = 0;

uint64_t Frame() { return frame::Count(); }

input::Pad OwnPad() {
  input::Pad pad;
  if (testinput::ScriptedPad(0, pad)) return pad;
  return input::ReadPhysical(0);
}

Pressed ReadPressed(const input::Pad& pad) {
  Pressed p;
  const bool dir[4] = {
      (pad.buttons & input::kDpadUp) || pad.ly > 16000, (pad.buttons & input::kDpadDown) || pad.ly < -16000,
      (pad.buttons & input::kDpadLeft) || pad.lx < -16000, (pad.buttons & input::kDpadRight) || pad.lx > 16000};
  bool* out[4] = {&p.up, &p.down, &p.left, &p.right};
  for (int i = 0; i < 4; i++) {
    g_hold[i] = dir[i] ? g_hold[i] + 1 : 0;
    *out[i] = g_hold[i] == 1 || (g_hold[i] > 20 && (g_hold[i] - 20) % 6 == 0);
  }
  const bool btn[4] = {(pad.buttons & input::kA) != 0, (pad.buttons & input::kB) != 0, (pad.buttons & input::kX) != 0,
                       (pad.buttons & input::kY) != 0};
  bool* bout[4] = {&p.a, &p.b, &p.x, &p.y};
  for (int i = 0; i < 4; i++) {
    *bout[i] = btn[i] && !g_was[i];
    g_was[i] = btn[i];
  }
  return p;
}

void Sound(PPCContext& ctx, uint8_t* base, int id) {
  if (!g_se) g_se = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16, 0x10);
  if (!g_se) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = g_se;
  ctx.r4.u64 = uint32_t(id);
  ctx.lr = 0x82189A40;  // as from main's loop
  sub_821DAD90(ctx, base);
  ctx = saved;
}

bool InHome() {
  const int seq = game::SequenceId();
  return seq == game::kSeqAstralHome || seq == 18;
}

// --- what the launcher says ---

bool Usable(const uilink::Ui& u) { return u.fresh && u.account.state == "signed_in"; }

// why online play is not there now, in plain words (empty: it is)
void Why(const uilink::Ui& u, std::string& head, std::string& text, bool& error) {
  head.clear();
  text.clear();
  error = false;
  if (!u.seen || !u.fresh) {
    head = "THE LAUNCHER IS NOT ANSWERING";
    text = u.seen
               ? "SCV Online's launcher stopped answering. Online play comes back when it does. Practice still works."
               : "Online play needs SCV Online's launcher: start the game with its PLAY button. Practice still works.";
    error = true;
  } else if (u.account.state == "signed_out") {
    head = "NOT SIGNED IN";
    text = "Sign in in the launcher first. Your name and password are typed there, never in the game.";
    error = true;
  } else if (u.account.state == "connecting") {
    head = "CONNECTING";
    text = "The launcher is connecting to the SCV Online server. Please wait.";
  } else if (u.account.state == "offline") {
    head = "OFFLINE";
    text = u.account.error.empty() ? "The SCV Online server cannot be reached. The launcher keeps trying."
                                   : "The SCV Online server cannot be reached: " + u.account.error;
    error = true;
  }
}

std::string Capital(std::string s) {
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = char(s[0] - 'a' + 'A');
  return s;
}

// a ranked title as the launcher sends it ("soul-warrior", or already in words) for the screen
std::string Title(const std::string& rank) {
  std::string out;
  bool start = true;
  for (char c : rank) {
    if (c == '-' || c == '_') {
      out += ' ';
      start = true;
      continue;
    }
    out += start && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    start = c == ' ';
  }
  return out;
}

std::string Clock(int secs) { return fmt::format("{}:{:02}", std::max(0, secs) / 60, std::max(0, secs) % 60); }

std::string LobbyName(int i, const uilink::Ui& u) {
  if (i == 0) return (u.account.name.empty() ? std::string("My") : u.account.name + "'s") + " Lobby";
  return kLobbyNames[i];
}

const char* SideName(int s) { return s == 1 ? "Left" : s == 2 ? "Right" : "Off"; }
int SideOf(const std::string& v) { return v == "left" ? 1 : v == "right" ? 2 : 0; }
const char* SideValue(int s) { return s == 1 ? "left" : s == 2 ? "right" : "off"; }

void Popup(const std::string& head, const std::string& text, bool error) {
  g_popup = true;
  g_popup_head = head;
  g_popup_text = text;
  g_popup_error = error;
  REXLOG_INFO("sc5 online: notice \"{}\": {}", head, text);
}

void Go(Screen s, int cursor = 0) {
  if (g_screen == Screen::Top) g_top_cursor = g_cursor;
  g_screen = s;
  g_cursor = s == Screen::Top ? g_top_cursor : cursor;
  static const char* kNames[] = {"top",  "search",      "lobbies",         "create",
                                 "room", "leaderboard", "after the match", "password"};
  REXLOG_INFO("sc5 online: screen {} at frame {}", kNames[int(s)], Frame());
}

const uilink::Member* Me(const uilink::Ui& u) {
  for (const uilink::Member& m : u.lobby.members)
    if (!u.account.name.empty() && m.name == u.account.name) return &m;
  return nullptr;
}

bool Searching(const uilink::Ui& u) { return u.queue.state != "idle"; }

// The menus close: a search is cancelled and a lobby left first (docs\PROTOCOL.md 3.8)
void Close(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, bool sound) {
  if (u.queue.state == "found")
    uilink::Ask("decline");
  else if (Searching(u) || (g_screen == Screen::Search && Frame() < g_asked_at + 300))
    uilink::Ask("cancel");
  if (u.lobby.present || !g_join.empty()) uilink::Ask("lobby_leave");
  g_left_search_at = g_left_lobby_at = Frame();
  g_join.clear();
  g_open = false;
  g_swallow = true;
  g_popup = false;
  if (sound) Sound(ctx, base, 11);
  REXLOG_INFO("sc5 online: the menus close at frame {}", Frame());
}

void Opened(PPCContext& ctx, uint8_t* base, const char* why) {
  g_open = true;
  g_swallow = true;
  g_screen = Screen::Top;
  g_cursor = g_top_cursor = 0;
  g_popup = false;
  for (int i = 0; i < 4; i++) g_was[i] = true;
  Sound(ctx, base, 10);
  REXLOG_INFO("sc5 online: the menus open ({}) at frame {}", why, Frame());
}

// --- each screen: its rows and what the pad does there ---

void TopRows(const uilink::Ui& u, View& v) {
  const bool ok = Usable(u);
  const int side = g_side_pending >= 0 ? g_side_pending : SideOf(u.side);
  v.title = kName;
  v.rows = {{"Ranked Match", "", "", ok},
            {"Casual Match", "", "", ok},
            {"Lobbies", "", "", ok},
            {"Practice"},
            {"Leaderboard", "", "", ok},
            {"Preferred Side", SideName(side), "", u.fresh, false, true},
            {"Back"}};
  static const char* kDesc[kTopItems] = {
      "Fight for rank and rating. A ranked set is first to three wins against a player near your level; the winner of "
      "each match keeps their fighter.",
      "Fight anyone for fun, as many matches as you both like against the same opponent. Nothing counts toward your "
      "rank.",
      "Open a room of your own or join another player's. Players take turns; King of the Hill keeps the winner in.",
      "Train on your own in the game's Training mode, the frame meter with you.",
      "The best fighters of SCV Online by rating, your own place marked.",
      "The side you start every fight on, whichever player you are. LEFT and RIGHT change it. Off: the side the match "
      "gives you.",
      "Return to Astral Home."};
  v.desc = kDesc[std::clamp(g_cursor, 0, kTopItems - 1)];
  v.legend = {{"UD", "Select"}, {"A", "OK"}, {"B", "Back"}};
  if (g_cursor == kSide) v.legend.insert(v.legend.begin() + 1, {"LR", "Change"});
}

void SetSide(int s) {
  g_side_pending = (s + 3) % 3;
  g_side_at = Frame();
  uilink::Ask("side", {{"value", SideValue(g_side_pending)}});
}

void TopInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  const int side = g_side_pending >= 0 ? g_side_pending : SideOf(u.side);
  if (g_cursor == kSide && (p.left || p.right)) {
    if (!u.fresh) return;
    SetSide(side + (p.right ? 1 : -1));
    Sound(ctx, base, 8);
    return;
  }
  if (p.b) {
    Close(ctx, base, u, true);
    return;
  }
  if (!p.a) return;
  const bool online = g_cursor == kRanked || g_cursor == kCasual || g_cursor == kLobbies || g_cursor == kBoard;
  if (online && !Usable(u)) {
    std::string head, text;
    bool error = false;
    Why(u, head, text, error);
    Popup(head, text, error);
    Sound(ctx, base, 11);
    return;
  }
  switch (g_cursor) {
    case kRanked:
    case kCasual:
      g_kind = g_cursor == kRanked ? "ranked" : "casual";
      uilink::Ask("queue", {{"kind", g_kind}});
      g_asked_at = Frame();
      Sound(ctx, base, 10);
      Go(Screen::Search);
      break;
    case kLobbies:
      uilink::Ask("lobbies");
      Sound(ctx, base, 10);
      Go(Screen::Lobbies);
      break;
    case kPractice:
      // the game's own Training (the autopilot's way there, then the player's): the menus close
      Sound(ctx, base, 10);
      Close(ctx, base, u, false);
      autopilot::ToTraining();
      break;
    case kBoard:
      uilink::Ask("leaderboard");
      g_board_jump = true;
      Sound(ctx, base, 10);
      Go(Screen::Board);
      break;
    case kSide:
      if (!u.fresh) {
        std::string head, text;
        bool error = false;
        Why(u, head, text, error);
        Popup(head, text, error);
        return;
      }
      SetSide(side + 1);
      Sound(ctx, base, 10);
      break;
    case kBack: Close(ctx, base, u, true); break;
  }
}

void SearchRows(const uilink::Ui& u, View& v) {
  const std::string kind = !u.queue.kind.empty() ? u.queue.kind : g_kind;
  v.title = kind == "ranked" ? "Ranked Match" : "Casual Match";
  v.stage = true;
  const std::string& st = u.queue.state;
  v.legend = {{"A", "OK"}};
  if (st == "found") {
    v.stage_head = "MATCH FOUND";
    v.stage_big = u.queue.opponent.empty() ? "An opponent" : u.queue.opponent;
    if (!u.queue.opponent_rank.empty()) v.stage_lines.push_back("Rank: " + Title(u.queue.opponent_rank));
    if (u.queue.ping > 0) v.stage_lines.push_back(fmt::format("Ping: {} ms", u.queue.ping));
    v.stage_sub = fmt::format("{} seconds left to accept", std::max(0, u.queue.accept_left));
    v.rows = {{"Accept"}, {"Decline"}};
    v.desc = g_cursor == 0 ? "Fight this player now." : "Turn this match down and stop searching.";
    v.legend = {{"UD", "Select"}, {"A", "OK"}, {"B", "Decline"}};
  } else if (st == "accepted") {
    v.stage_head = "MATCH ACCEPTED";
    v.stage_big = u.queue.opponent.empty() ? "Your opponent" : u.queue.opponent;
    v.stage_sub = "Waiting for the other player to accept";
    v.desc = "The match starts as soon as both players have accepted.";
    v.legend.clear();
  } else if (st == "connecting" || st == "starting") {
    v.stage_head = st == "connecting" ? "CONNECTING" : "THE MATCH IS STARTING";
    v.stage_big = u.queue.opponent.empty() ? "Your opponent" : u.queue.opponent;
    if (u.queue.ping > 0) v.stage_lines.push_back(fmt::format("Ping: {} ms", u.queue.ping));
    v.stage_sub = st == "connecting" ? "Reaching the other player's game" : "Get ready";
    v.desc = "The fight's character select comes next.";
    v.legend.clear();
  } else {
    v.stage_head = "SEARCHING FOR AN OPPONENT";
    v.stage_big = Clock(st == "searching" ? u.queue.secs : 0);
    v.stage_sub = st == "searching"
                      ? (kind == "ranked" ? "Looking for a player near your rating" : "Looking for a player")
                      : "Asking the launcher";
    v.rows = {{"Cancel"}};
    v.desc = "Stop searching and go back.";
    v.legend = {{"A", "OK"}, {"B", "Cancel"}};
  }
  if (!u.queue.text.empty()) v.stage_lines.push_back(u.queue.text);
}

void SearchInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  const std::string& st = u.queue.state;
  if (st == "found") {
    if (p.b || (p.a && g_cursor == 1)) {
      uilink::Ask("decline");
      Sound(ctx, base, 11);
      g_asked_at = g_left_search_at = Frame();
      Go(Screen::Top);
    } else if (p.a) {
      uilink::Ask("accept");
      Sound(ctx, base, 10);
    }
    return;
  }
  if (st == "accepted" || st == "connecting" || st == "starting") return;  // the match comes
  if (p.a || p.b) {
    uilink::Ask("cancel");
    Sound(ctx, base, 11);
    g_asked_at = g_left_search_at = Frame();
    Go(Screen::Top);
  }
}

// Lobbies: Refresh, Create a Lobby, then the open lobbies
constexpr int kLobbyFirst = 2;

std::string ModeName(const std::string& mode) { return mode == "sparring" ? "Online Practice" : "Versus"; }

void LobbiesRows(const uilink::Ui& u, View& v) {
  v.title = "Lobbies";
  v.rows = {{"Refresh"}, {"Create a Lobby"}};
  for (const uilink::LobbyRow& r : u.lobbies) {
    Row row;
    row.label = r.name.empty() ? r.code : r.name;
    row.detail = fmt::format("{}{}/{}", r.locked ? "LOCKED  " : "", r.players, r.max > 0 ? r.max : 2);
    v.rows.push_back(row);
  }
  if (g_cursor == 0) {
    const int64_t age = u.lobbies_at > 0 && u.t >= u.lobbies_at ? (u.t - u.lobbies_at) / 1000 : -1;
    v.desc = u.lobbies.empty() ? "No lobby is open just now. Refresh, or create one of your own."
             : age >= 0        ? fmt::format("{} open. The list is {} second{} old.",
                                             u.lobbies.size() == 1 ? std::string("One lobby is")
                                                                   : fmt::format("{} lobbies are", u.lobbies.size()),
                                             age, age == 1 ? "" : "s")
                               : "Ask the server for the open lobbies again.";
  } else if (g_cursor == 1) {
    v.desc =
        "Open a lobby of your own: its name from a list, versus or Online Practice, King of the Hill, private or "
        "not.";
  } else if (size_t(g_cursor - kLobbyFirst) < u.lobbies.size()) {
    const uilink::LobbyRow& r = u.lobbies[size_t(g_cursor - kLobbyFirst)];
    v.desc =
        fmt::format("Host {}. {}{}. Code {}.{}{}", r.host.empty() ? "unknown" : r.host, ModeName(r.mode),
                    r.koth ? ", King of the Hill" : "", r.code, r.region.empty() ? "" : " Region " + r.region + ".",
                    r.locked ? " Locked: it needs its password, which its host can tell you." : "");
  }
  if (!g_join.empty()) v.desc = "Joining lobby " + g_join + " ...";
  v.legend = {{"UD", "Select"}, {"A", g_cursor >= kLobbyFirst ? "Join" : "OK"}};
  if (g_cursor >= kLobbyFirst) v.legend.push_back({"X", "Watch"});
  v.legend.push_back({"B", "Back"});
}

void LobbiesInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  if (p.b) {
    g_join.clear();
    Sound(ctx, base, 11);
    Go(Screen::Top);
    return;
  }
  if (!p.a && !p.x) return;
  if (g_cursor == 0 && p.a) {
    uilink::Ask("lobbies");
    Sound(ctx, base, 10);
  } else if (g_cursor == 1 && p.a) {
    Sound(ctx, base, 10);
    Go(Screen::Create);
  } else if (size_t(g_cursor - kLobbyFirst) < u.lobbies.size()) {
    const uilink::LobbyRow& r = u.lobbies[size_t(g_cursor - kLobbyFirst)];
    if (r.locked) {
      // its password first: four digits with the pad
      g_pw_code = r.code;
      g_pw_name = r.name.empty() ? r.code : r.name;
      g_pw_watch = p.x;
      for (int& d : g_digits) d = 0;
      g_digit_at = 0;
      Sound(ctx, base, 10);
      Go(Screen::Password);
      return;
    }
    g_join = r.code;
    g_join_at = Frame();
    if (p.x)
      uilink::Ask("lobby_join", {{"code", r.code}, {"watch", "1"}});
    else
      uilink::Ask("lobby_join", {{"code", r.code}});
    Sound(ctx, base, 10);
  }
}

// A locked lobby's password: four digits, LEFT and RIGHT to the one to change, UP and DOWN change it, A joins
void PasswordRows(const uilink::Ui& u, View& v) {
  (void)u;
  v.title = "Lobby Password";
  v.stage = true;
  v.stage_head = "ENTER THE PASSWORD";
  for (int d : g_digits) v.digits += char('0' + d);
  v.digit_at = g_digit_at;
  v.stage_sub = g_pw_name;
  v.stage_lines.push_back("Its host sees it in the lobby's room.");
  v.rows = {{g_pw_watch ? "Watch the Lobby" : "Join the Lobby"}};
  v.desc = !g_join.empty() ? "Joining lobby " + g_join + " ..."
                           : "UP and DOWN change a digit, LEFT and RIGHT go to the next one. A joins with it.";
  v.legend = {{"UD", "Digit"}, {"LR", "Move"}, {"A", "Join"}, {"B", "Back"}};
}

void PasswordInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  (void)u;
  if (p.b) {
    g_join.clear();
    Sound(ctx, base, 11);
    Go(Screen::Lobbies);
    return;
  }
  if (p.left || p.right) {
    g_digit_at = std::clamp(g_digit_at + (p.right ? 1 : -1), 0, 3);
    Sound(ctx, base, 8);
  } else if (p.up || p.down) {
    g_digits[g_digit_at] = (g_digits[g_digit_at] + (p.up ? 1 : 9)) % 10;
    Sound(ctx, base, 8);
  } else if (p.a && g_join.empty()) {
    std::string pw;
    for (int d : g_digits) pw += char('0' + d);
    g_join = g_pw_code;
    g_join_at = Frame();
    if (g_pw_watch)
      uilink::Ask("lobby_join", {{"code", g_pw_code}, {"watch", "1"}, {"password", pw}});
    else
      uilink::Ask("lobby_join", {{"code", g_pw_code}, {"password", pw}});
    Sound(ctx, base, 10);
  }
}

void CreateRows(const uilink::Ui& u, View& v) {
  v.title = "Create a Lobby";
  v.rows = {{"Name", LobbyName(g_name_i, u), "", true, false, true},
            {"Mode", g_mode_i ? "Online Practice" : "Versus", "", true, false, true},
            {"Rounds", fmt::format("Best of {}", kRounds[g_rounds_i]), "", true, false, true},
            {"King of the Hill", g_koth ? "On" : "Off", "", true, false, true},
            {"Private", g_private ? "Yes" : "No", "", true, false, true},
            {"Create Lobby"},
            {"Back"}};
  static const char* kDesc[kCreateItems] = {
      "The name others see in the list.",
      "Versus: matches that count for nothing but the lobby's own wins. Online Practice: endless rounds, health "
      "refilled.",
      "The rounds of each match: best of 1, 3 or 5 (first to 3, as the console's default).",
      "On: the winner stays in and the next player in line takes on the winner.",
      "Yes: the lobby is locked with a 4-digit password the launcher makes up; the room shows it to you, for your "
      "friends to join with.",
      "Open the lobby and wait for players in it.",
      "Back to the lobbies."};
  v.desc = kDesc[std::clamp(g_cursor, 0, kCreateItems - 1)];
  if (!g_join.empty()) v.desc = "Opening the lobby ...";
  v.legend = {{"UD", "Select"}, {"LR", "Change"}, {"A", "OK"}, {"B", "Back"}};
}

void CreateInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  if (p.b || (p.a && g_cursor == kCBack)) {
    Sound(ctx, base, 11);
    Go(Screen::Lobbies, 1);
    return;
  }
  const int step = p.left ? -1 : (p.right || p.a) ? 1 : 0;
  if (!step) return;
  switch (g_cursor) {
    case kCName: g_name_i = (g_name_i + step + kLobbyNameCount) % kLobbyNameCount; break;
    case kCMode: g_mode_i ^= 1; break;
    case kCRounds: g_rounds_i = (g_rounds_i + step + 3) % 3; break;
    case kCKoth: g_koth ^= 1; break;
    case kCPrivate: g_private ^= 1; break;
    case kCCreate:
      if (!p.a) return;
      uilink::Ask("lobby_create", {{"name", LobbyName(g_name_i, u)},
                                   {"mode", g_mode_i ? "sparring" : "versus"},
                                   {"koth", g_koth ? "1" : "0"},
                                   {"private", g_private ? "1" : "0"},
                                   {"rounds", fmt::format("{}", kRounds[g_rounds_i])}});
      g_join = "new";
      g_join_at = Frame();
      Sound(ctx, base, 10);
      return;
    default: return;
  }
  Sound(ctx, base, 8);
}

void RoomRows(const uilink::Ui& u, View& v) {
  const uilink::Lobby& l = u.lobby;
  v.title = l.name.empty() ? "Lobby" : l.name;
  const uilink::Member* me = Me(u);
  const bool watching = me && me->role == "spectator";
  const bool ready = g_ready_pending >= 0 ? g_ready_pending == 1 : l.ready;
  if (!watching) v.rows.push_back({"Ready", ready ? "Yes" : "No", "", true, false, true});
  v.rows.push_back({"Leave Lobby"});
  const bool on_ready = !watching && g_cursor == 0;
  v.desc = !l.text.empty() ? l.text
           : on_ready      ? "Say you are ready for the lobby's next match. A changes it."
                           : "Leave this lobby and go back to the list.";
  v.card_head = fmt::format("Lobby {} - {}{}{}", l.code, ModeName(l.mode), l.koth ? ", King of the Hill" : "",
                            l.locked ? ", locked" : "");
  if (!l.password.empty()) {
    Line pw;  // the host's own lobby: its password, to tell friends
    pw.name = "Password";
    pw.value = l.password;
    pw.strong = true;
    v.card.push_back(pw);
  }
  for (const uilink::Member& m : l.members) {
    Line line;
    line.name = m.name + (m.name == l.host ? " (host)" : "");
    if (m.role == "spectator") {
      line.value = "WATCHING";
    } else {
      line.mark = m.ready ? 1 : 2;
      line.value = m.ready ? "READY" : "NOT READY";
      if (m.wins > 0) line.value += fmt::format("  {} {}", m.wins, m.wins == 1 ? "win" : "wins");
    }
    line.strong = me && &m == me;
    v.card.push_back(line);
  }
  if (!u.series.kind.empty() && u.series.kind == "lobby" && !u.series.opponent.empty())
    v.card.push_back({fmt::format("Last: you {} - {} {}", u.series.mine, u.series.theirs, u.series.opponent), ""});
  v.legend = {{"UD", "Select"}, {"A", "OK"}, {"B", "Leave"}};
}

void RoomInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  const uilink::Member* me = Me(u);
  const bool watching = me && me->role == "spectator";
  const bool on_ready = !watching && g_cursor == 0;
  if (p.b || (p.a && !on_ready)) {
    uilink::Ask("lobby_leave");
    uilink::Ask("lobbies");
    g_left_lobby_at = Frame();
    Sound(ctx, base, 11);
    Go(Screen::Lobbies);
    return;
  }
  if (on_ready && (p.a || p.left || p.right)) {
    const bool ready = g_ready_pending >= 0 ? g_ready_pending == 1 : u.lobby.ready;
    g_ready_pending = ready ? 0 : 1;
    g_ready_at = Frame();
    uilink::Ask("lobby_ready", {{"value", ready ? "0" : "1"}});
    Sound(ctx, base, 10);
  }
}

void BoardRows(const uilink::Ui& u, View& v) {
  v.title = "Leaderboard";
  for (const uilink::BoardRow& r : u.board) {
    Row row;
    row.label = fmt::format("{}. {}", r.place, r.name);
    row.detail = fmt::format("{}", r.elo);
    row.mine = !u.account.name.empty() && r.name == u.account.name;
    v.rows.push_back(row);
  }
  if (v.rows.empty()) {
    v.rows.push_back({"Waiting for the leaderboard ..."});
    v.desc = "The launcher asks the server for it.";
  } else if (size_t(g_cursor) < u.board.size()) {
    const uilink::BoardRow& r = u.board[size_t(g_cursor)];
    v.desc = fmt::format("{}{}: rating {}, {} wins and {} losses.", r.name, r.rank.empty() ? "" : ", " + Title(r.rank),
                         r.elo, r.wins, r.losses);
  }
  v.legend = {{"UD", "Select"}, {"X", "Refresh"}, {"B", "Back"}};
}

void BoardInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  (void)u;
  if (p.b) {
    Sound(ctx, base, 11);
    Go(Screen::Top);
  } else if (p.x) {
    uilink::Ask("leaderboard");
    g_board_jump = true;
    Sound(ctx, base, 10);
  }
}

bool SeriesGoing(const uilink::Ui& u) { return !u.series.kind.empty() && u.series.state != "over"; }

void AfterRows(const uilink::Ui& u, View& v) {
  const uilink::Series& s = u.series;
  v.title = s.kind == "ranked"   ? "Ranked Set"
            : s.kind == "casual" ? "Casual Match"
            : s.kind == "lobby"  ? "Lobby"
                                 : kName;
  v.stage = true;
  if (s.kind.empty()) {
    v.stage_head = "THE MATCH IS OVER";
    v.stage_big = "";
  } else {
    v.stage_head = s.state == "over"    ? (s.kind == "ranked" ? "THE SET IS OVER" : "THE SERIES IS OVER")
                   : s.kind == "ranked" ? "THE SET SO FAR"
                                        : "THE SCORE SO FAR";
    v.stage_big = fmt::format("You {}  -  {} {}", s.mine, s.theirs, s.opponent.empty() ? "Opponent" : s.opponent);
    if (s.to > 0) v.stage_sub = fmt::format("First to {} wins", s.to);
    if (s.state == "choosing")
      v.stage_lines.push_back("The last match's loser is choosing the next match");
    else if (s.state == "waiting")
      v.stage_lines.push_back("Waiting for the other player");
  }
  if (!s.text.empty()) v.stage_lines.push_back(s.text);
  if (SeriesGoing(u)) {
    v.rows = {{"Leave"}};
    v.desc = s.kind == "ranked" ? "Leave the set. Leaving after a match has counted loses the set."
                                : "Leave this series of matches. The other player is told.";
    v.legend = {{"A", "Leave"}};
  } else {
    v.rows = {{"Continue"}};
    v.desc = "Back to the online menu.";
    v.legend = {{"A", "OK"}, {"B", "Back"}};
  }
}

void AfterInput(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p) {
  if (SeriesGoing(u)) {
    if (p.a) {
      uilink::Ask("leave");
      Sound(ctx, base, 11);
      Go(Screen::Top);
    }
    return;
  }
  if (p.a || p.b) {
    Sound(ctx, base, p.a ? 10 : 11);
    Go(Screen::Top);
  }
}

// the account on the right, the banner, the legend: every screen
void Common(const uilink::Ui& u, View& v) {
  v.name = !u.account.name.empty() ? u.account.name : u.account.state == "signed_out" ? "Not signed in" : kName;
  if (Usable(u)) {
    v.rank = u.account.rank.empty() ? "No rank yet" : Title(u.account.rank);
    v.account.push_back({"Rating", fmt::format("{}", u.account.elo)});
    v.account.push_back({"Wins", fmt::format("{}", u.account.wins)});
    v.account.push_back({"Losses", fmt::format("{}", u.account.losses)});
    v.account.push_back({"Players online", fmt::format("{}", u.server.online)});
    v.account.push_back({"In matches", fmt::format("{}", u.server.in_matches)});
  } else {
    v.rank = !u.seen || !u.fresh ? "Launcher not answering"
                                 : Capital(u.account.state == "signed_out" ? "signed out" : u.account.state);
  }
  if (!u.update_version.empty())
    v.news.push_back({"UPDATE READY", u.update_text.empty() ? "Version " + u.update_version +
                                                                  " is ready. It is installed when you close the game."
                                                            : u.update_text});
  if (!u.server.notice.empty()) v.news.push_back({"SERVER NOTICE", u.server.notice});
  std::string head, text;
  bool error = false;
  Why(u, head, text, error);
  v.banner_head = head;
  v.banner_text = text;
  v.banner_error = error;
  v.popup = g_popup;
  v.popup_error = g_popup_error;
  v.popup_head = g_popup_head;
  v.popup_text = g_popup_text;
  if (g_popup) v.legend = {{"A", "Close"}};
}

// The launcher's own changes of screen: a search going on shows the search (one the launcher started again by itself
// after a series too); a lobby joined shows its room; a room gone shows the list; a search gone back to idle
// (cancelled, declined, the server's word) the top level
void Follow(const uilink::Ui& u) {
  // a lobby: the one just left is not followed back into, nor an old one while another is being joined
  const bool lobby_left = Frame() < g_left_lobby_at + kGrace && g_join.empty();
  const bool other_lobby = !g_join.empty() && g_join != "new" && u.lobby.code != g_join;
  // (only on the launcher's word while it answers: an old reading, the launcher gone or slow to confirm a leave, pulled
  // a player who had left the room back into it every 2.5 s)
  if (u.lobby.present && u.fresh && !lobby_left && !other_lobby) {
    if (g_screen != Screen::Room && g_screen != Screen::After) {
      g_join.clear();
      Go(Screen::Room);
    }
  } else if (g_screen == Screen::Room && !u.lobby.present) {
    Go(Screen::Lobbies);
  }
  if (Searching(u) && u.fresh && Frame() > g_left_search_at + kGrace && g_screen != Screen::Search &&
      g_screen != Screen::Room && !(g_screen == Screen::After && SeriesGoing(u))) {
    g_kind = u.queue.kind;
    Go(Screen::Search);
  }
  // the launcher stopped answering while a search or a match on its way was up: nothing more will come of it here
  // (a match already accepted takes no button, so the screen had no way out but closing the game)
  if (g_screen == Screen::Search && u.seen && !u.fresh) {
    Popup("SEARCH ENDED", "SCV Online's launcher stopped answering, so the search has ended. Start it again and search "
                          "once more.",
          true);
    g_left_search_at = Frame();
    Go(Screen::Top);
  }
  if (g_screen == Screen::Search && !Searching(u) && Frame() > g_asked_at + 180) {
    if (!u.queue.text.empty()) Popup("SEARCH ENDED", u.queue.text, false);
    Go(Screen::Top);
  }
  if (!g_join.empty() && Frame() > g_join_at + 900 && !u.lobby.present) {
    Popup("NO ANSWER",
          g_join == "new" ? "The lobby could not be opened. Please try again."
                          : "The lobby did not answer. It may have closed.",
          true);
    g_join.clear();
  }
  if (g_side_pending >= 0 && (SideOf(u.side) == g_side_pending || Frame() > g_side_at + 300)) g_side_pending = -1;
  if (g_ready_pending >= 0 && ((u.lobby.ready ? 1 : 0) == g_ready_pending || Frame() > g_ready_at + 300))
    g_ready_pending = -1;
  if (g_screen == Screen::Board && g_board_jump && !u.board.empty()) {
    g_board_jump = false;
    for (size_t i = 0; i < u.board.size(); i++)
      if (!u.account.name.empty() && u.board[i].name == u.account.name) g_cursor = int(i);
  }
  // the launcher's message, once per id (one already there when the menus first read ui.json is old news)
  if (!g_message_primed && u.seen) {
    g_message_primed = true;
    g_message_seen = u.message.id;
  }
  if (u.message.id != g_message_seen && !u.message.text.empty()) {
    g_message_seen = u.message.id;
    Popup(u.message.kind == "error" ? "ERROR" : "NOTICE", u.message.text, u.message.kind == "error");
    // an ask the launcher refused (it says why in an error): a lobby asked for is not coming
    if (u.message.kind == "error") g_join.clear();
  }
}

View Build(const uilink::Ui& u) {
  View v;
  switch (g_screen) {
    case Screen::Top: TopRows(u, v); break;
    case Screen::Search: SearchRows(u, v); break;
    case Screen::Lobbies: LobbiesRows(u, v); break;
    case Screen::Create: CreateRows(u, v); break;
    case Screen::Room: RoomRows(u, v); break;
    case Screen::Board: BoardRows(u, v); break;
    case Screen::After: AfterRows(u, v); break;
    case Screen::Password: PasswordRows(u, v); break;
  }
  g_cursor = v.rows.empty() ? 0 : std::clamp(g_cursor, 0, int(v.rows.size()) - 1);
  v.cursor = g_cursor;
  Common(u, v);
  return v;
}

void Input(PPCContext& ctx, uint8_t* base, const uilink::Ui& u, const Pressed& p, int rows) {
  if (g_popup) {
    if (p.a || p.b) {
      g_popup = false;
      Sound(ctx, base, 11);
    }
    return;
  }
  if (rows > 0 && (p.up || p.down) && g_screen != Screen::Password) {
    const int before = g_cursor;
    g_cursor = std::clamp(g_cursor + (p.down ? 1 : -1), 0, rows - 1);
    if (g_cursor != before) Sound(ctx, base, 8);
  }
  switch (g_screen) {
    case Screen::Top: TopInput(ctx, base, u, p); break;
    case Screen::Search: SearchInput(ctx, base, u, p); break;
    case Screen::Lobbies: LobbiesInput(ctx, base, u, p); break;
    case Screen::Create: CreateInput(ctx, base, u, p); break;
    case Screen::Room: RoomInput(ctx, base, u, p); break;
    case Screen::Board: BoardInput(ctx, base, u, p); break;
    case Screen::After: AfterInput(ctx, base, u, p); break;
    case Screen::Password: PasswordInput(ctx, base, u, p); break;
  }
}

void Publish(View v) {
  std::lock_guard<std::mutex> lock(g_view_lock);
  g_view = std::move(v);
}

}  // namespace

void Init() {
  const char* menus = std::getenv("SC5_MENUS");
  if (!menus || std::strcmp(menus, "1") != 0) return;
  uilink::Init();
  g_on = uilink::On();
  if (g_on) REXLOG_INFO("sc5 online: the game's own online menus (SC5_MENUS=1), the main menu's entry \"{}\"", kName);
}

bool On() { return g_on; }
bool Open() { return g_on && g_open.load(); }

void OnPark() {
  if (!g_on) return;
  g_parked = true;
}

bool HoldPad(uint32_t user, input::Pad& out) {
  if (!g_on || user >= 2) return false;
  if (!g_open.load() && !g_swallow.load()) return false;
  out = input::Pad{};
  out.connected = true;
  return true;
}

uint32_t Text(PPCContext& ctx, uint8_t* base, uint32_t table, uint32_t index) {
  if (!g_on) return 0;
  auto put = [base](const std::string& s) {
    const uint32_t at = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(uint32_t(s.size() + 1), 0x10);
    if (at) {
      std::memcpy(base + at, s.data(), s.size());
      base[at + s.size()] = 0;
    }
    return at;
  };
  if (table != kTextTable || (index != kTextLabel && index != kTextDesc) || !InHome()) {
    // any other text of the game's that names Xbox LIVE (the player's card: "Total Xbox LIVE Play Time") names
    // online play instead, the rest of the string and its markup as they are; each string looked at once
    static std::mutex lock;
    static std::map<uint64_t, uint32_t> swapped;
    const uint32_t text = ctx.r3.u32;
    if (!text) return 0;
    std::lock_guard<std::mutex> hold(lock);
    const uint64_t key = uint64_t(table) << 32 | index;
    if (const auto it = swapped.find(key); it != swapped.end()) return it->second;
    std::string s;
    for (uint32_t a = text; s.size() < 400 && base[a]; a++) s += char(base[a]);
    uint32_t ours = 0;
    if (const size_t at = s.find("Xbox LIVE"); at != std::string::npos && s.size() < 400) {
      s.replace(at, 9, "Online");
      ours = put(s);
    }
    swapped[key] = ours;
    return ours;
  }
  // the game's markup kept: $m the menu's style for the label, $z the description's scale
  static uint32_t label = 0, desc = 0;
  if (!label) {
    label = put(std::string("$m") + kName);
    desc = put(std::string("$z0.875/0.875/") + kEntryDesc);
  }
  return index == kTextLabel ? label : desc;
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (!g_on) return;
  const bool holding = autopilot::Holding();
  const bool showing = g_open && holding && InHome();
  uilink::Poll(showing);
  const uilink::Ui u = uilink::Snapshot();
  const input::Pad pad = OwnPad();
  const bool any = pad.buttons != 0 || std::abs(pad.lx) > 16000 || std::abs(pad.ly) > 16000;
  if (g_swallow && !any) g_swallow = false;
  int depth = 0, top = 0;
  const bool at_menu = holding && InHome() && autopilot::HomeCursor(depth, top);
  if (!g_open) {
    g_showing = false;
    Publish(View{});
    const bool a = (pad.buttons & input::kA) != 0;
    const bool a_pressed = a && !g_was[0];
    g_was[0] = a;
    // the player's A on the entry: the menus instead of Xbox LIVE
    if (at_menu && depth == 0 && top == kEntry && a_pressed && !g_swallow) {
      Opened(ctx, base, "the player's A on the entry");
      return;
    }
    // tests: SC5_TEST_ONLINE_MENU=1 opens them once the game is first at its hold point, as the player's A would
    static const bool test = [] {
      const char* v = std::getenv("SC5_TEST_ONLINE_MENU");
      return v && v[0] == '1';
    }();
    if (test && !g_test_opened && at_menu && depth == 0) {
      g_test_opened = true;
      Opened(ctx, base, "SC5_TEST_ONLINE_MENU");
    }
    return;
  }
  if (!showing) {
    // a match (or the way to one): the menus wait, shown again once the game is back at the main menu
    g_showing = false;
    Publish(View{});
    for (int i = 0; i < 4; i++) {
      g_was[i] = true;
      g_hold[i] = 0;
    }
    return;
  }
  if (!g_showing) {
    g_showing = true;
    g_swallow = true;  // a button still down from the fight's end is no press here
  }
  if (g_parked) {
    // back from a match (or a park at the menus themselves): its series, or the lobby's room
    g_parked = false;
    g_popup = false;
    if (u.lobby.present)
      Go(Screen::Room);
    else if (!u.series.kind.empty())
      Go(Screen::After);
    else
      Go(Screen::Top);
    REXLOG_INFO("sc5 online: back from the match: {}",
                u.series.kind.empty()
                    ? "no series"
                    : "the series " + u.series.kind +
                          fmt::format(" {} - {} ({})", u.series.mine, u.series.theirs, u.series.state));
  }
  Follow(u);
  // tests: SC5_TEST_ONLINE_ACCEPT=1 accepts a match found, as the player's A on Accept would
  static const bool test_accept = [] {
    const char* v = std::getenv("SC5_TEST_ONLINE_ACCEPT");
    return v && v[0] == '1';
  }();
  static bool test_accepted = false;
  if (test_accept && g_screen == Screen::Search && u.queue.state == "found") {
    if (!test_accepted) uilink::Ask("accept");
    test_accepted = true;
  } else if (u.queue.state != "found") {
    test_accepted = false;
  }
  const Pressed p = ReadPressed(pad);
  View v = Build(u);
  if (!g_swallow) {
    Input(ctx, base, u, p, int(v.rows.size()));
    if (g_open) v = Build(u);
  }
  v.shown = g_open && holding;
  Publish(std::move(v));
}

View ViewNow() {
  std::lock_guard<std::mutex> lock(g_view_lock);
  return g_view;
}

}  // namespace sc5::online
