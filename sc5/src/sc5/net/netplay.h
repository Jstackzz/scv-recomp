#pragma once
// Netplay for two players over UDP (adapted from MK9 Online's mk9/net/netplay.h). Each game runs on its own until its
// fight has loaded and played its intro; both meet there (a barrier: the first frame of round 1's call), agree on what
// must be alike (the random numbers' seed, the counters that start at boot, the battle's state machine, every block
// of the fight that differs: player 2's game takes player 1's), and from then on every frame runs with both players'
// inputs on both PCs: the local player's own pad goes out to the other game, the other's comes in, and the game's pad
// reads (sc5/input.h) answer with the frame's inputs (user 0 player 1, user 1 player 2).
//
// Outside a round's fighting (the round's call, a KO, between rounds) both games run on neutral inputs. While the
// round is fought they run ahead on a guess (the other's last input) and go back when a guess was wrong: the battle put
// back (sc5/battle.h) to the top of that frame and the frames since run again, unseen and unheard, within the same
// frame. Local input waits `delay` frames before the game uses it.
// Every 30 frames of fighting both games compare a hash of the fight (battle::FightHash); a difference is a desync.
//
// Configuration, from the launcher link or, for tests, the environment:
//   SC5_NET_BIND=<udp port>         this game's port
//   SC5_NET_PEER=<ip>:<port>        the other game
//   SC5_NET_SIDE=1|2                this game's player (1: player 1, the pad of user 0)
//   SC5_NET_DELAY=<frames>          input delay (default 2); both play the larger of the two games'
//   SC5_NET_LOCKSTEP=1              no rollback: every frame waits for the other's input
//   SC5_NET_LOG=<path>              every frame's inputs and checks
//   SC5_NET_LAG=<ms>, SC5_NET_JITTER=<ms>, SC5_NET_LOSS=<percent>   a simulated network on what this game sends
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace sc5::net {

struct Settings {
  uint16_t bind = 0;
  int side = 1;  // 1 or 2
  std::string peer;
  int delay = 2;
  bool rollback = true;
  std::string log;
  int lag_ms = 0, jitter_ms = 0, loss_percent = 0;
  // Online Practice (sc5/sparring.h): BACK goes over the wire and ends the match in sync, and so does the other
  // game's leaving
  bool sparring = false;
  // the launcher's spectator feed (docs\PROTOCOL.md 3.6), "127.0.0.1:port": a player's game writes what spectators
  // need there; with `watch` this game is a spectator's: no peer of its own, both players come from the feed
  std::string feed;
  bool watch = false;
};

enum class Status { Off, Connecting, Barrier, InSync, Desynced, Disconnected };

// reads SC5_NET_* and configures when they are there (tests); at startup
void InitFromEnvironment();
// the frame hooks, for the launcher's matches (once, at startup)
void Install();
// start or stop; applied by the main thread at the top of the next frame
void Configure(const Settings& st);
void Disable();
Status GetStatus();
const char* StatusName(Status s);
bool InSync();
// the other game was heard for the coming match
bool PeerHeard();
// Online Practice: a player pressed BACK (or the other game left) and the match in sync is over
bool SparringOver();
// the match's fight is over (both games saw its end): the games go on on their own (the result screen, menus) until
// the next fight's barrier (a rematch); main thread, at the top of a frame
void EndMatch();

// Before the barrier, the two games pass each other what their players pick on the select screen (a side picked on
// its own player's PC, followed on the other's) and the battle's settings (player 1's game's, which player 2's
// takes: the stage as player 1's game drew it, the round time, the rounds to win). For the coming match; main thread.
struct Pick {
  int phase = -1;  // -1 nothing yet; 0 choosing (slot: where the cursor is); 1 the character picked; 2 its costume too
  int slot = -1, costume = -1;
};
void SetLocalPick(int side, const Pick& pick, uint32_t pickable);
Pick RemotePick(int side);
// the select screen slots the other game can load (0 before it said)
uint32_t RemotePickable();
struct Setup {
  uint32_t stage = 0, round_time = 0, wins = 0;
  std::vector<uint32_t> words;  // player 1's game's values of what both must have alike from the setup on (versus.cpp)
};
void SendSetup(const Setup& s);
// player 2's game: player 1's setup, waiting for it up to `ms` (the network kept going meanwhile)
bool WaitSetup(Setup& out, int ms);
// a match is set up or running (the battle's second player is then the second pad's: the VS menus would leave it to
// the CPU)
bool Active();
// the match's frame running now (0 at the barrier)
uint32_t Frame();

struct Stats {
  uint32_t frames = 0, waits = 0, rollbacks = 0, rerun = 0, max_rollback = 0, holds = 0;
  double rollback_ms = 0, longest_rollback_ms = 0;
  int ping_ms = 0;
};
Stats GetStats();
// What the present (or last) match was like on this PC, as one line of JSON (the launcher's stats-<match>.json): its
// length, rollbacks, waits, frames held back, the input delay; `end` says how it ended ("running" while it goes on)
std::string MatchStatsJson(const std::string& match, const char* end);

}  // namespace sc5::net
