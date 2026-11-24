// The game's side of the launcher protocol (docs\PROTOCOL.md 3), adapted from MK9 Online's mk9/link.cpp. With
// SC5_NET=1 the game boots to its main menu and waits there (the hold point, "ready"), takes commands from cmd.txt
// (claimed by renaming it to cmd.txt.taken) and reports its state in status.txt (written to a .tmp file and moved
// over it):
//
//   start game=sc5 mode=versus side=1|2 local_port=N remote=ip:port delay=N seed=N p1=.. p2=.. rounds=1|3|5
//         ranked=0|1 match=<id>
//            p1/p2: a roster id (data\sc5.json), the game's character id with a costume ("21.1"), or "-": that side's
//            player picks on the game's own select screen and the other game follows (sc5/autopilot.h)
//   park     back to the hold point (from the result: Return to Astral Home)
//   rematch [seed=N] [match=<id>] [p1=.. p2=..] [stage=keep]   the same fighters again (or new ones): VS Battle's own
//            Rematch for the same two on the same stage, else through Character Select and the stage select
//   quit     exit
//   choose=1 (start, rematch): the match's loser chooses the next one on the result screen (a lobby's match)
//
// status.txt: booting, ready, starting match=<id>, in_match match=<id>, result winner=N p1_rounds=N p2_rounds=N
// p1_char=C.c p2_char=C.c match=<id>, chose choice=rematch|select match=<id> (choose=1: the loser's choice on the
// result screen), desync frame=N match=<id>, disconnected match=<id>, error <text>.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sc5/link.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "sc5/autopilot.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/net/netplay.h"
#include "sc5/select.h"
#include "sc5/sparring.h"
#include "sc5/versus.h"

namespace sc5::link {
namespace {

bool g_on = false;
std::wstring g_cmd, g_status;
std::string g_last_status;
std::string g_match = "none";
bool g_in_match = false;  // a start was taken and not parked
bool g_reported = false;  // result / desync / disconnected written for this match
bool g_error_shown = false;
autopilot::Plan g_plan;
net::Settings g_net;
std::string g_mode = "versus";  // the present start's mode

std::wstring Widen(const std::string& s) {
  std::wstring w;
  for (unsigned char c : s) w += wchar_t(c);
  return w;
}

std::wstring EnvW(const wchar_t* name) {
  wchar_t buf[4096];
  const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

void WriteStatus(const std::string& line) {
  if (!g_on || line == g_last_status) return;
  g_last_status = line;
  g_error_shown = line.rfind("error", 0) == 0;
  const std::wstring tmp = g_status + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  const std::string text = line + "\n";
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  for (int i = 0; i < 200; i++) {
    if (MoveFileExW(tmp.c_str(), g_status.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    Sleep(5);
  }
  REXLOG_INFO("sc5 link: status {}", line);
}

bool g_taken_unread = false;  // cmd.txt.taken claimed and not read yet

bool ClaimCommand(std::string& out) {
  const std::wstring taken = g_cmd + L".taken";
  if (!g_taken_unread) {
    if (!MoveFileExW(g_cmd.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    g_taken_unread = true;
  }
  // Another program (a virus scanner) can hold the file it just saw renamed for a moment: a test game once lost its
  // quit that way. The command waits for the next try then, and no newer one is claimed over it.
  FILE* f = nullptr;
  for (int i = 0; i < 10 && !(f = _wfopen(taken.c_str(), L"rb")); i++) Sleep(2);
  if (!f) {
    static uint64_t warned = 0;
    if (frame::Count() > warned + 300) {
      warned = frame::Count();
      REXLOG_WARN("sc5 link: cmd.txt.taken cannot be read yet (error {}): trying again", GetLastError());
    }
    return false;
  }
  g_taken_unread = false;
  char buf[2048];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  out = buf;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return true;
}

struct Command {
  std::string verb;
  std::map<std::string, std::string> keys;
};

Command Parse(const std::string& line) {
  Command c;
  std::istringstream in(line);
  in >> c.verb;
  for (std::string tok; in >> tok;) {
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) c.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
  }
  return c;
}

std::string Key(const Command& c, const char* k, const char* def = "") {
  auto it = c.keys.find(k);
  return it == c.keys.end() ? def : it->second;
}

// a start line's pick: "-" (that side's player picks), a roster id, the game's character id with a costume, or a created
// character (c.<its checksum>, as a result line names it: its player's own game picks it, the other follows)
bool ParsePick(const std::string& v, int& slot, int& costume, uint32_t& created) {
  slot = -1;
  costume = -1;
  created = 0;
  if (v.empty() || v == "-") return true;
  if (v.size() == 10 && v[0] == 'c' && v[1] == '.') {
    char* end = nullptr;
    created = uint32_t(std::strtoul(v.c_str() + 2, &end, 16));
    return end && !*end && created;
  }
  if (std::isdigit(uint8_t(v[0]))) {
    slot = select::SlotOfCharacter(std::atoi(v.c_str()));
    if (const size_t dot = v.find('.'); dot != std::string::npos) costume = std::atoi(v.c_str() + dot + 1);
  } else {
    slot = select::SlotOfRosterId(v);
  }
  return slot >= 0;
}

// a start that came before the game was at its hold point: taken once it is
Command g_waiting_start;
bool g_start_waits = false;

// a spectator's start (watch=1): the match comes from the launcher's spectator feed
void StartWatch(const Command& c);

void Start(const Command& c) {
  if (!autopilot::AtHoldPoint()) {
    if (!g_start_waits) REXLOG_INFO("sc5 link: the start waits until the game is at its hold point");
    g_waiting_start = c;
    g_start_waits = true;
    return;
  }
  g_start_waits = false;
  const std::string mode = Key(c, "mode", "versus");
  if (Key(c, "watch") == "1") {
    StartWatch(c);
    return;
  }
  if (mode != "versus" && mode != "sparring" && mode != "practice") {
    WriteStatus("error bad command: no mode " + mode);
    return;
  }
  autopilot::Plan plan;
  plan.mode = mode == "practice"   ? autopilot::Mode::Practice
              : mode == "sparring" ? autopilot::Mode::Sparring
                                   : autopilot::Mode::Versus;
  for (int s = 0; s < 2; s++) {
    const std::string v = Key(c, s ? "p2" : "p1", "-");
    if (!ParsePick(v, plan.slots[s], plan.costumes[s], plan.created[s])) {
      WriteStatus("error bad command: no character " + v);
      return;
    }
  }
  const int rounds = std::atoi(Key(c, "rounds", "3").c_str());
  plan.wins = std::max(1, (rounds + 1) / 2);
  plan.side = Key(c, "side", "1") == "2" ? 1 : 0;
  plan.choose = Key(c, "choose") == "1" && plan.mode == autopilot::Mode::Versus;
  g_mode = mode;
  if (plan.mode == autopilot::Mode::Practice) {
    // offline practice from the hold point: Training, the start line's fighters (the player's and the dummy)
    if (plan.slots[0] < 0 || plan.slots[1] < 0) {
      WriteStatus("error bad command: practice needs p1 and p2");
      return;
    }
    g_match = "practice";
    g_plan = plan;
    g_in_match = true;
    g_reported = false;
    autopilot::Begin(plan);
    REXLOG_INFO("sc5 link: start practice");
    WriteStatus("starting match=practice");
    return;
  }
  // Online Practice: no clock, health back up, nobody knocked out (a ring out still ends a round: five to win)
  if (plan.mode == autopilot::Mode::Sparring) plan.wins = 5;
  plan.online = true;
  g_match = Key(c, "match", "none");
  const uint32_t seed = uint32_t(std::strtoul(Key(c, "seed", "0").c_str(), nullptr, 10));

  net::Settings st;
  st.side = plan.side + 1;
  st.bind = uint16_t(std::atoi(Key(c, "local_port", "7001").c_str()));
  st.peer = Key(c, "remote");
  st.delay = std::atoi(Key(c, "delay", "2").c_str());
  st.sparring = plan.mode == autopilot::Mode::Sparring;
  st.feed = Key(c, "feed");  // a lobby match's: the spectators' feed
  if (st.peer.empty()) {
    WriteStatus("error bad command: no remote");
    return;
  }
  if (const char* log = std::getenv("SC5_NET_LOG"); log && *log) {
    st.log = log;  // "{match}" becomes the match id
    if (size_t at = st.log.find("{match}"); at != std::string::npos) st.log.replace(at, 7, g_match);
  }
  // tests: a simulated network on what this game sends
  if (const char* v = std::getenv("SC5_NET_LAG")) st.lag_ms = std::atoi(v);
  if (const char* v = std::getenv("SC5_NET_JITTER")) st.jitter_ms = std::atoi(v);
  if (const char* v = std::getenv("SC5_NET_LOSS")) st.loss_percent = std::atoi(v);
  game::SetSeed(int64_t(seed));
  versus::SetOnline(true, plan.side, plan.wins);
  sparring::Set(st.sparring);
  net::Configure(st);
  g_net = st;
  g_plan = plan;
  g_in_match = true;
  g_reported = false;
  autopilot::Begin(plan);
  REXLOG_INFO("sc5 link: start {} as player {}, match {}, seed {}, best of {}, the other at {}", mode, plan.side + 1,
              g_match, seed, rounds, st.peer);
  WriteStatus("starting match=" + g_match);
}

// A spectator (docs\PROTOCOL.md 3.6): the game goes to the match's select screen as a player's would, follows both
// players' picks from the feed (or the start line's), takes player 1's battle setup and fight from it, and plays
// every frame from both players' inputs (sc5/net/netplay.cpp)
void StartWatch(const Command& c) {
  const std::string mode = Key(c, "mode", "versus");
  const std::string feed = Key(c, "feed");
  if (feed.empty()) {
    WriteStatus("error bad command: watch needs feed");
    return;
  }
  if (mode != "versus" && mode != "sparring") {
    WriteStatus("error bad command: no mode " + mode + " to watch");
    return;
  }
  autopilot::Plan plan;
  plan.mode = mode == "sparring" ? autopilot::Mode::Sparring : autopilot::Mode::Versus;
  for (int s = 0; s < 2; s++) {
    const std::string v = Key(c, s ? "p2" : "p1", "-");
    if (!ParsePick(v, plan.slots[s], plan.costumes[s], plan.created[s])) {
      WriteStatus("error bad command: no character " + v);
      return;
    }
  }
  const int rounds = std::atoi(Key(c, "rounds", "3").c_str());
  plan.wins = plan.mode == autopilot::Mode::Sparring ? 5 : std::max(1, (rounds + 1) / 2);
  plan.online = true;
  plan.side = 2;  // neither player's: both sides follow the feed
  g_mode = mode;
  g_match = Key(c, "match", "none");
  const uint32_t seed = uint32_t(std::strtoul(Key(c, "seed", "0").c_str(), nullptr, 10));
  net::Settings st;
  st.watch = true;
  st.feed = feed;
  st.sparring = plan.mode == autopilot::Mode::Sparring;
  if (const char* log = std::getenv("SC5_NET_LOG"); log && *log) {
    st.log = log;
    if (size_t at = st.log.find("{match}"); at != std::string::npos) st.log.replace(at, 7, g_match);
  }
  game::SetSeed(int64_t(seed));
  versus::SetOnline(true, 2, plan.wins);
  sparring::Set(st.sparring);
  net::Configure(st);
  g_net = st;
  g_plan = plan;
  g_in_match = true;
  g_reported = false;
  autopilot::Begin(plan);
  REXLOG_INFO("sc5 link: watch {}, match {}, seed {}, the feed at {}", mode, g_match, seed, feed);
  WriteStatus("starting match=" + g_match);
}

// stats-<match>.json in the session folder: what an online match was like on this PC (net::MatchStatsJson), written
// every 10 s while it goes on and at its end, before its status line (a game closed in a match leaves the last)
void WriteStats(const char* end) {
  static const std::wstring dir = EnvW(L"SC5_SESSION_DIR");
  if (dir.empty() || g_match.empty() || g_match == "none" || g_match == "practice") return;
  std::wstring id;
  for (char ch : g_match) id += std::isalnum(uint8_t(ch)) || ch == '-' || ch == '_' ? wchar_t(ch) : L'_';
  const std::wstring path = dir + L"\\stats-" + id + L".json", tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  const std::string text = net::MatchStatsJson(g_match, end) + "\n";
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  for (int i = 0; i < 200; i++) {
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    Sleep(5);
  }
}

void Park() {
  g_start_waits = false;
  net::Disable();
  versus::SetOnline(false, 0, 2);
  sparring::Set(false);
  autopilot::Park();
  g_in_match = false;
  REXLOG_INFO("sc5 link: park");
}

void Rematch(const Command& c) {
  if (!g_in_match) {
    WriteStatus("error no match to repeat");
    return;
  }
  if (std::string m = Key(c, "match"); !m.empty()) g_match = m;
  if (std::string seed = Key(c, "seed"); !seed.empty())
    game::SetSeed(int64_t(std::strtoul(seed.c_str(), nullptr, 10)));
  // per side: a fighter named (a roster id or the game's id with its costume, "0.1"), `-` (that side's player picks
  // again on the select screen), or no key (the same fighter again); stage=keep: the last match's stage
  autopilot::Plan plan = g_plan;
  bool named[2] = {false, false};
  for (int s = 0; s < 2; s++) {
    auto it = c.keys.find(s ? "p2" : "p1");
    if (it == c.keys.end()) continue;
    if (!ParsePick(it->second, plan.slots[s], plan.costumes[s], plan.created[s])) {
      WriteStatus("error bad command: no character " + it->second);
      return;
    }
    named[s] = true;
  }
  const bool same_stage = Key(c, "stage") == "keep";
  if (c.keys.count("choose")) plan.choose = Key(c, "choose") == "1" && plan.mode == autopilot::Mode::Versus;
  g_plan = plan;
  g_reported = false;
  autopilot::Rematch(plan, named, same_stage);
  REXLOG_INFO("sc5 link: rematch, match {}, picks {} {}{}", g_match, Key(c, "p1", "(the same)"),
              Key(c, "p2", "(the same)"), same_stage ? ", the same stage" : "");
  WriteStatus("starting match=" + g_match);
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  autopilot::OnFrame(ctx, base);
  if (frame::Count() % 6 == 0) {
    std::string line;
    if (ClaimCommand(line)) {
      REXLOG_INFO("sc5 link: command {}", line);
      g_error_shown = false;
      g_last_status.clear();  // the state again after a command, even unchanged
      const Command c = Parse(line);
      if (c.verb == "start") Start(c);
      else if (c.verb == "park") Park();
      else if (c.verb == "rematch") Rematch(c);
      else if (c.verb == "quit") {
        WriteStatus("quit");
        ExitProcess(0);
      } else {
        WriteStatus("error bad command");
      }
    }
  }
  if (g_start_waits && autopilot::AtHoldPoint()) Start(g_waiting_start);
  if (g_error_shown) return;  // an error line stands until the next command
  // the match's loser chose the next match on the result screen (choose=1): said once the result was
  if (g_in_match && g_reported && g_plan.choose) {
    std::string choice;
    if (autopilot::ResultChosen(choice)) WriteStatus("chose choice=" + choice + " match=" + g_match);
    return;
  }
  if (!g_in_match) {
    if (autopilot::Lost()) WriteStatus("error the game went somewhere the launcher's autopilot does not know");
    else WriteStatus(autopilot::AtHoldPoint() ? "ready" : "booting");
    return;
  }
  // practice: in_match once Training's fight is up; the player leaving it for the main menu is the hold point again
  if (g_mode == "practice") {
    if (autopilot::Lost()) {
      WriteStatus("error the game went somewhere the launcher's autopilot does not know");
    } else if (autopilot::AtHoldPoint()) {
      g_in_match = false;
      WriteStatus("ready");
    } else {
      WriteStatus(autopilot::Practicing() ? "in_match match=practice" : "starting match=practice");
    }
    return;
  }
  if (g_reported) return;
  // the match's stats every 10 s while it runs in sync
  static uint64_t last_stats = 0;
  if (net::InSync() && frame::Count() >= last_stats + 600) {
    last_stats = frame::Count();
    WriteStats("running");
  }
  // Online Practice: a player's BACK (or the other game's leaving) ends it; the netplay stops at once, so that no
  // round later in the fight meets the other game again
  if (g_mode == "sparring" && net::SparringOver()) {
    WriteStats("sparring_over");
    WriteStatus("sparring_over match=" + g_match);
    net::Disable();
    g_reported = true;
    return;
  }
  int winner = 0, p1 = 0, p2 = 0;
  std::string c1, c2;
  if (autopilot::MatchOver(winner, p1, p2, c1, c2)) {
    WriteStats(g_mode == "sparring" ? "sparring_over" : "result");
    net::EndMatch();
    if (g_mode == "sparring")
      WriteStatus("sparring_over match=" + g_match);
    else
      WriteStatus(fmt::format("result winner={} p1_rounds={} p2_rounds={} p1_char={} p2_char={} match={}", winner, p1,
                              p2, c1, c2, g_match));
    g_reported = true;
    return;
  }
  const net::Status s = net::GetStatus();
  if (s == net::Status::Desynced) {
    WriteStats("desync");
    WriteStatus(fmt::format("desync frame={} match={}", net::Frame(), g_match));
    g_reported = true;
    return;
  }
  if (s == net::Status::Disconnected) {
    WriteStats("disconnected");
    WriteStatus("disconnected match=" + g_match);
    g_reported = true;
    return;
  }
  if (autopilot::Lost()) {
    WriteStatus(autopilot::LostWhy().empty() ? "error the game went somewhere the launcher's autopilot does not know"
                                             : "error " + autopilot::LostWhy());
    g_reported = true;
    return;
  }
  WriteStatus((s == net::Status::InSync ? "in_match match=" : "starting match=") + g_match);
}

}  // namespace

void Init() {
  const char* on = std::getenv("SC5_NET");
  if (!on || std::strcmp(on, "1") != 0) return;
  g_cmd = EnvW(L"SC5_CMD");
  g_status = EnvW(L"SC5_STATUS");
  if (g_cmd.empty() || g_status.empty()) {
    REXLOG_ERROR("sc5 link: SC5_NET=1 without SC5_CMD and SC5_STATUS");
    return;
  }
  g_on = true;
  WriteStatus("booting");
  net::Install();
  autopilot::Init(true);
  frame::SetMenuHook(&OnFrame);
  REXLOG_INFO("sc5 link: the launcher's game (instance {})", std::getenv("SC5_INSTANCE") ? std::getenv("SC5_INSTANCE") : "1");
}

bool On() { return g_on; }

}  // namespace sc5::link
