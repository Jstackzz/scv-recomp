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
// A round was started again after a desync (netplay.cpp, RoundAgain): how many times this match, and whether the
// players are being told just now (the overlay's notice, for a few seconds after)
int RoundsAgain();
bool RoundAgainNotice();
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
  // -1 nothing yet; 0 choosing (slot: where the cursor is); 1 the character picked; 2 its costume too; 3 the whole pick
  // being taken back (costume: the request's number), and from the other game for that side, its yes (autopilot.cpp)
  int phase = -1;
  int slot = -1, costume = -1;
};
void SetLocalPick(int side, const Pick& pick, uint32_t pickable);
Pick RemotePick(int side);
// the select screen slots the other game can load (0 before it said)
uint32_t RemotePickable();
// this game's downloadable parts for created characters, as a fingerprint (creation::DlcFingerprint), sent with the
// picks; the other game's (0 before it said). A created character with downloadable parts is picked only when the two
// are the same.
void SetLocalDlc(uint32_t fingerprint);
uint32_t RemoteDlc();
// The stage select before the coming match: the player who picks the stage there (`chooser`, 0 or 1: player 1's
// game decides, player 2's takes its word) and where they are with it: `phase` -1 nothing yet, 0 choosing (`cursor`:
// the place, 0 Random, sc5/select.h), 1 picked (`stage`: the place picked, Random drawn from the match's seed)
struct StagePick {
  int chooser = -1, phase = -1, cursor = -1, stage = -1;
};
void SetLocalStage(const StagePick& s);
StagePick RemoteStage();
// The result screen's menu after a match whose loser chooses the next one (a lobby's: the start line's choose=1,
// sc5/autopilot.h): the chooser (0 or 1: the match's loser, player 1 after a draw), `phase` -1 nothing yet, 0 choosing
// (`cursor`: 0 Rematch, 1 Character Select), 1 chosen. The chooser's game says it; the other's follows. Main thread.
struct ResultChoice {
  int chooser = -1, phase = -1, cursor = -1;
};
void SetLocalChoice(const ResultChoice& c);
ResultChoice RemoteChoice();
// The way into the coming fight (the VS screen while it loads, the stage's fly-over, the fighters' intros) plays unless
// a player skips it with START on their own controller: this game's player did (sent to the other game and the
// spectators), and whether either player did (a spectator's game: from the feed). Main thread.
void SetLocalSkip();
bool SkipWanted();
// The next match from the result screen (the launcher's rematch, both games): how many frames after the match's end
// this game had it, and the other game's (-1 not yet). Both take the result screen's menu from the same frame counted
// from the match's end, past both, so that whatever went on on the result screen went on for as long in both (the
// rematch parted at its first check, a player's animation one frame on in one game, when the two pressed a frame
// apart as the command came). Main thread.
void SetRematchAt(uint32_t frames);
int64_t RemoteRematchAt();
// A created character picked for the coming match (sc5/creation.h): its record, 13,340 bytes. This game's player's
// (`side` this game's) is sent until the other game has every chunk; the other player's is taken as it comes: exactly
// the record's bytes, from chunks 0-11 (1,200 bytes each, the last 140), the same bytes when a chunk comes again.
// Anything else refuses it for this match. A spectator's game takes it from the feed. Empty clears.
void SetLocalCreated(int side, const std::vector<uint8_t>& record);
// the other player's record for `side`: 1 whole (in `out`), 0 not yet, -1 refused (`why`)
int RemoteCreated(int side, std::vector<uint8_t>& out, std::string& why);
// that record forgotten, as if none had come: the other player took their pick back (sc5/autopilot.cpp), and another
// record, or the same again, may come for the match
void ForgetRemoteCreated(int side);
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
