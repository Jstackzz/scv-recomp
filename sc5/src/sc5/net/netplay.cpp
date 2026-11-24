// Netplay for two players over UDP, see netplay.h. Adapted from MK9 Online's mk9/net/netplay.cpp, with SC5's battle
// snapshots (sc5/battle.h) in place of MK9's copy-on-write memory.
//
// Packets (little endian): u32 magic "SC5N", u8 type, u8 slot (side - 1), u16 match (barrier number), then
//   1 HELLO  -                                  presence while connecting
//   2 READY  u32 seed, u8 n, n x u32, u8 delay  "my fight is loaded": the barrier ends when the other's READY of this
//                                               match came; the match plays the larger input delay of the two
//   3 INPUT  u32 ack, u32 now, u32 seen, u32 first, u8 n, n x 8 bytes
//            the sender's own inputs for frames first..first+n-1; ack: how many of the receiver's inputs the sender
//            holds without a gap; now: the frame the sender runs; seen: the frame the sender last heard the receiver
//            run (for keeping the two sides even)
//   4 CHECK  u32 frame, u32 hash low, u32 hash high   the sender's hash of the fight at the top of that frame
//   5 BYE    u32 frame                          the sender left the match
//   6 PICK   u8 side, i8 phase, i8 slot, i8 costume, u32 pickable, u32 dlc
//            the select screen before the coming match (its number in the header): where the sender's player is with
//            their pick (Pick), the slots the sender's game can load and its downloadable parts for created
//            characters (a fingerprint: the same on two PCs with the same parts); repeated while connecting
//   7 SETUP  u32 stage, u32 round time, u32 wins, u8 n, n x u32
//            player 1's battle settings for the coming match and its values of what both games must have alike from
//            the battle setup on (sc5/versus.cpp); repeated
//   8 SUMS   u16 chunk, u16 chunks, u8 n, n x (u32 source, u32 size, u64 hash)
//            at the barrier, player 2's game's fight blocks (MeetSync below), repeated until FIX comes
//   9 FIX    u16 chunk, u16 chunks, u32 source, u16 offset, u16 len, len bytes
//            player 1's bytes of a fight block player 2's differs in (chunks 0: none differs), repeated until FIXOK
//  10 FIXOK  -                                  player 2's game took them all
//  11 STAGE  u8 chooser, i8 phase, i8 cursor, i8 stage
//            the stage select before the coming match (StagePick): who picks the stage there (player 1's game
//            decides) and where they are with it; repeated while connecting
//  12 CREATED u8 side, u8 chunk, u8 chunks, u16 len, len bytes
//            the sender's player's created character for the coming match (a PICK's costume -2): its record in 12
//            chunks (net/created_transfer.h), each sent every 100 ms until CREATED_ACK says the other game has it
//  13 CREATED_ACK u8 side, u16 mask   the chunks of that side's record this game has
//  14 CHOICE u8 chooser, i8 phase, i8 cursor
//            the result screen's menu after a match whose loser chooses the next one (ResultChoice): from the chooser's
//            game, where its cursor is (0 Rematch, 1 Character Select) and, phase 1, what they chose; repeated
//  15 SKIP   -   the sender's player skips the way into the coming fight (the VS screen, the stage's fly-over and the
//            fighters' intros: START on their own controller); repeated while connecting
//  16 RSKIP  u32 frame, u8 kind   a KO's replay cut short at that match frame (START or A on a player's own
//            controller): 0 asked (the frame some 30 ahead), 1 agreed (the frame is still to come here: both games
//            press A on both pads there), 2 refused; repeated until answered
//  17 GOAT   u32 frames   the next match from the result screen: this game had the launcher's `rematch` that many frames
//            after the match's end; both games take the result screen's menu from the same frame counted from there
//            (RematchAt); repeated while connecting

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "sc5/net/netplay.h"
#include "sc5/net/created_transfer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <random>
#include <thread>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/battle.h"
#include "sc5/controls.h"
#include "sc5/frame.h"
#include "sc5/game.h"
#include "sc5/input.h"
#include "sc5/rng.h"
#include "sc5/sound.h"
#include "sc5/sparring.h"
#include "sc5/testinput.h"

#pragma comment(lib, "ws2_32.lib")

namespace sc5::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E354353;  // "SC5N"
enum : uint8_t { kHello = 1, kReady = 2, kInput = 3, kCheck = 4, kBye = 5, kPick = 6, kSetup = 7, kSums = 8, kFix = 9,
                 kFixOk = 10, kStage = 11, kCreated = 12, kCreatedAck = 13, kChoice = 14, kSkip = 15,
                 kReplaySkip = 16, kRematchAt = 17 };
constexpr int kWindow = 24;                // inputs repeated in every INPUT packet
constexpr int kMaxWords = 32;
constexpr auto kPeerTimeout = std::chrono::seconds(10);
constexpr auto kBarrierTimeout = std::chrono::seconds(90);
constexpr uint32_t kMaxPredict = 8;        // frames run ahead of the other's last input, at most
constexpr int kRing = 16;                  // battle snapshots kept (> kMaxPredict)
constexpr uint32_t kCheckEvery = 30;

// what the two games make alike at the barrier: the counters that start at boot (docs\GAME.md, "Determinism"), each
// at the larger of the two games' values (CountsUp); not the three that count frames presented to the GPU
// (0x8280DE94, 0x8281929C, 0x8281991C: battle.cpp's kRaw says why), which each game's own GPU waits by
constexpr uint32_t kAgreedWords[] = {
    0x82771B40, 0x82771B44, 0x82771B48, 0x82771B4C, 0x82771B50, 0x82771B54, 0x82771B58, 0x82771B5C,
    0x828E8790, 0x835B3EB4,
};
// and the battle's state machine (0x82767D08, 0x200 bytes: the current and pending states and the embedded states'
// own counts, static addresses only): a state keeps counts from before it (the "between rounds" a rematch's barrier
// is the first frame of counted on from the result screens, which took a different time on each PC, and the two
// games left it on different frames)
constexpr uint32_t kAgreedMachine = 0x82767D08, kAgreedMachineSize = 0x200;
// ... all but +0x14, a heap object's address (the menus' object the fight was set up from), which each game has where
// its own heap put it: player 1's there left player 2's game pointing into its own heap at whatever lay at that
// address, and with the two heaps apart (two PCs: a game whose heap started 2 MB higher showed it on one) its KO
// replays no longer played, so every KO parted the games (2026-11-17)
// (and +0x13C, another heap object's address, which a fight sets: Elysium against Edge Master, Kilik against Ezio;
// with the count at +0x138 that comes with it, worked out from a player's pose: battle.cpp KindOf)
// (and +0x10, which goes with +0x14: that object counts as the fight's setup only while its +48 is the state machine's
// +0x10 (sub_82479360 and the KO replay's start test it). In a rematch the two games had numbered it apart (0x569 and
// 0x54B), player 2's game took player 1's, no longer saw its own setup object and played no KO replay: it went to the
// result 397 frames before the other (Ezio against Viola with 40 ms each way and render scale 2 on game 2,
// 2026-11-18). The snapshot does not carry it.)
constexpr uint32_t kMachineOwnWords[] = {0x82767D18, 0x82767D1C, 0x82767E40, 0x82767E44};

// The counters among the agreed words only count up, and none may go back at the barrier: each game takes the larger
// of the two games' values (READY carries each game's) and never less than its own now; a spectator takes the larger
// of the two. With player 1's values player 2's went back whenever player 2's game reached the barrier first (its
// loading or intro the faster), and the post-effects' blends (frame.cpp) then took an effect started before the
// barrier with a fade-in of 0 to elapsed below 0: -inf, the colour matrix of the final picture NaN, player 2's scene
// black under its HUD until the next round's call (0.1.2's testers; the exposure session found it, 2026-11-19: 7 of 7
// matches set back by 2 frames or more went black, none of 15 others). The main thread's counters stand still while a
// game waits here, so both games come out alike; two, 0x82771B40 and 0x828E8790, count on during the wait (other
// threads keep them: 137 frames in a game that waited 2.3 s), and they part the games whatever the barrier does, as
// they did before. 0x82771B5C holds packed bytes another thread changes: player 1's, as the state machine's words.
bool CountsUp(uint32_t a) { return (a >= 0x82771B40 && a <= 0x82771B58) || a == 0x828E8790 || a == 0x835B3EB4; }

std::vector<uint32_t> AgreedAddresses() {
  std::vector<uint32_t> out(std::begin(kAgreedWords), std::end(kAgreedWords));
  for (uint32_t a = kAgreedMachine; a < kAgreedMachine + kAgreedMachineSize; a += 4)
    if (std::find(std::begin(kMachineOwnWords), std::end(kMachineOwnWords), a) == std::end(kMachineOwnWords))
      out.push_back(a);
  return out;
}
// the game's own input delay per player (sub_8216E2F8 reads it at 0x8216E63C: 2 frames offline); the netplay's
// delay stands in for it
constexpr uint32_t kGameDelay = 0x8275C15C;
// The game's count of frames shown (0x82771B44, one of the agreed words): only the present moves it (sub_824945F8),
// and only when the GPU took the frame, yet the battle reads it, to throttle what it does to once in a few frames shown
// (sub_82155B80, sub_82114200, sub_821CA8F0: "4 or more since the stamp", the stamp kept in its objects). A frame run
// again presents nothing, and a busy GPU makes the present skip a count, so each game's count went its own way and
// the stamps came out apart (frame numbers 32 apart in the players' objects, 2026-11-17). In a match it is the
// match's frame from its agreed value at the barrier, in every frame run, again or not (SetFrameInputs).
constexpr uint32_t kFramesShown = 0x82771B44;
uint32_t g_shown_base = 0;

struct Wire {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int8_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Wire& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(Wire) == 8);

Wire Pack(const input::Pad& p) {
  Wire w;
  w.buttons = p.buttons;
  w.lt = p.left_trigger;
  w.rt = p.right_trigger;
  w.lx = int8_t(p.lx >> 8);
  w.ly = int8_t(p.ly >> 8);
  w.rx = int8_t(p.rx >> 8);
  w.ry = int8_t(p.ry >> 8);
  return w;
}

input::Pad Unpack(const Wire& w) {
  input::Pad p;
  p.connected = true;
  p.buttons = w.buttons;
  p.left_trigger = w.lt;
  p.right_trigger = w.rt;
  p.lx = int16_t(w.lx * 256);
  p.ly = int16_t(w.ly * 256);
  p.rx = int16_t(w.rx * 256);
  p.ry = int16_t(w.ry * 256);
  return p;
}

struct Peer {
  sockaddr_in addr{};
  bool seen = false;
  Clock::time_point last_heard{};
  std::map<uint32_t, Wire> inputs;  // their own inputs in the match
  uint32_t contiguous = 0;          // their inputs held without a gap: [0, contiguous)
  uint32_t ack = 0;                 // how many of ours they hold without a gap
  uint32_t now = 0;                 // the frame they said they run
  uint32_t heard = 0;               // the frame of ours they last heard of
  uint16_t ready_match = 0;
  uint32_t seed = 0;
  std::vector<uint32_t> words;
  int delay = -1;
  std::map<uint32_t, uint64_t> checks;
  bool left = false;
  std::map<uint32_t, Wire> used;  // what each frame ran with for them (theirs or a guess)
  uint32_t confirmed = 0;         // frames below this ran with their real inputs
  // before the coming match: their picks, the slots their game can load, player 1's setup
  Pick picks[2];
  uint32_t pickable = 0;
  uint32_t dlc = 0;  // their game's downloadable parts for created characters (a fingerprint; 0 before they said)
  bool has_setup = false;
  Setup setup;
  StagePick stage;            // their stage select (player 1's game's: who picks the stage)
  ResultChoice choice;        // their result screen's menu (the chooser's game's)
  bool skip_in = false;       // their player skips the way into the coming fight
  int64_t rematch_at = -1;    // their game had the launcher's rematch that many frames after the match's end
  bool heard_coming = false;  // heard since the coming match was set up
};

struct Outgoing {
  Clock::time_point when;
  std::vector<uint8_t> data;
};

std::mutex g_cfg_lock;
Settings g_pending;
bool g_pending_set = false, g_pending_off = false;
std::atomic<bool> g_active{false};

Settings g_cfg;
std::atomic<Status> g_status{Status::Off};
SOCKET g_sock = INVALID_SOCKET;
Peer g_peer;
int g_slot = 0;  // ours: side - 1
uint16_t g_match = 0;
uint32_t g_frame = 0;
std::map<uint32_t, Wire> g_mine;
Wire g_now[2];
std::map<uint32_t, uint64_t> g_my_checks, g_tentative;
int g_match_delay = -1;
// Online Practice: a BACK was seen (this player's or in the other's inputs, or the other left); the match is over
bool g_back_seen = false;
std::atomic<bool> g_sparring_over{false};
Stats g_stats;
Clock::time_point g_match_start{};  // the present match's first frame in sync
// where a rollback's time goes: putting the battle back, keeping the frames run again, running them
struct Cost {
  double restore_ms = 0, keep_ms = 0, run_ms = 0, top_keep_ms = 0;
  uint32_t keeps = 0, runs = 0, rollbacks = 0, top_keeps = 0, top_skipped = 0;
};
Cost g_cost;
// tests: SC5_NET_KEEP_ALL=1 keeps the battle at every frame top while rolling and at every frame run again, as before
// the frames that can never be gone back to were left out
bool KeepAll() {
  static const bool on = [] { const char* v = std::getenv("SC5_NET_KEEP_ALL"); return v && v[0] == '1'; }();
  return on;
}

// The spectator feed (docs\PROTOCOL.md 3.6): a TCP connection to the launcher on this PC, each packet after its u16
// length (little endian). Header: "SC5R", u8 type, u8 side (the netplay's slot), u16 match (the barrier's number); then
//   5 PICK   u8 side, i8 phase, i8 slot, i8 costume, u32 pickable   the player's pick on the select screen
//   6 MEET   u8 kind, then: 0 READY u32 seed, u8 n, n x u32 words, u8 delay (each game at the barrier);
//            1 SETUP u32 stage, u32 round time, u32 wins, u8 n, n x u32 (player 1's battle setup, sc5/versus.cpp);
//            2 BLOCK u16 chunk, u16 chunks, u32 source, u16 offset, u16 len, len bytes (player 1's fight at the
//            barrier, every block: a spectator takes them all as player 2's game takes the ones that differ)
//   7 INPUTS u32 first frame, u8 n, n x (u32 buttons | lt << 16 | rt << 24, u32 sticks lx ly rx ry)   the player's own
//   8 CHECK  u32 frame, u32 hash low, u32 hash high
//   9 END    u32 frame   the frame the game left the match at
//  10 STAGE  u8 chooser, i8 phase, i8 cursor, i8 stage   the stage select (StagePick), from the game whose player picks
//  11 CREATED u8 side, u8 chunk, u8 chunks, u16 len, len bytes   the player's created character (as netplay's 12)
//  12 SKIP   u8 kind, then kind 0: -   the player skips the way into the coming fight (the barrier's number: that
//            match's); kind 1: u32 frame   a KO's replay cut short at that frame of the match (agreed: RSKIP)
// A player's game writes its own; a spectator's (watch) reads both players' and plays every frame from them.
constexpr uint32_t kFeedMagic = 0x52354353;  // "SC5R"
enum : uint8_t { kFeedPick = 5, kFeedMeet = 6, kFeedInputs = 7, kFeedCheck = 8, kFeedEnd = 9, kFeedStage = 10,
                 kFeedCreated = 11, kFeedSkip = 12 };

// A created character's record on its way (CREATED chunks, the feed's type 11): 12 chunks of 1,200 bytes, the last 140.
// Exactly those: a chunk count, number or length otherwise, or a chunk again with other bytes, refuses it.
enum : uint8_t { kMeetReady = 0, kMeetSetup = 1, kMeetBlock = 2 };
constexpr size_t kFeedOutMax = 8u << 20;
SOCKET g_feed = INVALID_SOCKET;
bool g_feed_connected = false;
Clock::time_point g_feed_retry{};
sockaddr_in g_feed_addr{};
std::string g_feed_out, g_feed_in;
Pick g_fed_pick;  // this game's pick as last written to the feed

// a spectator: what the feed brought of each player, by match (the barrier's number)
struct Watched {
  std::map<uint32_t, Wire> inputs[2];
  uint32_t contiguous[2] = {0, 0};
  std::map<uint32_t, uint64_t> checks[2];
  bool ready[2] = {false, false};
  uint32_t seed = 0;            // player 1's
  std::vector<uint32_t> words;  // player 1's
  std::vector<uint32_t> words2;  // player 2's (its counters: CountsUp)
  int delay[2] = {-1, -1};
  bool has_setup = false;
  Setup setup;
  std::map<uint16_t, std::vector<uint8_t>> blocks;  // player 1's fight, by chunk
  int block_chunks = -1;
  bool ended[2] = {false, false};
  uint32_t end_frame[2] = {0, 0};
  Pick picks[2];
  StagePick stage;  // the stage select, from the game whose player picks the stage
  Incoming created[2];  // each player's created character, from that player's game
  bool skip_in = false;               // a player skipped the way into this fight
  std::set<uint32_t> replay_skips;    // the frames a KO's replay was cut short at (agreed by the players' games)
};
std::map<uint16_t, Watched> g_watched;
uint32_t g_watch_stream = 0;
std::map<uint32_t, uint64_t> g_watch_mine;  // the spectator's own checks, until the players' come
bool g_watch_rolling = false;               // the players rolled the frame before (WatchNeutral)
FILE* g_log = nullptr;
Clock::time_point g_last_hello{};
std::deque<Outgoing> g_outbox;
std::mutex g_outbox_lock;
std::mt19937 g_rng{12345};

// rollback
bool g_rolling = false;
std::array<uint32_t, kRing> g_ring{};
std::array<int64_t, kRing> g_ring_frame{};
std::array<uint64_t, kRing> g_ring_count{};  // frame::Count() at that frame's top (the key states are kept by)

// before the coming match: this game's picks and settings for the other (PICK, SETUP)
Pick g_local_picks[2];
uint32_t g_local_pickable = 0;
uint32_t g_local_dlc = 0;
bool g_picks_changed = false;
StagePick g_local_stage, g_fed_stage;  // this game's stage select for the coming match, and as last fed
ResultChoice g_local_choice;             // this game's result screen's menu, when its player chooses the next match
bool g_local_skip_in = false, g_fed_skip_in = false;  // this game's player skips the way into the coming fight
int64_t g_local_rematch_at = -1;  // this game had the launcher's rematch that many frames after the match's end
// A KO's replay cut short (RSKIP): the frame both games agreed on (0 none), the one this game asked for and has no
// answer to yet (0 none), whether that frame found the replay going (both games press A there), and when the ask went
struct ReplaySkip {
  uint32_t at = 0, asked = 0;
  uint32_t press_at = 0;  // the replay was going at the top of `at`: A on both pads from there for kReplaySkipPress
  Clock::time_point sent{};
  uint32_t replay_frames = 0;  // this game's frames of the present replay (tests: SC5_TEST_SKIP_REPLAY)
};
constexpr uint32_t kReplaySkipAhead = 30, kReplaySkipPress = 4;
ReplaySkip g_rskip;
bool g_rskip_held = true;  // the player's START or A down the frame before (held into the replay: no press)
// created characters for the coming match: this game's player's record (sent until acknowledged, fed once) and the
// other player's as it comes
std::vector<uint8_t> g_local_created[2];
uint16_t g_created_acked[2] = {0, 0};
bool g_created_fed[2] = {false, false};
Incoming g_incoming[2];
Clock::time_point g_last_pick_send{}, g_last_setup_send{}, g_opened_at{};
Setup g_my_setup;
bool g_my_setup_set = false;

// keeping the sides even
struct Pacing {
  std::deque<int> samples;
  uint32_t streak = 0, last_hold = 0;
  bool had_hold = false;
};
Pacing g_pace;

// The fight's state made alike at the barrier. Each game keeps state from before a match that its loading does not
// set again (from an earlier match and its result screens, which ran a different time on each PC: a rematch or the
// next match started apart, and in a few frames the two fights went different ways). So at the barrier both games save
// the battle (battle::Save) and hash each block of the fight in it (Kind::Fight bytes: not the poses, heap objects, the
// stack, nor the raw pointers); player 2's game sends its hashes (SUMS), player 1's game sends its bytes of every block
// whose hash differs (FIX), and player 2's game writes them where they came from before the first frame (FIXOK). A
// first match from a fresh start has nothing to send.
struct SyncBlock {
  uint32_t source, size;
  uint64_t hash;
  std::vector<uint8_t> data;  // player 1: the block's bytes
};
struct MeetSync {
  uint16_t match = 0;
  std::vector<SyncBlock> mine;
  // player 1: player 2's sums by chunk; the FIX chunks to send; FIXOK came
  std::map<uint16_t, std::vector<SyncBlock>> their_sums;
  int their_chunks = -1;
  std::vector<std::vector<uint8_t>> fix;  // whole packets
  bool fix_built = false, fix_ok = false;
  // player 2: the FIX chunks that came, their count; written
  std::map<uint16_t, std::vector<uint8_t>> got;
  int got_chunks = -1;
  bool applied = false;
  uint32_t bytes = 0, blocks = 0;
};
MeetSync g_sync;
uint32_t g_sync_stream = 0;

int OwnDelay() { return std::clamp(g_cfg.delay, 0, 8); }
int Delay() { return g_match_delay >= 0 ? g_match_delay : OwnDelay(); }

rex::memory::Memory* Mem() { return REX_KERNEL_STATE()->memory(); }
uint32_t Get32(uint32_t a) { return __builtin_bswap32(*Mem()->TranslateVirtual<const uint32_t*>(a)); }
void Put32(uint32_t a, uint32_t v) { *Mem()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v); }

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

bool ParseAddress(const std::string& s, sockaddr_in& out) {
  const auto colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  std::memset(&out, 0, sizeof out);
  out.sin_family = AF_INET;
  out.sin_port = htons(uint16_t(std::atoi(s.c_str() + colon + 1)));
  return inet_pton(AF_INET, s.substr(0, colon).c_str(), &out.sin_addr) == 1;
}

struct Writer {
  uint8_t b[1400];
  int n = 0;
  void U8(uint8_t v) { b[n++] = v; }
  void U16(uint16_t v) {
    std::memcpy(b + n, &v, 2);
    n += 2;
  }
  void U32(uint32_t v) {
    std::memcpy(b + n, &v, 4);
    n += 4;
  }
  void Bytes(const void* p, int len) {
    std::memcpy(b + n, p, size_t(len));
    n += len;
  }
};

Writer Header(uint8_t type) {
  Writer w;
  w.U32(kMagic);
  w.U8(type);
  w.U8(uint8_t(g_slot));
  w.U16(g_match);
  return w;
}

// a packet about the coming match (the next barrier's number)
Writer HeaderComing(uint8_t type) {
  Writer w;
  w.U32(kMagic);
  w.U8(type);
  w.U8(uint8_t(g_slot));
  w.U16(uint16_t(g_match + 1));
  return w;
}

void SendNow(const uint8_t* b, int n) {
  sendto(g_sock, reinterpret_cast<const char*>(b), n, 0, reinterpret_cast<const sockaddr*>(&g_peer.addr),
         sizeof g_peer.addr);
}

void Pump() {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_outbox_lock);
  while (!g_outbox.empty() && g_outbox.front().when <= now) {
    SendNow(g_outbox.front().data.data(), int(g_outbox.front().data.size()));
    g_outbox.pop_front();
  }
}

// the simulated network delivers a packet when it is due (a thread pumps every millisecond)
void StartPumpThread() {
  static const bool started = [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Pump();
      }
    }).detach();
    return true;
  }();
  (void)started;
}

void Send(const Writer& w) {
  if (g_sock == INVALID_SOCKET) return;
  if (g_cfg.loss_percent && int(g_rng() % 100) < g_cfg.loss_percent) return;
  if (!g_cfg.lag_ms && !g_cfg.jitter_ms) return SendNow(w.b, w.n);
  const int extra = g_cfg.jitter_ms ? int(g_rng() % uint32_t(g_cfg.jitter_ms + 1)) : 0;
  StartPumpThread();
  std::lock_guard<std::mutex> lock(g_outbox_lock);
  Outgoing o{Clock::now() + std::chrono::milliseconds(g_cfg.lag_ms + extra), std::vector<uint8_t>(w.b, w.b + w.n)};
  // a packet is never delivered before one sent earlier (one route)
  if (!g_outbox.empty() && o.when < g_outbox.back().when) o.when = g_outbox.back().when;
  g_outbox.push_back(std::move(o));
}

// --- the spectator feed ---

Writer FeedHeader(uint8_t type, uint16_t match) {
  Writer w;
  w.U32(kFeedMagic);
  w.U8(type);
  w.U8(uint8_t(g_slot));
  w.U16(match);
  return w;
}

// a player's game: a packet into the feed (kept while the launcher's port is not connected)
void Feed(const Writer& w) {
  if (g_cfg.feed.empty() || g_cfg.watch || g_feed_out.size() > kFeedOutMax) return;
  const uint16_t len = uint16_t(w.n);
  g_feed_out.append(reinterpret_cast<const char*>(&len), 2);
  g_feed_out.append(reinterpret_cast<const char*>(w.b), size_t(w.n));
}

void FeedClose() {
  if (g_feed != INVALID_SOCKET) closesocket(g_feed);
  g_feed = INVALID_SOCKET;
  g_feed_connected = false;
  g_feed_in.clear();
}

void HandleFeed(const uint8_t* b, int n);

// connect (again every half second), send what waits, and for a spectator take what came
void FeedPump() {
  if (g_cfg.feed.empty()) return;
  if (g_feed == INVALID_SOCKET) {
    if (Clock::now() < g_feed_retry) return;
    g_feed_retry = Clock::now() + std::chrono::milliseconds(500);
    g_feed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_feed == INVALID_SOCKET) return;
    u_long nb = 1;
    ioctlsocket(g_feed, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(g_feed, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    connect(g_feed, reinterpret_cast<const sockaddr*>(&g_feed_addr), sizeof g_feed_addr);
  }
  if (!g_feed_connected) {
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(g_feed, &w);
    FD_SET(g_feed, &e);
    timeval tv{0, 0};
    if (select(0, nullptr, &w, &e, &tv) <= 0) return;
    if (FD_ISSET(g_feed, &e) || !FD_ISSET(g_feed, &w)) {
      FeedClose();
      return;
    }
    g_feed_connected = true;
    REXLOG_INFO("sc5 net: the spectator feed is connected ({})", g_cfg.watch ? "watching" : "writing");
  }
  while (!g_feed_out.empty()) {
    const int sent = send(g_feed, g_feed_out.data(), int(std::min<size_t>(g_feed_out.size(), 1 << 16)), 0);
    if (sent > 0) {
      g_feed_out.erase(0, size_t(sent));
      continue;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) FeedClose();
    break;
  }
  if (!g_cfg.watch || g_feed == INVALID_SOCKET) return;
  char buf[1 << 16];
  for (;;) {
    const int got = recv(g_feed, buf, sizeof buf, 0);
    if (got > 0) {
      g_feed_in.append(buf, size_t(got));
      continue;
    }
    if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK) FeedClose();
    break;
  }
  size_t at = 0;
  while (g_feed_in.size() - at >= 2) {
    uint16_t len;
    std::memcpy(&len, g_feed_in.data() + at, 2);
    if (g_feed_in.size() - at - 2 < len) break;
    HandleFeed(reinterpret_cast<const uint8_t*>(g_feed_in.data()) + at + 2, len);
    at += 2 + size_t(len);
  }
  g_feed_in.erase(0, at);
}

// a spectator: one packet of a player's (the launcher sends everything again when the game connects again: taking a
// packet twice changes nothing)
void HandleFeed(const uint8_t* b, int n) {
  if (n < 8) return;
  uint32_t magic;
  std::memcpy(&magic, b, 4);
  if (magic != kFeedMagic || b[5] > 1) return;
  const int side = b[5];
  uint16_t match;
  std::memcpy(&match, b + 6, 2);
  auto u16 = [&](int o) {
    uint16_t v;
    std::memcpy(&v, b + o, 2);
    return v;
  };
  auto u32 = [&](int o) {
    uint32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
  };
  // A lobby's match that went on from the last one's result screens is its two games' second meeting (or a later
  // one), and its packets carry that number: a spectator's game, whose count starts at 0 for each match it watches,
  // counts from the latest one its feed brings before it meets the players
  if (g_cfg.watch && g_status == Status::Connecting && match > uint16_t(g_match + 1) && match < 0x8000) {
    REXLOG_INFO("sc5 net: the feed's match is the players' meeting {}", match);
    g_match = uint16_t(match - 1);
  }
  Watched& w = g_watched[match];
  switch (b[4]) {
    case kFeedPick:
      if (n >= 16 && b[8] <= 1) {
        Pick& p = w.picks[b[8]];
        p.phase = int8_t(b[9]);
        p.slot = int8_t(b[10]);
        p.costume = int8_t(b[11]);
      }
      break;
    case kFeedCreated:
      if (n >= 13 && b[8] <= 1 && b[8] == side) {
        const int len = b[11] | b[12] << 8;
        if (13 + len <= n) w.created[b[8]].Take(b[9], b[10], b + 13, len);
      }
      break;
    case kFeedSkip:
      if (n >= 9 && b[8] == 0) w.skip_in = true;
      if (n >= 13 && b[8] == 1) w.replay_skips.insert(u32(9));
      break;
    case kFeedStage:
      if (n >= 12 && b[8] <= 1) {
        w.stage.chooser = b[8];
        w.stage.phase = int8_t(b[9]);
        w.stage.cursor = int8_t(b[10]);
        w.stage.stage = int8_t(b[11]);
      }
      break;
    case kFeedMeet:
      if (n < 9) break;
      if (b[8] == kMeetReady && n >= 14) {
        w.ready[side] = true;
        const int count = b[13];
        std::vector<uint32_t> words;
        for (int i = 0; i < count && 14 + i * 4 + 4 <= n; i++) words.push_back(u32(14 + i * 4));
        const int after = 14 + count * 4;
        w.delay[side] = after < n ? b[after] : 2;
        if (side == 0) {
          w.seed = u32(9);
          w.words = std::move(words);
        } else {
          w.words2 = std::move(words);
        }
      } else if (b[8] == kMeetSetup && side == 0 && n >= 22) {
        w.has_setup = true;
        w.setup.stage = u32(9);
        w.setup.round_time = u32(13);
        w.setup.wins = u32(17);
        w.setup.words.clear();
        for (int i = 0; i < b[21] && 22 + i * 4 + 4 <= n; i++) w.setup.words.push_back(u32(22 + i * 4));
      } else if (b[8] == kMeetBlock && side == 0 && n >= 13) {
        w.block_chunks = u16(11);
        if (n > 13) w.blocks[u16(9)] = std::vector<uint8_t>(b + 13, b + n);
      }
      break;
    case kFeedInputs:
      if (n >= 13) {
        const uint32_t first = u32(8);
        for (int i = 0; i < b[12] && 13 + i * 8 + 8 <= n; i++) {
          const uint32_t buttons = u32(13 + i * 8), sticks = u32(17 + i * 8);
          Wire in;
          in.buttons = uint16_t(buttons);
          in.lt = uint8_t(buttons >> 16);
          in.rt = uint8_t(buttons >> 24);
          in.lx = int8_t(sticks);
          in.ly = int8_t(sticks >> 8);
          in.rx = int8_t(sticks >> 16);
          in.ry = int8_t(sticks >> 24);
          w.inputs[side].emplace(first + uint32_t(i), in);
        }
        while (w.inputs[side].count(w.contiguous[side])) w.contiguous[side]++;
      }
      break;
    case kFeedCheck:
      if (n >= 20) w.checks[side][u32(8)] = uint64_t(u32(12)) | uint64_t(u32(16)) << 32;
      break;
    case kFeedEnd:
      if (n >= 12) {
        w.ended[side] = true;
        w.end_frame[side] = u32(8);
      }
      break;
  }
}

// a player's game: the frame it left the match at
void FeedEnd() {
  if (g_cfg.feed.empty() || g_cfg.watch || !g_match) return;
  Writer w = FeedHeader(kFeedEnd, g_match);
  w.U32(g_frame);
  Feed(w);
  FeedPump();
}

void SendInputs() {
  if (g_mine.empty()) return;
  const uint32_t last = g_mine.rbegin()->first;
  uint32_t first = std::max(g_peer.ack, last + 1 >= uint32_t(kWindow) ? last + 1 - kWindow : 0u);
  first = std::max(first, g_mine.begin()->first);
  if (first > last) first = last;
  Writer w = Header(kInput);
  w.U32(g_peer.contiguous);
  w.U32(g_frame);
  w.U32(g_peer.now);
  w.U32(first);
  w.U8(uint8_t(last - first + 1));
  for (uint32_t f = first; f <= last; f++) {
    auto it = g_mine.find(f);
    const Wire in = it != g_mine.end() ? it->second : Wire{};
    w.Bytes(&in, sizeof in);
  }
  Send(w);
}

// A KO's replay cut short (RSKIP): this game's answer, or the frame agreed. Both games press A on both pads at the
// agreed frame for a few frames if the replay is still going there (decided at its top, the same in both: the replay
// starts and ends on the same frame in the two games), which the replay's manager (sub_8211A8A8) takes as its player's
// skip (START or A newly pressed on that player's pad). Asked frames are some 30 ahead, past where a rollback reaches.
void SendReplaySkip(uint32_t frame, uint8_t kind) {
  Writer w = Header(kReplaySkip);
  w.U32(frame);
  w.U8(kind);
  Send(w);
}

void ReplaySkipAgreed(uint32_t frame) {
  if (g_rskip.at == frame) return;
  g_rskip.at = frame;
  g_rskip.asked = 0;
  REXLOG_INFO("sc5 net: the KO's replay is cut short at frame {} (now {})", frame, g_frame);
  Writer f = FeedHeader(kFeedSkip, g_match);
  f.U8(1);
  f.U32(frame);
  Feed(f);
}

void OnReplaySkip(uint32_t frame, uint8_t kind) {
  if (kind == 0) {  // the other game asks: yes while the frame is still to come here and nothing earlier was agreed
    const bool yes = (g_rskip.at == frame) ||
                     (!g_rskip.at && (!g_rskip.asked || frame <= g_rskip.asked) && g_frame < frame);
    if (yes) ReplaySkipAgreed(frame);
    SendReplaySkip(frame, yes ? 1 : 2);
  } else if (kind == 1 && g_rskip.asked == frame) {
    ReplaySkipAgreed(frame);
  } else if (kind == 2 && g_rskip.asked == frame) {
    REXLOG_INFO("sc5 net: the other game refused cutting the KO's replay short at frame {}", frame);
    g_rskip.asked = 0;
  }
}

void HandlePacket(const uint8_t* b, int n) {
  if (n < 8) return;
  uint32_t magic;
  std::memcpy(&magic, b, 4);
  if (magic != kMagic || b[5] != uint8_t(1 - g_slot)) return;
  const uint8_t type = b[4];
  uint16_t match;
  std::memcpy(&match, b + 6, 2);
  Peer& p = g_peer;
  p.seen = true;
  p.last_heard = Clock::now();
  auto u32 = [&](int o) {
    uint32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
  };
  const uint16_t coming = uint16_t(g_match + 1);
  if (match == coming) p.heard_coming = true;
  if (type == kSums && n >= 13 && g_slot == 0) {
    // player 2's sums, for the barrier of this match or the next (this game may not be there yet)
    if (match != g_match && match != coming) return;
    if (g_sync.match != match) g_sync = MeetSync{match};
    const uint16_t chunk = uint16_t(b[8] | b[9] << 8), chunks = uint16_t(b[10] | b[11] << 8);
    std::vector<SyncBlock> list;
    for (int i = 0; i < b[12] && 13 + i * 16 + 16 <= n; i++) {
      SyncBlock sb;
      sb.source = u32(13 + i * 16);
      sb.size = u32(17 + i * 16);
      sb.hash = uint64_t(u32(21 + i * 16)) | uint64_t(u32(25 + i * 16)) << 32;
      list.push_back(sb);
    }
    g_sync.their_sums[chunk] = std::move(list);
    g_sync.their_chunks = chunks;
    return;
  }
  if (type == kFix && n >= 12 && g_slot == 1) {
    if (match != g_match && match != coming) return;
    if (g_sync.match != match) g_sync = MeetSync{match};
    const uint16_t chunk = uint16_t(b[8] | b[9] << 8), chunks = uint16_t(b[10] | b[11] << 8);
    g_sync.got_chunks = chunks;
    if (chunks && n >= 20) g_sync.got[chunk] = std::vector<uint8_t>(b + 12, b + n);
    // already through the barrier (player 1's game did not hear the FIXOK): again
    if (g_sync.applied && match == g_match) Send(Header(kFixOk));
    return;
  }
  if (type == kFixOk && g_slot == 0) {
    if (match == g_match && g_sync.match == match) g_sync.fix_ok = true;
    return;
  }
  if (type == kPick && n >= 16) {
    if (match != coming || b[8] > 1) return;
    Pick& pk = p.picks[b[8]];
    pk.phase = int8_t(b[9]);
    pk.slot = int8_t(b[10]);
    pk.costume = int8_t(b[11]);
    p.pickable = u32(12);
    if (n >= 20) p.dlc = u32(16);
  } else if (type == kCreated && n >= 13) {
    // their player's created character (only their own side's: a game sends its own player's)
    if (match != coming || b[8] != uint8_t(1 - g_slot)) return;
    const int len = b[11] | b[12] << 8;
    if (13 + len > n) return;
    Incoming& in = g_incoming[b[8]];
    in.Take(b[9], b[10], b + 13, len);
    Writer a = HeaderComing(kCreatedAck);
    a.U8(b[8]);
    a.U16(in.mask);
    Send(a);
  } else if (type == kCreatedAck && n >= 11) {
    if (match != coming || b[8] != uint8_t(g_slot)) return;
    g_created_acked[b[8]] = uint16_t(g_created_acked[b[8]] | (b[9] | b[10] << 8));
  } else if (type == kStage && n >= 12) {
    if (match != coming) return;
    p.stage.chooser = b[8] <= 1 ? b[8] : -1;
    p.stage.phase = int8_t(b[9]);
    p.stage.cursor = int8_t(b[10]);
    p.stage.stage = int8_t(b[11]);
  } else if (type == kChoice && n >= 11) {
    if (match != coming) return;
    p.choice.chooser = b[8] <= 1 ? b[8] : -1;
    p.choice.phase = int8_t(b[9]);
    p.choice.cursor = int8_t(b[10]);
  } else if (type == kRematchAt && n >= 12) {
    if (match != coming) return;
    p.rematch_at = int64_t(u32(8));
  } else if (type == kSkip && n >= 8) {
    if (match != coming) return;
    if (!p.skip_in) REXLOG_INFO("sc5 net: the other player skips the way into match {}", coming);
    p.skip_in = true;
  } else if (type == kReplaySkip && n >= 13) {
    if (match != g_match || g_status != Status::InSync) return;
    OnReplaySkip(u32(8), b[12]);
  } else if (type == kSetup && n >= 21) {
    if (match != coming) return;
    p.has_setup = true;
    p.setup.stage = u32(8);
    p.setup.round_time = u32(12);
    p.setup.wins = u32(16);
    p.setup.words.clear();
    for (int i = 0; i < b[20] && 21 + i * 4 + 4 <= n; i++) p.setup.words.push_back(u32(21 + i * 4));
  } else if (type == kReady && n >= 13) {
    if (match != g_match && match != uint16_t(g_match + 1)) return;
    if (match == p.ready_match) return;
    std::vector<uint32_t> words;
    for (int i = 0; i < b[12] && 13 + i * 4 + 4 <= n; i++) words.push_back(u32(13 + i * 4));
    const int after = 13 + b[12] * 4;
    p.ready_match = match;
    p.seed = u32(8);
    p.words = std::move(words);
    p.delay = after < n ? b[after] : -1;
  } else if (type == kInput && n >= 25 && match == g_match) {
    p.ack = std::max(p.ack, u32(8));
    p.now = std::max(p.now, u32(12));
    p.heard = std::max(p.heard, u32(16));
    const uint32_t first = u32(20);
    const int count = b[24];
    for (int i = 0; i < count && 25 + i * 8 + 8 <= n; i++) {
      if (first + uint32_t(i) > g_frame + 600) break;
      Wire in;
      std::memcpy(&in, b + 25 + i * 8, 8);
      if (g_cfg.sparring && (in.buttons & input::kBack)) g_back_seen = true;
      p.inputs.emplace(first + uint32_t(i), in);
    }
    while (p.inputs.count(p.contiguous)) p.contiguous++;
  } else if (type == kCheck && n >= 20 && match == g_match) {
    p.checks[u32(8)] = uint64_t(u32(12)) | uint64_t(u32(16)) << 32;
  } else if (type == kBye && match == g_match) {
    p.left = true;
    if (g_cfg.sparring) g_back_seen = true;  // Online Practice: the other's leaving ends it as BACK does
  }
}

void WriteSetup(Writer& w) {
  w.U32(g_my_setup.stage);
  w.U32(g_my_setup.round_time);
  w.U32(g_my_setup.wins);
  const size_t n = std::min<size_t>(g_my_setup.words.size(), 200);
  w.U8(uint8_t(n));
  for (size_t i = 0; i < n; i++) w.U32(g_my_setup.words[i]);
}

// this game's picks (every 100 ms, at once when they changed) and player 1's setup (every 250 ms) while the two
// games are on their way to the barrier
void SendComing() {
  const auto now = Clock::now();
  if (g_picks_changed || now - g_last_pick_send > std::chrono::milliseconds(100)) {
    g_picks_changed = false;
    g_last_pick_send = now;
    // the spectators: this player's pick, as it changes
    const Pick& mine = g_local_picks[g_slot];
    if (mine.phase >= 0 &&
        (mine.phase != g_fed_pick.phase || mine.slot != g_fed_pick.slot || mine.costume != g_fed_pick.costume)) {
      g_fed_pick = mine;
      Writer f = FeedHeader(kFeedPick, uint16_t(g_match + 1));
      f.U8(uint8_t(g_slot));
      f.U8(uint8_t(int8_t(mine.phase)));
      f.U8(uint8_t(int8_t(mine.slot)));
      f.U8(uint8_t(int8_t(mine.costume)));
      f.U32(g_local_pickable);
      Feed(f);
    }
    // the stage select: who picks the stage (player 1's game says), where they are; the spectators: the chooser's own
    if (g_local_stage.chooser >= 0) {
      Writer w = HeaderComing(kStage);
      w.U8(uint8_t(g_local_stage.chooser));
      w.U8(uint8_t(int8_t(g_local_stage.phase)));
      w.U8(uint8_t(int8_t(g_local_stage.cursor)));
      w.U8(uint8_t(int8_t(g_local_stage.stage)));
      Send(w);
      const StagePick& f = g_local_stage;
      if (f.chooser == g_slot && f.phase >= 0 &&
          (f.phase != g_fed_stage.phase || f.cursor != g_fed_stage.cursor || f.stage != g_fed_stage.stage)) {
        g_fed_stage = f;
        Writer fs = FeedHeader(kFeedStage, uint16_t(g_match + 1));
        fs.U8(uint8_t(f.chooser));
        fs.U8(uint8_t(int8_t(f.phase)));
        fs.U8(uint8_t(int8_t(f.cursor)));
        fs.U8(uint8_t(int8_t(f.stage)));
        Feed(fs);
      }
    }
    // the result screen's menu, from the game whose player chooses the next match
    if (g_local_choice.chooser >= 0) {
      Writer w = HeaderComing(kChoice);
      w.U8(uint8_t(g_local_choice.chooser));
      w.U8(uint8_t(int8_t(g_local_choice.phase)));
      w.U8(uint8_t(int8_t(g_local_choice.cursor)));
      Send(w);
    }
    // the frames after the match's end this game had the launcher's rematch at
    if (g_local_rematch_at >= 0) {
      Writer w = HeaderComing(kRematchAt);
      w.U32(uint32_t(g_local_rematch_at));
      Send(w);
    }
    // this game's player skips the way into the coming fight; the spectators once
    if (g_local_skip_in) {
      Send(HeaderComing(kSkip));
      if (!g_fed_skip_in) {
        g_fed_skip_in = true;
        Writer f = FeedHeader(kFeedSkip, uint16_t(g_match + 1));
        f.U8(0);
        Feed(f);
      }
    }
    // this game's player's created character: every chunk the other game does not have yet; the spectators once
    for (int s = 0; s < 2; s++) {
      const std::vector<uint8_t>& rec = g_local_created[s];
      if (rec.size() != size_t(kCreatedSize)) continue;
      for (int c = 0; c < kCreatedChunks; c++) {
        const int off = c * kCreatedChunk, len = std::min(kCreatedChunk, kCreatedSize - off);
        const bool acked = (g_created_acked[s] >> c & 1) != 0, feed = !g_created_fed[s];
        if (acked && !feed) continue;
        Writer w = HeaderComing(kCreated);
        w.U8(uint8_t(s));
        w.U8(uint8_t(c));
        w.U8(uint8_t(kCreatedChunks));
        w.U16(uint16_t(len));
        w.Bytes(rec.data() + off, len);
        if (!acked) Send(w);
        if (feed) {
          Writer f = FeedHeader(kFeedCreated, uint16_t(g_match + 1));
          f.U8(uint8_t(s));
          f.U8(uint8_t(c));
          f.U8(uint8_t(kCreatedChunks));
          f.U16(uint16_t(len));
          f.Bytes(rec.data() + off, len);
          Feed(f);
        }
      }
      g_created_fed[s] = true;
    }
    for (int s = 0; s < 2; s++) {
      if (s != g_slot && g_local_picks[s].phase < 0) continue;
      Writer w = HeaderComing(kPick);
      w.U8(uint8_t(s));
      w.U8(uint8_t(int8_t(g_local_picks[s].phase)));
      w.U8(uint8_t(int8_t(g_local_picks[s].slot)));
      w.U8(uint8_t(int8_t(g_local_picks[s].costume)));
      w.U32(g_local_pickable);
      w.U32(g_local_dlc);
      Send(w);
    }
  }
  if (g_my_setup_set && now - g_last_setup_send > std::chrono::milliseconds(250)) {
    g_last_setup_send = now;
    Writer w = HeaderComing(kSetup);
    WriteSetup(w);
    Send(w);
  }
}

// what was passed for a match is done with once it met
void ClearComing() {
  for (int s = 0; s < 2; s++) {
    g_local_created[s].clear();
    g_created_acked[s] = 0;
    g_created_fed[s] = false;
    g_incoming[s] = Incoming{};
  }
  for (Pick& p : g_local_picks) p = Pick{};
  g_local_stage = g_fed_stage = StagePick{};
  g_peer.stage = StagePick{};
  g_local_choice = ResultChoice{};
  g_peer.choice = ResultChoice{};
  g_local_skip_in = g_fed_skip_in = false;
  g_peer.skip_in = false;
  g_local_rematch_at = g_peer.rematch_at = -1;
  g_my_setup_set = false;
  for (Pick& p : g_peer.picks) p = Pick{};
  g_peer.has_setup = false;
  g_peer.heard_coming = false;
}

void Receive() {
  Pump();
  FeedPump();
  if (g_sock == INVALID_SOCKET) return;
  uint8_t b[1500];
  for (;;) {
    sockaddr_in from{};
    int fl = sizeof from;
    const int n = recvfrom(g_sock, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 8) return;
    // only the other game's address is heard
    if (from.sin_addr.s_addr != g_peer.addr.sin_addr.s_addr || from.sin_port != g_peer.addr.sin_port) continue;
    HandlePacket(b, n);
  }
}

void CloseSocket() {
  if (g_sock != INVALID_SOCKET) closesocket(g_sock);
  g_sock = INVALID_SOCKET;
}

bool Open(const Settings& st) {
  static const bool wsa = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  if (!wsa) return false;
  CloseSocket();
  FeedClose();
  g_cfg = st;
  g_slot = st.side == 2 ? 1 : 0;
  g_peer = Peer{};
  g_feed_out.clear();
  g_fed_pick = Pick{};
  g_watched.clear();
  g_watch_mine.clear();
  if (!st.feed.empty() && !ParseAddress(st.feed, g_feed_addr)) {
    REXLOG_ERROR("sc5 net: bad spectator feed address {}", st.feed);
    g_cfg.feed.clear();
  }
  if (st.watch) {
    // a spectator: no socket of its own, both players come from the feed
    if (g_cfg.feed.empty()) return false;
    g_slot = 0;
    for (auto& s : g_ring)
      if (!s) s = battle::NewStream();
    g_match = 0;
    g_match_delay = -1;
    ClearComing();
    g_opened_at = Clock::now();
    g_back_seen = false;
    g_sparring_over = false;
    if (g_log) std::fclose(g_log);
    g_log = st.log.empty() ? nullptr : std::fopen(st.log.c_str(), "w");
    g_status = Status::Connecting;
    g_active = true;
    REXLOG_INFO("sc5 net: watching a match from the spectator feed {}", st.feed);
    return true;
  }
  if (!ParseAddress(st.peer, g_peer.addr)) {
    REXLOG_ERROR("sc5 net: bad peer address {}", st.peer);
    return false;
  }
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock == INVALID_SOCKET) return false;
  u_long nb = 1;
  ioctlsocket(g_sock, FIONBIO, &nb);
  int buf = 1 << 20;
  setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_port = htons(st.bind);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(g_sock, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
    REXLOG_ERROR("sc5 net: cannot bind UDP port {}", st.bind);
    CloseSocket();
    return false;
  }
  if (g_log) std::fclose(g_log);
  g_log = st.log.empty() ? nullptr : std::fopen(st.log.c_str(), "w");
  for (auto& s : g_ring)
    if (!s) s = battle::NewStream();
  g_ring_frame.fill(-1);
  g_match = 0;
  g_match_delay = -1;
  // the matches count from 1 again: what the barrier kept of the last session's match 1 (MeetSync) is not this one's.
  // Kept, its "FIX built / FIXOK came / applied" let both games through the next barrier at once with nothing made
  // alike, and a match started after a park went out of step at its first check (frame 120: 7 of 7 after a desync on
  // two players' PCs, 2026-11-20, each time until both games were closed)
  g_sync = MeetSync{};
  ClearComing();
  g_local_pickable = 0;
  g_opened_at = Clock::now();
  g_back_seen = false;
  g_sparring_over = false;
  g_status = Status::Connecting;
  g_active = true;
  REXLOG_INFO("sc5 net: player {} on port {}, the other at {}, {} with input delay {}{}", st.side, st.bind, st.peer,
              st.rollback ? "rollback" : "lockstep", OwnDelay(),
              st.lag_ms || st.jitter_ms || st.loss_percent
                  ? fmt::format(" (simulated network: {} ms, jitter {} ms, loss {}%)", st.lag_ms, st.jitter_ms,
                                st.loss_percent)
                  : "");
  return true;
}

void StopRolling();
void DumpAtTop(PPCContext& ctx, uint8_t* base, uint32_t f);
void DumpSettled(uint32_t settled);

void ApplyPending() {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  if (g_pending_off) {
    g_pending_off = false;
    if (g_status == Status::InSync) FeedEnd();
    if (g_status != Status::Off) {
      Writer w = Header(kBye);
      w.U32(g_frame);
      Send(w);
    }
    // what the feed still holds goes out before it closes
    for (int i = 0; i < 50 && !g_feed_out.empty() && g_feed != INVALID_SOCKET; i++) {
      FeedPump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    FeedClose();
    StopRolling();
    CloseSocket();
    g_status = Status::Off;
    g_active = false;
    input::SetSource(nullptr);
  }
  if (g_pending_set) {
    g_pending_set = false;
    Open(g_pending);
  }
}

// one line per frame run: its number ("r" in front when a rollback ran it again), both players' inputs, its check
void LogFrame(bool again) {
  if (!g_log) return;
  std::fprintf(g_log, again ? "r%u" : "%u", g_frame);
  for (int s = 0; s < 2; s++)
    std::fprintf(g_log, " %04X:%02X%02X:%02X%02X%02X%02X", g_now[s].buttons, g_now[s].lt, g_now[s].rt,
                 uint8_t(g_now[s].lx), uint8_t(g_now[s].ly), uint8_t(g_now[s].rx), uint8_t(g_now[s].ry));
  if (auto t = g_tentative.find(g_frame); t != g_tentative.end())
    std::fprintf(g_log, " check %016llX", static_cast<unsigned long long>(t->second));
  std::fputc('\n', g_log);
}

std::atomic<bool> g_waiting{false};

// the other's inputs up to frame f are here (false: they are gone)
bool WaitForInputs(uint32_t f) {
  g_waiting = true;
  auto started = Clock::now();
  bool waited = false;
  for (;;) {
    if (g_peer.contiguous > f) break;
    if (g_peer.left || Clock::now() - g_peer.last_heard > kPeerTimeout) {
      REXLOG_WARN("sc5 net: the other game stopped answering at frame {}", f);
      g_waiting = false;
      return false;
    }
    waited = true;
    // ask again: our latest inputs (theirs may wait on ours)
    if (Clock::now() - started > std::chrono::milliseconds(8)) {
      SendInputs();
      started = Clock::now();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    Receive();
  }
  if (waited) g_stats.waits++;
  g_waiting = false;
  return true;
}

void CompareChecks() {
  for (auto it = g_peer.checks.begin(); it != g_peer.checks.end();) {
    auto mine = g_my_checks.find(it->first);
    if (mine == g_my_checks.end()) {
      ++it;
      continue;
    }
    if (mine->second != it->second && g_status != Status::Desynced) {
      REXLOG_ERROR("sc5 net: DESYNC at frame {}: {:016X} here, {:016X} there", it->first, mine->second, it->second);
      static const bool keep_going = std::getenv("SC5_NET_DESYNC_CONTINUE") != nullptr;
      if (!keep_going) g_status = Status::Desynced;
    }
    it = g_peer.checks.erase(it);
  }
}

// the input the other runs with at frame f: theirs when it is here, else a guess: the last one they sent
Wire Guess(uint32_t f) {
  if (auto it = g_peer.inputs.find(f); it != g_peer.inputs.end()) return it->second;
  auto last = g_peer.contiguous ? g_peer.inputs.find(g_peer.contiguous - 1) : g_peer.inputs.end();
  return last != g_peer.inputs.end() ? last->second : Wire{};
}

// the frame running now plays on neutral inputs (outside a round's fighting)
bool g_neutral = false;

// a round is fought: the battle state machine's "fighting", but not a KO's replay, which the game plays in that state
// too (battle::InKoReplay says why it runs on neutral inputs, as the KO around it)
bool RoundFought() { return game::BattleState() == game::kBattleFighting && !battle::InKoReplay(); }

// The fight's random numbers seeded again wherever the battle's state changes outside a round's fighting (the KO, the
// KO's replay and its end, between rounds, the round's call, the round's first frame fought), at the frame's top: the
// two games reach each of these on the same frame with the same fight. Within a round their generators come apart
// though the fight stays alike: some draws from the C library's rand, TT800 and the Mersenne Twister come in a frame's
// first run and not in its rerun, or the other way round (who draws them is not known yet), and the check does not
// hash the generators. Between rounds the game picks from them: Elysium's and Edge Master's style for the next round
// (each player's +0x1B8 and +0x17AC), which the two games picked apart, loaded for 19 and 14 frames and started round
// 3 on frames 5 apart (Elysium against Edge Master with 40 ms each way: TT800 a draw apart from frame 1189 and the
// Twister two from 434, round 3 parted from its first check, 2026-11-18). Seeded as the barrier seeds them
// (sub_824A35A8), from the match's seed and the frame.
uint32_t g_match_seed = 0;
uint32_t g_seeded_state = 0;
uint32_t SeededState() { return game::BattleState() | (battle::InKoReplay() ? 0x10000u : 0u); }
void ReseedAtStateChange(PPCContext& ctx, uint8_t* base) {
  const uint32_t state = SeededState();
  if (state == g_seeded_state) return;
  g_seeded_state = state;
  // the seeding's hook (game.cpp) seeds with the match's seed whatever it is asked: this seed for this one
  const uint32_t seed = g_match_seed ^ (g_frame * 0x9E3779B9u);
  game::SetSeed(int64_t(seed));
  game::Reseed(ctx, base, seed);
  game::SetSeed(int64_t(g_match_seed));
}

// frame f's inputs: what the game's pad reads get until the next frame's top (and its count of frames shown)
void SetFrameInputs(uint32_t f) {
  Put32(kFramesShown, g_shown_base + f);
  if (g_cfg.watch) {  // a spectator: both players' own, from the feed (neutral where theirs were)
    Watched& w = g_watched[g_match];
    for (int s = 0; s < 2; s++) {
      auto it = w.inputs[s].find(f);
      g_now[s] = !g_neutral && it != w.inputs[s].end() ? it->second : Wire{};
    }
    return;
  }
  if (g_neutral) {
    g_now[0] = g_now[1] = Wire{};
    return;
  }
  g_now[g_slot] = g_mine[f];
  const Wire w = Guess(f);
  g_peer.used[f] = w;
  g_now[1 - g_slot] = w;
}

// the state at the top of frame f into the ring (rolling), with what goes back with it
void Keep(PPCContext& ctx, uint8_t* base, uint32_t f) {
  const int slot = int(f % kRing);
  if (!battle::Save(ctx, base, g_ring[slot])) {
    g_ring_frame[slot] = -1;
    return;
  }
  g_ring_frame[slot] = int64_t(f);
  g_ring_count[slot] = frame::Count();
  input::KeepKeys(frame::Count());
  if (f % kCheckEvery == 0) {
    g_tentative[f] = battle::FightHash(g_ring[slot]);
    // tests: SC5_NET_DUMP_DIR, the first checks' snapshots and their blocks (what came from where), to compare games
    static int dumped = 0;
    static const int dump_count = std::getenv("SC5_NET_DUMP_COUNT") ? std::atoi(std::getenv("SC5_NET_DUMP_COUNT")) : 3;
    static const uint32_t dump_from = std::getenv("SC5_NET_DUMP_FROM") ? uint32_t(std::atoi(std::getenv("SC5_NET_DUMP_FROM"))) : 0;
    if (const char* dir = std::getenv("SC5_NET_DUMP_DIR"); dir && *dir && dumped < dump_count && f >= dump_from && !frame::Replaying()) {
      dumped++;
      const std::string stem = fmt::format("{}/net{}_{}", dir, g_slot + 1, f);
      if (FILE* out = std::fopen((stem + ".bin").c_str(), "wb")) {
        std::fwrite(battle::Data(g_ring[slot]), 1, battle::Size(g_ring[slot]), out);
        std::fclose(out);
      }
      if (FILE* out = std::fopen((stem + ".blocks").c_str(), "w")) {
        if (const auto* blocks = battle::Blocks(g_ring[slot]))
          for (const auto& b : *blocks) std::fprintf(out, "%u %u %08X\n", b.pos, b.size, b.source);
        std::fclose(out);
      }
    }
  }
}

// a check is sent once every input before its frame is known (it no longer changes)
void SettleChecks() {
  const uint32_t settled = std::min(g_frame, g_peer.confirmed);
  while (!g_tentative.empty() && g_tentative.begin()->first <= settled) {
    const uint32_t f = g_tentative.begin()->first;
    const uint64_t h = g_tentative.begin()->second;
    g_my_checks[f] = h;
    Writer w = Header(kCheck);
    w.U32(f);
    w.U32(uint32_t(h));
    w.U32(uint32_t(h >> 32));
    Send(w);
    Writer fc = FeedHeader(kFeedCheck, g_match);
    fc.U32(f);
    fc.U32(uint32_t(h));
    fc.U32(uint32_t(h >> 32));
    Feed(fc);
    g_tentative.erase(g_tentative.begin());
  }
  while (g_my_checks.size() > 64) g_my_checks.erase(g_my_checks.begin());
}

void StartRolling(PPCContext& ctx, uint8_t* base) {
  g_rolling = true;
  g_ring_frame.fill(-1);
  g_pace = Pacing{};
  g_peer.confirmed = g_frame;
  REXLOG_INFO("sc5 net: rollback from frame {} of match {}", g_frame, g_match);
  (void)ctx;
  (void)base;
}

void StopRolling() {
  if (!g_rolling) return;
  g_rolling = false;
  REXLOG_INFO("sc5 net: lockstep from frame {} of match {} ({} rollbacks so far, {} frames run again, the longest {} "
              "frames, {:.1f} ms; {} frames held back)",
              g_frame, g_match, g_stats.rollbacks, g_stats.rerun, g_stats.max_rollback, g_stats.longest_rollback_ms,
              g_stats.holds);
}

void Lost(Status s) {
  if (g_status == Status::InSync) FeedEnd();
  StopRolling();
  g_status = s;
  input::SetSource(nullptr);
  REXLOG_WARN("sc5 net: match {} over: {}", g_match, StatusName(s));
}

// Inputs that came for frames that ran on a guess: when a guess was wrong, the battle goes back to the top of that
// frame and the frames since run again with what is known now. False when it cannot go back that far.
bool CorrectGuesses(PPCContext& ctx, uint8_t* base) {
  uint32_t wrong = g_frame;
  const uint32_t known = std::min(g_peer.contiguous, g_frame);
  for (uint32_t f = g_peer.confirmed; f < known; f++) {
    auto u = g_peer.used.find(f);
    if (u != g_peer.used.end() && !(u->second == g_peer.inputs[f])) {
      wrong = f;
      break;
    }
  }
  g_peer.confirmed = std::max(g_peer.confirmed, known);
  if (wrong >= g_frame) return true;
  const uint32_t now = g_frame;
  const int slot = int(wrong % kRing);
  if (g_ring_frame[slot] != int64_t(wrong)) {
    REXLOG_ERROR("sc5 net: cannot go back to frame {} from {} (not kept)", wrong, now);
    return false;
  }
  const auto t0 = Clock::now();
  if (!battle::Restore(ctx, base, g_ring[slot])) {
    REXLOG_ERROR("sc5 net: the game would not take back frame {}", wrong);
    return false;
  }
  frame::SetCount(g_ring_count[slot]);
  input::KeysBack(g_ring_count[slot]);
  sound::RollbackBegin(g_ring_count[slot]);
  const auto t_restored = Clock::now();
  Clock::duration keeping{}, running{};
  // A frame run again whose inputs are all known now is never gone back to again: it is kept again only for its check
  // (every kCheckEvery frames). The frames after the last known input still are guesses, kept as before.
  for (uint32_t f = wrong; f < now; f++) {
    g_frame = f;
    if (f > wrong) {
      if (KeepAll() || f >= g_peer.confirmed || f % kCheckEvery == 0) {
        const auto k0 = Clock::now();
        Keep(ctx, base, f);
        keeping += Clock::now() - k0;
        g_cost.keeps++;
      } else {
        g_ring_frame[f % kRing] = -1;  // the first run's snapshot of it is of the guess
      }
    }
    SetFrameInputs(f);
    sparring::OnFrame(f);  // Online Practice's rules, as the frame's first run had them
    DumpAtTop(ctx, base, f);
    const auto r0 = Clock::now();
    frame::RunAgain(ctx, base);
    running += Clock::now() - r0;
    LogFrame(true);
  }
  sound::RollbackEnd(ctx, base);  // the sounds only the wrong guess started stop
  g_frame = now;
  g_cost.restore_ms += Ms(t_restored - t0);
  g_cost.keep_ms += Ms(keeping);
  g_cost.run_ms += Ms(running);
  g_cost.runs += now - wrong;
  g_cost.rollbacks++;
  const double ms = Ms(Clock::now() - t0);
  const uint32_t depth = now - wrong;
  g_stats.rollbacks++;
  g_stats.rerun += depth;
  g_stats.max_rollback = std::max(g_stats.max_rollback, depth);
  g_stats.rollback_ms += ms;
  g_stats.longest_rollback_ms = std::max(g_stats.longest_rollback_ms, ms);
  return true;
}

// Keep the sides even: the game ahead guesses the other's inputs further ahead, and every guess that was wrong is a
// rollback that long. A game ahead by more than a frame and a quarter (measured both ways, averaged over 8 frames)
// for a quarter of a second holds a frame back (one vertical blank left out: the screen shows a frame once more), and
// then as often as every other frame while it stays ahead.
void Pace() {
  if (!g_peer.now) return;
  const int mine = int(g_frame) - int(g_peer.now);
  const int theirs = int(g_peer.now) - int(g_peer.heard);
  g_pace.samples.push_back(mine - theirs);
  while (g_pace.samples.size() > 8) g_pace.samples.pop_front();
  if (g_pace.samples.size() < 8) return;
  double avg = 0;
  for (int s : g_pace.samples) avg += s;
  avg /= double(g_pace.samples.size());
  const bool ahead = avg > 2.5;
  g_pace.streak = ahead ? g_pace.streak + 1 : 0;
  const bool following = g_pace.had_hold && g_frame - g_pace.last_hold < 40;
  if (!ahead || (g_pace.streak < 14 && !following)) return;
  if (g_pace.had_hold && g_frame - g_pace.last_hold < 2) return;
  g_pace.had_hold = true;
  g_pace.last_hold = g_frame;
  g_stats.holds++;
  // the next blank is left out: the screen shows this frame once more and this game runs a frame fewer (a sleep here
  // was made up by the frames after it: the main thread runs ahead of the GPU)
  if (auto* rt = rex::Runtime::instance(); rt && rt->graphics_system()) rt->graphics_system()->HoldVblanks(1);
  // the lead it showed is half gone (ours a frame less now): what the average holds of it counts no more
  for (int& s : g_pace.samples) s -= 2;
}

void SendOwn() {
  input::Pad local;
  if (!testinput::ScriptedPad(uint32_t(g_slot), local)) local = input::ReadPhysical(0);
  // the player's own buttons for fights (the launcher's [fight]) as the game's default layout presses them: both games
  // and the spectators see the combination the player meant, whatever each player set (sc5/controls.h)
  controls::ToDefaultLayout(local);
  const uint32_t at = g_frame + uint32_t(Delay());
  // no pausing a match: START stays with this PC, and BACK too but in Online Practice, where it ends the match (it
  // goes to the other game, which ends it as well; neither game's pad reads see it)
  local.buttons &= uint16_t(~input::kStart);
  if (!g_cfg.sparring) local.buttons &= uint16_t(~input::kBack);
  else if (local.buttons & input::kBack) g_back_seen = true;
  g_mine[at] = Pack(local);
  SendInputs();
  // the spectators: this player's input for that frame
  if (!g_cfg.feed.empty()) {
    const Wire& in = g_mine[at];
    Writer f = FeedHeader(kFeedInputs, g_match);
    f.U32(at);
    f.U8(1);
    f.U32(uint32_t(in.buttons) | uint32_t(in.lt) << 16 | uint32_t(in.rt) << 24);
    f.U32(uint32_t(uint8_t(in.lx)) | uint32_t(uint8_t(in.ly)) << 8 | uint32_t(uint8_t(in.rx)) << 16 |
          uint32_t(uint8_t(in.ry)) << 24);
    Feed(f);
  }
}

// the game's pad reads in a match: the frame's inputs (user 0 player 1, user 1 player 2)
bool FrameInput(uint32_t user, input::Pad& out) {
  if (g_status != Status::InSync) return false;
  if (user >= 2) {
    out = input::Pad{};
    return true;
  }
  out = Unpack(g_now[user]);
  out.buttons &= uint16_t(~input::kBack);  // Online Practice's BACK is the netplay's, not the game's
  // a KO's replay cut short (RSKIP): A on both pads for a few frames from the agreed frame, in both games
  if (g_rskip.press_at && g_frame >= g_rskip.press_at && g_frame < g_rskip.press_at + kReplaySkipPress)
    out.buttons |= input::kA;
  // a new packet every frame, numbered by the frame: the same when a rollback runs it again
  out.packet = (uint32_t(g_match) << 24) + g_frame + 1;
  return true;
}

// this game's fight blocks now (MeetSync): source, size, hash of the fight's bytes, and the bytes (player 1)
std::vector<SyncBlock> FightBlocks(PPCContext& ctx, uint8_t* base, bool with_data) {
  std::vector<SyncBlock> out;
  if (!g_sync_stream) g_sync_stream = battle::NewStream();
  if (!g_sync_stream || !battle::Save(ctx, base, g_sync_stream)) return out;
  const auto* blocks = battle::Blocks(g_sync_stream);
  const uint8_t* data = battle::Data(g_sync_stream);
  const uint32_t size = battle::Size(g_sync_stream);
  if (!blocks || !data) return out;
  for (const battle::Block& b : *blocks) {
    if (b.pos + b.size > size) continue;
    uint64_t h = 1469598103934665603ull;
    bool fight = false;
    for (uint32_t i = 0; i < b.size; i++) {
      if (battle::KindOf(b.source + i) != battle::Kind::Fight) continue;
      fight = true;
      h = (h ^ data[b.pos + i]) * 1099511628211ull;
    }
    if (!fight) continue;
    SyncBlock sb{b.source, b.size, h, {}};
    if (with_data) sb.data.assign(data + b.pos, data + b.pos + b.size);
    out.push_back(std::move(sb));
  }
  return out;
}

void SendPacket(const std::vector<uint8_t>& v) {
  Writer w;
  std::memcpy(w.b, v.data(), std::min(v.size(), sizeof w.b));
  w.n = int(std::min(v.size(), sizeof w.b));
  Send(w);
}

// player 2's game: its sums out, in chunks of 60
void SendSums() {
  const auto& mine = g_sync.mine;
  const uint16_t chunks = uint16_t(std::max<size_t>(1, (mine.size() + 59) / 60));
  for (uint16_t c = 0; c < chunks; c++) {
    Writer w = Header(kSums);
    w.U16(c);
    w.U16(chunks);
    const size_t from = size_t(c) * 60, to = std::min(mine.size(), from + 60);
    w.U8(uint8_t(to - from));
    for (size_t i = from; i < to; i++) {
      w.U32(mine[i].source);
      w.U32(mine[i].size);
      w.U32(uint32_t(mine[i].hash));
      w.U32(uint32_t(mine[i].hash >> 32));
    }
    Send(w);
  }
}

// player 1's game: once all of player 2's sums came, the FIX packets for every block that differs (blocks matched by
// their source and how many times it came before)
void BuildFix() {
  if (g_sync.fix_built || g_sync.their_chunks < 0 || int(g_sync.their_sums.size()) < g_sync.their_chunks) return;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> theirs;
  {
    std::map<uint32_t, uint32_t> seen;
    for (auto& [chunk, list] : g_sync.their_sums)
      for (const SyncBlock& sb : list) theirs[{sb.source, seen[sb.source]++}] = sb.hash;
  }
  std::map<uint32_t, uint32_t> seen;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pieces;  // source, bytes (at most 1100)
  for (const SyncBlock& sb : g_sync.mine) {
    const auto key = std::make_pair(sb.source, seen[sb.source]++);
    auto it = theirs.find(key);
    if (it != theirs.end() && it->second == sb.hash) continue;
    g_sync.blocks++;
    g_sync.bytes += sb.size;
    for (uint32_t off = 0; off < sb.size; off += 1100) {
      const uint32_t len = std::min<uint32_t>(1100, sb.size - off);
      std::vector<uint8_t> piece(8);
      std::memcpy(piece.data(), &sb.source, 4);
      const uint16_t o16 = uint16_t(off), l16 = uint16_t(len);
      std::memcpy(piece.data() + 4, &o16, 2);
      std::memcpy(piece.data() + 6, &l16, 2);
      piece.insert(piece.end(), sb.data.begin() + off, sb.data.begin() + off + len);
      pieces.emplace_back(sb.source, std::move(piece));
    }
  }
  const uint16_t chunks = uint16_t(pieces.size());
  for (uint16_t c = 0; c < chunks || (chunks == 0 && c == 0); c++) {
    Writer w = Header(kFix);
    w.U16(c);
    w.U16(chunks);
    if (chunks) w.Bytes(pieces[c].second.data(), int(pieces[c].second.size()));
    g_sync.fix.emplace_back(w.b, w.b + w.n);
  }
  g_sync.fix_built = true;
  REXLOG_INFO("sc5 net: match {}: {} blocks of the fight ({} bytes) differ in player 2's game, sent", g_match,
              g_sync.blocks, g_sync.bytes);
}

// player 2's game: all of player 1's bytes here: written where they came from (the fight's bytes only)
void ApplyFix() {
  if (g_sync.applied || g_sync.got_chunks < 0 || int(g_sync.got.size()) < g_sync.got_chunks) return;
  uint32_t bytes = 0;
  std::set<uint32_t> blocks;
  for (auto& [chunk, piece] : g_sync.got) {
    if (piece.size() < 8) continue;
    uint32_t source;
    uint16_t off, len;
    std::memcpy(&source, piece.data(), 4);
    std::memcpy(&off, piece.data() + 4, 2);
    std::memcpy(&len, piece.data() + 6, 2);
    if (piece.size() < 8u + len) continue;
    uint8_t* to = Mem()->TranslateVirtual<uint8_t*>(source + off);
    for (uint32_t i = 0; i < len; i++) {
      if (battle::KindOf(source + off + i) != battle::Kind::Fight) continue;
      to[i] = piece[8 + i];
      bytes++;
    }
    blocks.insert(source);
  }
  g_sync.applied = true;
  REXLOG_INFO("sc5 net: match {}: {} bytes of the fight in {} blocks taken from player 1's game", g_match, bytes,
              blocks.size());
}

// The barrier: once this game's fight has loaded and played its intro (round 1's call), the other's READY of this
// match and the fight's blocks made alike (MeetSync). The two games then agree on the seed (player 1's), take player
// 1's counters and state machine, and start the match's frames together.
bool Meet(PPCContext& ctx, uint8_t* base) {
  g_match++;
  g_rskip = ReplaySkip{};
  g_status = Status::Barrier;
  Peer& p = g_peer;
  p.inputs.clear();
  p.used.clear();
  p.contiguous = p.confirmed = p.ack = p.now = p.heard = 0;
  p.checks.clear();
  p.left = false;
  p.last_heard = Clock::now();
  g_mine.clear();
  g_my_checks.clear();
  g_tentative.clear();
  std::vector<uint32_t> words;
  const std::vector<uint32_t> agreed = AgreedAddresses();
  for (uint32_t a : agreed) words.push_back(Get32(a));
  const uint32_t my_seed = uint32_t(game::FightSeed());
  Writer w = Header(kReady);
  w.U32(my_seed);
  w.U8(uint8_t(words.size()));
  for (uint32_t v : words) w.U32(v);
  w.U8(uint8_t(OwnDelay()));
  const std::string fight = controls::FightText();
  REXLOG_INFO("sc5 net: meeting for match {} at frame {} (this player's buttons in a fight: {})", g_match,
              frame::Count(), fight.empty() ? "the game's own" : fight);
  // the fight's state alike (MeetSync): what came early for this match is kept
  if (g_sync.match != g_match) g_sync = MeetSync{g_match};
  g_sync.mine = FightBlocks(ctx, base, g_slot == 0);
  // the spectators: this game's READY; player 1's game also its whole fight (a spectator takes it all, as player 2's
  // game takes what differs)
  if (!g_cfg.feed.empty()) {
    Writer f = FeedHeader(kFeedMeet, g_match);
    f.U8(kMeetReady);
    f.U32(my_seed);
    f.U8(uint8_t(words.size()));
    for (uint32_t v : words) f.U32(v);
    f.U8(uint8_t(OwnDelay()));
    Feed(f);
    if (g_slot == 0) {
      std::vector<std::vector<uint8_t>> pieces;
      for (const SyncBlock& sb : g_sync.mine)
        for (uint32_t off = 0; off < sb.size; off += 1100) {
          const uint32_t len = std::min<uint32_t>(1100, sb.size - off);
          std::vector<uint8_t> piece(8);
          std::memcpy(piece.data(), &sb.source, 4);
          const uint16_t o16 = uint16_t(off), l16 = uint16_t(len);
          std::memcpy(piece.data() + 4, &o16, 2);
          std::memcpy(piece.data() + 6, &l16, 2);
          piece.insert(piece.end(), sb.data.begin() + off, sb.data.begin() + off + len);
          pieces.push_back(std::move(piece));
        }
      for (size_t c = 0; c < pieces.size(); c++) {
        Writer p = FeedHeader(kFeedMeet, g_match);
        p.U8(kMeetBlock);
        p.U16(uint16_t(c));
        p.U16(uint16_t(pieces.size()));
        p.Bytes(pieces[c].data(), int(pieces[c].size()));
        Feed(p);
      }
    }
    FeedPump();
  }
  const auto started = Clock::now();
  auto last_send = Clock::time_point{};
  bool synced = false;
  for (;;) {
    if (Clock::now() - last_send > std::chrono::milliseconds(50)) {
      Send(w);
      // player 2: its sums until player 1's answer is here, FIXOK once taken; player 1: its FIX until FIXOK
      if (g_slot == 1 && g_sync.got_chunks < 0) SendSums();
      if (g_slot == 1 && g_sync.applied) Send(Header(kFixOk));
      if (g_slot == 0 && g_sync.fix_built && !g_sync.fix_ok)
        for (const auto& packet : g_sync.fix) SendPacket(packet);
      last_send = Clock::now();
    }
    if (g_slot == 0) {
      const bool was_built = g_sync.fix_built;
      BuildFix();
      if (!was_built && g_sync.fix_built)
        for (const auto& packet : g_sync.fix) SendPacket(packet);
      synced = g_sync.fix_ok;
    } else {
      const bool was_applied = g_sync.applied;
      ApplyFix();
      if (!was_applied && g_sync.applied) Send(Header(kFixOk));
      synced = g_sync.applied;
    }
    // the setup again (player 2's game may still wait for it at its battle setup); its match number is this one's
    if (g_my_setup_set && Clock::now() - g_last_setup_send > std::chrono::milliseconds(250)) {
      g_last_setup_send = Clock::now();
      Writer s = Header(kSetup);
      WriteSetup(s);
      Send(s);
    }
    Receive();
    if (p.ready_match == g_match && synced) break;
    if (Clock::now() - started > kBarrierTimeout) {
      REXLOG_WARN("sc5 net: the other game did not come to match {}", g_match);
      g_status = Status::Disconnected;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; i++) Send(w);  // a few more, should one be lost
  if (g_slot == 1)
    for (int i = 0; i < 3; i++) Send(Header(kFixOk));
  const uint32_t seed = g_slot == 0 ? my_seed : p.seed;
  {
    // the agreed words: each counter the larger of the two games' (READY's: what each had on coming) and never below
    // this game's own now (CountsUp); the rest player 1's, which player 2's game takes. The log names what this game
    // had otherwise (a counter that went up, or the words a game far along its own way had)
    std::string differed;
    int n = 0;
    for (size_t i = 0; i < agreed.size() && i < words.size() && i < p.words.size(); i++) {
      const uint32_t mine = Get32(agreed[i]);
      const uint32_t p1 = g_slot == 0 ? words[i] : p.words[i], p2 = g_slot == 0 ? p.words[i] : words[i];
      const uint32_t v = CountsUp(agreed[i]) ? std::max({p1, p2, mine}) : g_slot == 0 ? mine : p1;
      if (mine == v) continue;
      if (n++ < 16) differed += fmt::format(" {:08X}:{:08X}>{:08X}", agreed[i], mine, v);
      Put32(agreed[i], v);
    }
    if (n) REXLOG_INFO("sc5 net: match {}: {} agreed words changed:{}", g_match, n, differed);
  }
  g_shown_base = Get32(kFramesShown);  // the larger count, in both games
  // the fight's random numbers from the agreed seed on both PCs (the game's own seeding: sub_824A35A8)
  game::SetSeed(int64_t(seed));
  game::Reseed(ctx, base, seed);
  g_match_seed = seed;
  g_seeded_state = SeededState();
  // the visual systems' random numbers (sc5/rng.h) from the agreed ones too: their own streams went apart before the
  // match (menus, result screens), and the intro draws from them (a rematch's intro played otherwise on the two PCs)
  rng::ResetVisual();
  // the netplay's delay stands in for the game's own
  Put32(kGameDelay, 0);
  Put32(kGameDelay + 4, 0);
  g_match_delay = std::max(OwnDelay(), p.delay);
  g_frame = 0;
  for (int f = 0; f < Delay(); f++) {
    g_mine[uint32_t(f)] = Wire{};
    p.inputs[uint32_t(f)] = Wire{};
  }
  p.contiguous = uint32_t(Delay());
  while (p.inputs.count(p.contiguous)) p.contiguous++;
  g_stats = Stats{};
  g_cost = Cost{};
  g_match_start = Clock::now();
  g_status = Status::InSync;
  ClearComing();
  input::SetSource(&FrameInput);
  REXLOG_INFO("sc5 net: match {} in sync, seed {:08X}, input delay {}", g_match, seed, Delay());
  return true;
}

// --- a spectator ---

// the meeting: both players' READY of this match and player 1's fight through the feed (however long ago they came);
// then player 1's counters, state machine and fight taken as player 2's game takes them, and the match's frames
bool WatchMeet(PPCContext& ctx, uint8_t* base) {
  g_match++;
  g_rskip = ReplaySkip{};
  g_status = Status::Barrier;
  REXLOG_INFO("sc5 net: a spectator meeting match {} at frame {}", g_match, frame::Count());
  const auto started = Clock::now();
  for (;;) {
    FeedPump();
    const Watched& w = g_watched[g_match];
    if (w.ready[0] && w.ready[1] && w.block_chunks >= 0 && int(w.blocks.size()) >= w.block_chunks) break;
    if (Clock::now() - started > std::chrono::minutes(5)) {
      REXLOG_WARN("sc5 net: the feed did not bring both players to match {}", g_match);
      g_status = Status::Disconnected;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Watched& w = g_watched[g_match];
  const std::vector<uint32_t> agreed = AgreedAddresses();
  for (size_t i = 0; i < agreed.size() && i < w.words.size(); i++)
    Put32(agreed[i], CountsUp(agreed[i]) && i < w.words2.size() ? std::max(w.words[i], w.words2[i]) : w.words[i]);
  g_shown_base = Get32(kFramesShown);
  uint32_t bytes = 0;
  for (auto& [chunk, piece] : w.blocks) {
    if (piece.size() < 8) continue;
    uint32_t source;
    uint16_t off, len;
    std::memcpy(&source, piece.data(), 4);
    std::memcpy(&off, piece.data() + 4, 2);
    std::memcpy(&len, piece.data() + 6, 2);
    if (piece.size() < 8u + len) continue;
    uint8_t* to = Mem()->TranslateVirtual<uint8_t*>(source + off);
    for (uint32_t i = 0; i < len; i++) {
      if (battle::KindOf(source + off + i) != battle::Kind::Fight) continue;
      to[i] = piece[8 + i];
      bytes++;
    }
  }
  game::SetSeed(int64_t(w.seed));
  game::Reseed(ctx, base, w.seed);
  g_match_seed = w.seed;
  g_seeded_state = SeededState();
  rng::ResetVisual();
  Put32(kGameDelay, 0);
  Put32(kGameDelay + 4, 0);
  g_match_delay = std::max({w.delay[0], w.delay[1], 0});
  g_frame = 0;
  for (int s = 0; s < 2; s++) {
    for (int f = 0; f < g_match_delay; f++) w.inputs[s].emplace(uint32_t(f), Wire{});
    while (w.inputs[s].count(w.contiguous[s])) w.contiguous[s]++;
  }
  g_watch_mine.clear();
  g_watch_rolling = false;
  g_stats = Stats{};
  g_match_start = Clock::now();
  g_status = Status::InSync;
  ClearComing();
  input::SetSource(&FrameInput);
  REXLOG_INFO("sc5 net: watching match {} in sync: seed {:08X}, input delay {}, {} bytes of player 1's fight taken",
              g_match, w.seed, g_match_delay, bytes);
  return true;
}

// the spectator's check of frame f (the players' at the same top of frame) against theirs
void WatchCheck(PPCContext& ctx, uint8_t* base, uint32_t f) {
  if (f % kCheckEvery != 0 || !RoundFought()) return;
  if (!g_watch_stream) g_watch_stream = battle::NewStream();
  if (!g_watch_stream || !battle::Save(ctx, base, g_watch_stream)) return;
  g_watch_mine[f] = battle::FightHash(g_watch_stream);
  Watched& w = g_watched[g_match];
  for (auto it = g_watch_mine.begin(); it != g_watch_mine.end();) {
    bool done = false;
    for (int s = 0; s < 2; s++) {
      auto theirs = w.checks[s].find(it->first);
      if (theirs == w.checks[s].end()) continue;
      done = true;
      if (theirs->second != it->second && g_status != Status::Desynced) {
        REXLOG_ERROR("sc5 net: the watched match went apart at frame {}: {:016X} here, {:016X} in player {}'s game",
                     it->first, it->second, theirs->second, s + 1);
        g_status = Status::Desynced;
      } else if (theirs->second == it->second) {
        static uint32_t agreed = 0;
        if (++agreed % 20 == 1)
          REXLOG_INFO("sc5 net: the watched match agrees with the players' games ({} checks so far, frame {})", agreed,
                      it->first);
      }
    }
    if (done || it->first + 600 < f) it = g_watch_mine.erase(it);
    else ++it;
  }
}

// both players' inputs of frame f here: false when they will not come (a player left the match before it)
bool WatchWait(uint32_t f) {
  const auto started = Clock::now();
  for (;;) {
    const Watched& w = g_watched[g_match];
    if (w.contiguous[0] > f && w.contiguous[1] > f) return true;
    for (int s = 0; s < 2; s++)
      if (w.contiguous[s] <= f && w.ended[s] && w.end_frame[s] <= f + uint32_t(std::max(g_match_delay, 0))) {
        REXLOG_INFO("sc5 net: player {} left the watched match at frame {}", s + 1, w.end_frame[s]);
        return false;
      }
    if (Clock::now() - started > std::chrono::seconds(20)) {
      REXLOG_WARN("sc5 net: the feed brought no inputs for frame {} of the watched match", f);
      return false;
    }
    g_waiting = true;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    FeedPump();
  }
}

// Which frames the players ran on neutral inputs (OnTop): a frame whose top is outside a round's fighting, unless the
// frame before was fought (that one still takes the real inputs, as the players' last rolled frame does)
void WatchNeutral() {
  const bool fighting = RoundFought();
  if (g_watch_rolling) {
    if (!fighting) g_watch_rolling = false;
    g_neutral = false;
  } else if (fighting) {
    g_watch_rolling = true;
    g_neutral = false;
  } else {
    g_neutral = true;
  }
}

// a spectator's frame: never a guess, the frame waits for both inputs; far behind the players (a spectator who came
// late), frames run unseen until it is within half a second of them
void ReplaySkipTop();

void WatchTop(PPCContext& ctx, uint8_t* base) {
  FeedPump();
  const Watched& w = g_watched[g_match];
  int unseen = 0;
  while (unseen < 120 && std::min(w.contiguous[0], w.contiguous[1]) > g_frame + 30 && RoundFought()) {
    WatchNeutral();
    ReseedAtStateChange(ctx, base);  // as the players' games do
    ReplaySkipTop();
    SetFrameInputs(g_frame);
    sparring::OnFrame(g_frame);
    WatchCheck(ctx, base, g_frame);
    frame::RunAgain(ctx, base);
    LogFrame(true);
    g_frame++;
    g_stats.frames++;
    unseen++;
  }
  WatchNeutral();
  if (!g_neutral && !WatchWait(g_frame)) {
    g_waiting = false;
    if (g_cfg.sparring) {  // Online Practice: a player's BACK ended it
      g_sparring_over = true;
      g_status = Status::Connecting;
      input::SetSource(nullptr);
      return;
    }
    return Lost(Status::Disconnected);
  }
  g_waiting = false;
  ReseedAtStateChange(ctx, base);
  ReplaySkipTop();
  SetFrameInputs(g_frame);
  sparring::OnFrame(g_frame);
  WatchCheck(ctx, base, g_frame);
}

// A KO's replay cut short (RSKIP): this game's player's START or A while one plays asks the other game for a frame some
// 30 ahead (tests: SC5_TEST_SKIP_REPLAY=<n> asks n frames into each replay)
void ReplaySkipAsk() {
  input::Pad own;
  if (!testinput::ScriptedPad(uint32_t(g_slot), own)) own = input::ReadPhysical(0);
  const bool down = (own.buttons & (input::kStart | input::kA)) != 0;
  bool press = down && !g_rskip_held;
  g_rskip_held = down;
  if (!battle::InKoReplay()) {
    g_rskip.replay_frames = 0;
    return;
  }
  g_rskip.replay_frames++;
  static const int64_t test = [] {
    const char* v = std::getenv("SC5_TEST_SKIP_REPLAY");
    return v ? std::atoll(v) : -1;
  }();
  if (test >= 0 && int64_t(g_rskip.replay_frames) == test) press = true;
  if (press && !g_rskip.at && !g_rskip.asked) {
    g_rskip.asked = g_frame + kReplaySkipAhead;
    g_rskip.sent = Clock::now();
    SendReplaySkip(g_rskip.asked, 0);
    REXLOG_INFO("sc5 net: this player cuts the KO's replay short: frame {} asked (now {})", g_rskip.asked, g_frame);
  } else if (g_rskip.asked && Clock::now() - g_rskip.sent > std::chrono::milliseconds(100)) {
    g_rskip.sent = Clock::now();
    SendReplaySkip(g_rskip.asked, 0);
  }
}

// the asked frame reached and no answer yet: this game waits for the other's word (it may have agreed)
bool ReplaySkipWait() {
  if (!g_rskip.asked || g_frame < g_rskip.asked) return true;
  const auto start = Clock::now();
  while (g_rskip.asked) {
    Receive();
    if (!g_rskip.asked) break;
    if (Clock::now() - g_rskip.sent > std::chrono::milliseconds(100)) {
      g_rskip.sent = Clock::now();
      SendReplaySkip(g_rskip.asked, 0);
    }
    if (Clock::now() - start > kPeerTimeout) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// at the top of a match frame (a spectator's too, from the feed): the agreed frame decides the presses
void ReplaySkipTop() {
  uint32_t at = g_rskip.at;
  if (g_cfg.watch) {
    const std::set<uint32_t>& s = g_watched[g_match].replay_skips;
    auto it = s.lower_bound(g_frame);
    at = it != s.end() && *it == g_frame ? *it : 0;
  }
  if (at && g_frame == at && !g_rskip.press_at) {
    if (battle::InKoReplay()) {
      g_rskip.press_at = at;
      REXLOG_INFO("sc5 net: frame {}: A for the KO's replay, cut short", at);
    } else {
      REXLOG_INFO("sc5 net: frame {}: the KO's replay was over already", at);
    }
  }
  if (g_rskip.press_at && g_frame >= g_rskip.press_at + kReplaySkipPress) {
    g_rskip.press_at = 0;
    if (!g_cfg.watch) g_rskip.at = 0;
  }
}

void OnTop(PPCContext& ctx, uint8_t* base) {
  ApplyPending();
  if (g_status == Status::Off) return;
  Receive();
  if (g_cfg.watch) {
    if (g_status == Status::Connecting && game::BattleState() == game::kBattleRoundCall) {
      if (!WatchMeet(ctx, base)) return Lost(Status::Disconnected);
    }
    if (g_status == Status::InSync) WatchTop(ctx, base);
    return;
  }
  if (g_status == Status::Connecting) {
    if (Clock::now() - g_last_hello > std::chrono::milliseconds(250)) {
      Send(Header(kHello));
      g_last_hello = Clock::now();
    }
    SendComing();
    // the other game gone quiet on the way (or never there): a player's menus take a minute or two at most
    const auto quiet = Clock::now() - (g_peer.seen ? g_peer.last_heard : g_opened_at);
    if (quiet > (g_peer.seen ? std::chrono::seconds(20) : std::chrono::seconds(120))) {
      REXLOG_WARN("sc5 net: {} on the way to match {}", g_peer.seen ? "the other game went quiet" : "no word from the other game",
                  g_match + 1);
      return Lost(Status::Disconnected);
    }
    // the fight has loaded and played its intro: the first frame of round 1's call. (The intro goes as the characters'
    // voice lines play: on two PCs it ran on different frames from the second match on. The block sync at the
    // barrier makes what it left alike.) A rematch either loads again (through Character Select) or is VS Battle's own
    // Rematch, the fight kept loaded: its "between rounds" counts on from the result screens, however long each game
    // stayed there, and the barrier's agreed state machine and block sync make that alike too.
    if (game::BattleState() != game::kBattleRoundCall) return;
    if (!Meet(ctx, base)) return Lost(Status::Disconnected);
  }
  if (g_status != Status::InSync) return;
  // Online Practice: a player's BACK (or the other game's leaving) ends the match in sync for both
  if (g_cfg.sparring && g_back_seen && !g_cfg.watch) {
    REXLOG_INFO("sc5 net: Online Practice match {} over at frame {} (BACK)", g_match, g_frame);
    FeedEnd();
    StopRolling();
    input::SetSource(nullptr);
    g_sparring_over = true;
    g_status = Status::Connecting;
    g_opened_at = Clock::now();
    g_peer.last_heard = Clock::now();
    return;
  }
  g_neutral = false;
  // the game ahead of the other's real inputs runs on them as they come
  if (g_rolling) {
    SendOwn();
    if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
    // the round is no longer fought: lockstep again once every frame so far ran with the real inputs
    if (!RoundFought()) {
      if (g_frame && !WaitForInputs(g_frame - 1)) return Lost(Status::Disconnected);
      if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
      if (!RoundFought()) {
        StopRolling();
        ReseedAtStateChange(ctx, base);
      }
    }
    if (g_rolling) {
      if (g_frame >= g_peer.confirmed + kMaxPredict && !WaitForInputs(g_frame - kMaxPredict))
        return Lost(Status::Disconnected);
      if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
      Pace();
      // a frame that runs on the other's real input (it came in time: the input delay covered the trip) is never gone
      // back to: kept only for its check
      if (KeepAll() || g_peer.contiguous <= g_frame || g_frame % kCheckEvery == 0) {
        const auto k0 = Clock::now();
        Keep(ctx, base, g_frame);
        g_cost.top_keep_ms += Ms(Clock::now() - k0);
        g_cost.top_keeps++;
      } else {
        g_ring_frame[g_frame % kRing] = -1;
        g_cost.top_skipped++;
      }
    }
  } else {
    SendOwn();
    ReseedAtStateChange(ctx, base);  // before the round's first frame is kept
    if (g_cfg.rollback && RoundFought()) {
      StartRolling(ctx, base);
      Keep(ctx, base, g_frame);
    } else {
      // Outside a round's fighting (the intro, the round's call, a KO and its replay, between rounds) the players'
      // inputs do nothing the fight keeps: both games run these frames on neutral inputs, without waiting for the
      // other's (with a round trip longer than the input delay, lockstep made the intro wait at almost every frame)
      g_peer.confirmed = g_frame + 1;
      g_neutral = true;
      ReplaySkipAsk();  // a player's START or A in a KO's replay cuts it short for both games
      if (!ReplaySkipWait()) return Lost(Status::Disconnected);
      Pace();  // the two games kept even here too, or they start the round apart
    }
  }
  ReplaySkipTop();
  SetFrameInputs(g_frame);
  sparring::OnFrame(g_frame);  // Online Practice's rules at the frame's top, after it was kept (a rerun starts there)
  SettleChecks();
  CompareChecks();
  DumpAtTop(ctx, base, g_frame);
  DumpSettled(g_rolling ? g_peer.confirmed : g_frame + 1);
}

// tests: SC5_NET_DUMP_AT=<frame>,...: the battle's snapshot at the top of those match frames (their inputs set) as the
// two games end up with it, into SC5_NET_DUMP_DIR as net<side>_m<match>at<frame>.bin / .blocks (each block's place in
// the .bin, size and guest address), and the raw memory kept beside it (battle.cpp) as .raw.bin / .raw.blocks. Each
// run of such a frame (the first, and every rerun) keeps its top; it is written once the frame's own input and every
// one before it are known, so the last run had them all and a wrong guess shows in neither game's files.
struct AtDumps {
  std::vector<uint32_t> at;
  std::map<uint32_t, uint32_t> pending;  // frame -> the stream holding its top from its last run
  std::vector<uint32_t> spare;
};

AtDumps& Dumps() {
  static AtDumps d = [] {
    AtDumps out;
    if (const char* v = std::getenv("SC5_NET_DUMP_AT"))
      for (const char* p = v; *p;) {
        out.at.push_back(uint32_t(std::strtoul(p, const_cast<char**>(&p), 10)));
        while (*p == ',') p++;
      }
    return out;
  }();
  return d;
}

void DumpAtTop(PPCContext& ctx, uint8_t* base, uint32_t f) {
  AtDumps& d = Dumps();
  const char* dir = std::getenv("SC5_NET_DUMP_DIR");
  if (d.at.empty() || !dir || std::find(d.at.begin(), d.at.end(), f) == d.at.end()) return;
  uint32_t& stream = d.pending[f];
  if (!stream) {
    if (d.spare.empty()) {
      stream = battle::NewStream();
    } else {
      stream = d.spare.back();
      d.spare.pop_back();
    }
  }
  if (!battle::Save(ctx, base, stream)) {
    d.spare.push_back(stream);
    d.pending.erase(f);
  }
}

// the frames below `settled` ran with every real input: their kept tops are final
void DumpSettled(uint32_t settled) {
  AtDumps& d = Dumps();
  const char* dir = std::getenv("SC5_NET_DUMP_DIR");
  while (!d.pending.empty() && d.pending.begin()->first < settled) {
    const auto [f, stream] = *d.pending.begin();
    const std::string stem = fmt::format("{}/net{}_m{}at{}", dir, g_slot + 1, g_match, f);
    auto write = [](const std::string& path, const uint8_t* data, size_t size, const std::vector<battle::Block>& blocks) {
      if (FILE* out = std::fopen((path + ".bin").c_str(), "wb")) {
        std::fwrite(data, 1, size, out);
        std::fclose(out);
      }
      if (FILE* out = std::fopen((path + ".blocks").c_str(), "w")) {
        for (const auto& b : blocks) std::fprintf(out, "%u %u %08X\n", b.pos, b.size, b.source);
        std::fclose(out);
      }
    };
    if (const auto* blocks = battle::Blocks(stream)) write(stem, battle::Data(stream), battle::Size(stream), *blocks);
    // the raw memory kept beside it (battle.cpp kRaw and DynamicRegions), which the check does not hash
    std::vector<battle::Block> raw_blocks;
    const std::vector<uint8_t> raw = battle::RawBytes(stream, 0, raw_blocks);
    write(stem + ".raw", raw.data(), raw.size(), raw_blocks);
    d.spare.push_back(stream);
    d.pending.erase(d.pending.begin());
  }
}

void OnEnd(PPCContext&, uint8_t*) {
  if (g_status != Status::InSync) return;
  LogFrame(false);
  g_stats.frames++;
  // what both hold and no longer need
  const uint32_t keep = g_frame > 120 ? g_frame - 120 : 0;
  g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(keep, g_peer.ack)));
  g_peer.inputs.erase(g_peer.inputs.begin(), g_peer.inputs.lower_bound(keep));
  g_peer.used.erase(g_peer.used.begin(), g_peer.used.lower_bound(keep));
  g_frame++;
  if (g_stats.frames % 600 == 0) {
    REXLOG_INFO("sc5 net: frame {}: {} rollbacks ({} frames run again, the longest {} frames and {:.1f} ms, {:.2f} ms "
                "on average), {} waits for the other, {} frames held back",
                g_frame, g_stats.rollbacks, g_stats.rerun, g_stats.max_rollback, g_stats.longest_rollback_ms,
                g_stats.rollbacks ? g_stats.rollback_ms / g_stats.rollbacks : 0.0, g_stats.waits, g_stats.holds);
    if (g_cost.top_keeps || g_cost.rollbacks)
      REXLOG_INFO("sc5 net: keeping a frame top {:.2f} ms ({} kept, {} needed none); a rollback: {:.2f} ms putting the "
                  "battle back, {:.2f} ms keeping a frame run again ({} kept), {:.2f} ms running one ({} run)",
                  g_cost.top_keeps ? g_cost.top_keep_ms / g_cost.top_keeps : 0.0, g_cost.top_keeps, g_cost.top_skipped,
                  g_cost.rollbacks ? g_cost.restore_ms / g_cost.rollbacks : 0.0,
                  g_cost.keeps ? g_cost.keep_ms / g_cost.keeps : 0.0, g_cost.keeps,
                  g_cost.runs ? g_cost.run_ms / g_cost.runs : 0.0, g_cost.runs);
  }
}

}  // namespace

const char* StatusName(Status s) {
  switch (s) {
    case Status::Off: return "off";
    case Status::Connecting: return "connecting";
    case Status::Barrier: return "at the barrier";
    case Status::InSync: return "in sync";
    case Status::Desynced: return "desynced";
    case Status::Disconnected: return "disconnected";
  }
  return "?";
}

void InitFromEnvironment() {
  const char* bind = std::getenv("SC5_NET_BIND");
  const char* peer = std::getenv("SC5_NET_PEER");
  if (!bind || !peer) return;
  Settings st;
  st.bind = uint16_t(std::atoi(bind));
  st.peer = peer;
  if (const char* v = std::getenv("SC5_NET_SIDE")) st.side = std::atoi(v) == 2 ? 2 : 1;
  if (const char* v = std::getenv("SC5_NET_DELAY")) st.delay = std::atoi(v);
  if (const char* v = std::getenv("SC5_NET_LOCKSTEP")) st.rollback = v[0] != '1';
  if (const char* v = std::getenv("SC5_NET_LOG")) st.log = v;
  if (const char* v = std::getenv("SC5_NET_LAG")) st.lag_ms = std::atoi(v);
  if (const char* v = std::getenv("SC5_NET_JITTER")) st.jitter_ms = std::atoi(v);
  if (const char* v = std::getenv("SC5_NET_LOSS")) st.loss_percent = std::atoi(v);
  frame::SetTopHook(&OnTop);
  frame::SetEndHook(&OnEnd);
  Configure(st);
}

void Configure(const Settings& st) {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  g_pending = st;
  g_pending_set = true;
  g_active = true;
}

void Disable() {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  g_pending_off = true;
}

void Install() {
  frame::SetTopHook(&OnTop);
  frame::SetEndHook(&OnEnd);
}

void EndMatch() {
  if (g_status != Status::InSync) return;
  FeedEnd();
  StopRolling();
  input::SetSource(nullptr);
  g_opened_at = Clock::now();
  g_peer.last_heard = Clock::now();
  g_status = Status::Connecting;
  REXLOG_INFO("sc5 net: match {} over at frame {}; the games go on on their own until the next", g_match, g_frame);
}

bool InSync() { return g_status.load() == Status::InSync; }
bool PeerHeard() { return g_peer.heard_coming || g_peer.seen; }
bool SparringOver() { return g_sparring_over.load(); }

void SetLocalPick(int side, const Pick& pick, uint32_t pickable) {
  if (side < 0 || side > 1) return;
  Pick& p = g_local_picks[side];
  if (p.phase != pick.phase || p.slot != pick.slot || p.costume != pick.costume || g_local_pickable != pickable) {
    p = pick;
    g_local_pickable = pickable;
    g_picks_changed = true;
  }
}

Pick RemotePick(int side) {
  if (side != 0 && side != 1) return Pick{};
  if (g_cfg.watch) {  // a spectator: each player's from the feed
    auto it = g_watched.find(uint16_t(g_match + 1));
    return it != g_watched.end() ? it->second.picks[side] : Pick{};
  }
  return g_peer.picks[side];
}
uint32_t RemotePickable() { return g_peer.pickable; }
void SetLocalDlc(uint32_t fingerprint) {
  if (g_local_dlc != fingerprint) {
    g_local_dlc = fingerprint;
    g_picks_changed = true;
  }
}
uint32_t RemoteDlc() { return g_peer.dlc; }

void SetLocalStage(const StagePick& s) {
  StagePick& l = g_local_stage;
  if (l.chooser != s.chooser || l.phase != s.phase || l.cursor != s.cursor || l.stage != s.stage) {
    l = s;
    g_picks_changed = true;
  }
}

void SetLocalChoice(const ResultChoice& c) {
  ResultChoice& l = g_local_choice;
  if (l.chooser != c.chooser || l.phase != c.phase || l.cursor != c.cursor) {
    l = c;
    g_picks_changed = true;
  }
}

ResultChoice RemoteChoice() { return g_peer.choice; }

void SetRematchAt(uint32_t frames) {
  if (g_local_rematch_at == int64_t(frames)) return;
  g_local_rematch_at = int64_t(frames);
  g_picks_changed = true;
}

int64_t RemoteRematchAt() { return g_peer.rematch_at; }

void SetLocalSkip() {
  if (g_local_skip_in) return;
  g_local_skip_in = true;
  g_picks_changed = true;
  REXLOG_INFO("sc5 net: this player skips the way into match {}", g_match + 1);
}

bool SkipWanted() {
  if (g_cfg.watch) {  // a spectator: a player's, from the feed
    auto it = g_watched.find(uint16_t(g_match + 1));
    return it != g_watched.end() && it->second.skip_in;
  }
  return g_local_skip_in || g_peer.skip_in;
}

void SetLocalCreated(int side, const std::vector<uint8_t>& record) {
  if (side < 0 || side > 1) return;
  if (record.size() != size_t(kCreatedSize)) {
    g_local_created[side].clear();
    return;
  }
  if (g_local_created[side] == record) return;
  g_local_created[side] = record;
  g_created_acked[side] = 0;
  g_created_fed[side] = false;
  g_picks_changed = true;
}

int RemoteCreated(int side, std::vector<uint8_t>& out, std::string& why) {
  if (side < 0 || side > 1) return 0;
  const Incoming* in = &g_incoming[side];
  if (g_cfg.watch) {  // a spectator: from the feed
    auto it = g_watched.find(uint16_t(g_match + 1));
    if (it == g_watched.end()) return 0;
    in = &it->second.created[side];
  }
  if (!in->refused.empty()) {
    why = in->refused;
    return -1;
  }
  if (!in->Whole()) return 0;
  out = in->Joined();
  return 1;
}

StagePick RemoteStage() {
  if (g_cfg.watch) {  // a spectator: the chooser's, from the feed
    auto it = g_watched.find(uint16_t(g_match + 1));
    return it != g_watched.end() ? it->second.stage : StagePick{};
  }
  return g_peer.stage;
}

void SendSetup(const Setup& s) {
  g_my_setup = s;
  g_my_setup_set = true;
  g_last_setup_send = {};
  SendComing();
  // the spectators: player 1's setup of the coming match
  Writer f = FeedHeader(kFeedMeet, uint16_t(g_match + 1));
  f.U8(kMeetSetup);
  WriteSetup(f);
  Feed(f);
  FeedPump();
}

bool WaitSetup(Setup& out, int ms) {
  if (g_cfg.watch) {
    // a spectator: player 1's setup through the feed (player 1 may still be on its way there: two minutes)
    const auto until = Clock::now() + std::chrono::seconds(120);
    const uint16_t coming = uint16_t(g_match + 1);
    while (!g_watched[coming].has_setup) {
      if (g_status != Status::Connecting || Clock::now() > until) return false;
      FeedPump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    out = g_watched[coming].setup;
    return true;
  }
  const auto until = Clock::now() + std::chrono::milliseconds(ms);
  while (!g_peer.has_setup) {
    if (g_status != Status::Connecting || Clock::now() > until) return false;
    Receive();
    SendComing();
    if (Clock::now() - g_last_hello > std::chrono::milliseconds(250)) {
      Send(Header(kHello));
      g_last_hello = Clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  out = g_peer.setup;
  return true;
}

Status GetStatus() { return g_status.load(); }
bool Active() { return g_active.load(); }
uint32_t Frame() { return g_frame; }
Stats GetStats() { return g_stats; }

std::string MatchStatsJson(const std::string& match, const char* end) {
  std::string id;
  for (char c : match) id += std::isalnum(uint8_t(c)) || c == '-' || c == '_' ? c : '_';
  const double secs = g_match_start == Clock::time_point{} ? 0.0 : Ms(Clock::now() - g_match_start) / 1000.0;
  const Stats& s = g_stats;
  return fmt::format(
      "{{\"v\":1,\"match\":\"{}\",\"end\":\"{}\",\"role\":\"{}\",\"netcode\":\"{}\",\"delay\":{},\"secs\":{:.1f},"
      "\"frames\":{},\"rollbacks\":{},\"rerun\":{},\"max_rollback\":{},\"rollback_ms_avg\":{:.2f},"
      "\"rollback_longest_ms\":{:.1f},\"waits\":{},\"held\":{}}}",
      id, end, g_cfg.watch ? "spectator" : g_slot == 0 ? "player1" : "player2",
      g_cfg.rollback ? "rollback" : "lockstep", Delay(), secs, s.frames, s.rollbacks, s.rerun, s.max_rollback,
      s.rollbacks ? s.rollback_ms / s.rollbacks : 0.0, s.longest_rollback_ms, s.waits, s.holds);
}

}  // namespace sc5::net
