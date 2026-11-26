#pragma once
// The game's end of the online menus' contract with the launcher (docs\PROTOCOL.md 3.8): ui.json, which the launcher
// replaces whole whenever anything in it changes (the account, the search, the lobbies, the series ...), read here into
// a Ui; and ask.txt, the player's choices the other way, one line at a time from a queue of the game's own. Only with
// SC5_NET=1 and SC5_MENUS=1 (the launcher's one game for everything). Main thread, but for Snapshot.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sc5::uilink {

struct Account {
  std::string state = "signed_out";  // signed_out, connecting, signed_in, offline
  std::string name, rank, error;
  int elo = 0, wins = 0, losses = 0;
};
struct Server {
  int online = 0, in_matches = 0;
  std::string notice;
};
struct Queue {
  std::string state = "idle";  // idle, searching, found, accepted, connecting, starting
  std::string kind;            // "", ranked, casual
  int secs = 0, accept_left = 0, ping = 0;
  std::string opponent, opponent_rank, text;
};
struct Series {
  std::string kind;   // "", ranked, casual, lobby
  std::string opponent;
  int mine = 0, theirs = 0, to = 0;
  std::string state;  // "", choosing, waiting, over
  std::string text;
};
struct LobbyRow {
  std::string code, name, host, mode, region;
  int players = 0, max = 0;
  bool koth = false, locked = false;
};
struct Member {
  std::string name, role;  // player, spectator
  bool ready = false;
  int wins = 0;
};
struct Lobby {
  bool present = false;  // "lobby": null when the player is in none
  std::string code, name, host, mode, state, text;
  bool koth = false, ready = false;
  std::vector<Member> members;
};
struct BoardRow {
  int place = 0, elo = 0, wins = 0, losses = 0;
  std::string name, rank;
};
struct Message {
  int64_t id = 0;
  std::string kind, text;  // info, error
};

struct Ui {
  bool seen = false;    // a ui.json was read at all
  bool fresh = false;   // its `t` moved on within the last 5 s: the launcher answers
  int64_t t = 0;
  Account account;
  Server server;
  std::string side = "off";
  Queue queue;
  Series series;
  int64_t lobbies_at = 0;
  std::vector<LobbyRow> lobbies;
  Lobby lobby;
  int64_t board_at = 0;
  std::vector<BoardRow> board;
  Message message;
  std::string update_version, update_text;
};

// SC5_MENUS=1 with the launcher's link: the files are the session folder's (SC5_SESSION_DIR)
void Init();
bool On();

// main thread, every frame: ui.json read again (about ten times a second while `often`, once a second otherwise), the
// next ask written once the launcher took the last one
void Poll(bool often);
// the latest reading (any thread)
Ui Snapshot();

// one ask: `verb` and its keys (text values are percent-encoded here), queued and written in turn as
// `ask n=<count> <verb> k=v ...`
void Ask(const std::string& verb, const std::vector<std::pair<std::string, std::string>>& keys = {});
// the asks written so far (tests, the log)
int AsksSent();

// percent-encoding for a text value: UTF-8 bytes other than [A-Za-z0-9_.-] as %XX
std::string Encode(const std::string& text);

}  // namespace sc5::uilink
