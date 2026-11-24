#pragma once
// The launcher's autopilot (docs\PROTOCOL.md 3): drives the game's menus from boot to its hold point (the main menu),
// from there to a VS fight with the match's picks, and back. See autopilot.cpp.
#include <cstdint>
#include <string>

struct PPCContext;

namespace sc5::autopilot {

// what the game is taken to: VS Battle (a match; online Online Practice too) or Training (practice)
enum class Mode { Versus, Practice, Sparring };

struct Plan {
  Mode mode = Mode::Versus;
  bool online = false;
  int side = 0;                // this PC's player online: 0 (player 1) or 1 (player 2)
  int slots[2] = {-1, -1};     // the launcher's picks as select screen slots; -1: that side's player picks
  int costumes[2] = {-1, -1};  // the costume to give a launcher's pick (-1: the first)
  // a created character to pick again (its checksum, the result line's c.<checksum>): the player's own game picks it
  // for them (slots: -1), the other game follows that pick as any of the player's and takes the record again
  uint32_t created[2] = {0, 0};
  int wins = 2;                // rounds a player must win
  bool hand_over = false;      // a plain game: the player takes the select screen over (the autopilot stops there)
  bool two_players = false;    // VS Battle's "Player VS Player" (offline versus: the second pad picks player 2)
  // the match's loser chooses the next one on the result screen's menu (a lobby's match: the start line's choose=1):
  // REMATCH or CHARACTER SELECT with their own controller, player 1 after a draw; the other game follows
  bool choose = false;
};

// launcher: the game is the launcher's (SC5_NET=1); else nothing happens
void Init(bool launcher);
// A plain game for practice or the offline versus (SC5_TRAINING, SC5_VERSUS): the autopilot takes the game from boot
// to the plan's select screen and hands it to the players there (it registers its own frame hook)
void InitPlain(const Plan& plan);
// the practice the launcher started runs (Training's fight is up)
bool Practicing();
bool AtHoldPoint();
// from the hold point to a fight
void Begin(const Plan& plan);
// back to the hold point
void Park();
// the next match from the result screen (a new seed is the link's): per side the same fighter again, unless
// `named[side]` (the plan's slot then: a fighter, or -1 for that side's player to pick again); `same_stage`: the last
// match's stage. The same two fighters on the same stage: VS Battle's own Rematch (as on the console); else through
// Character Select and the stage select.
void Rematch(const Plan& plan, const bool named[2], bool same_stage);
// the VS settings go into the battle setup (versus.cpp, before player 2's game waits for player 1's there): the stage
// select's pick has been taken
void OnSetup();
// still pressing buttons (menus)
bool Busy();
// lost: the game went somewhere the autopilot does not know (the launcher should start it again)
bool Lost();
// why, when the autopilot knows (a created character refused); empty otherwise
std::string LostWhy();

// The match's result, once the fight is over: the winner (1 or 2), the rounds each won, and each side's character as
// the game's id with the costume after a dot ("21.1"). True once per match.
bool MatchOver(int& winner, int& p1_rounds, int& p2_rounds, std::string& p1_char, std::string& p2_char);

// The match's loser's choice on the result screen (Plan::choose): "rematch" or "select" once made (theirs on this PC,
// or followed from the other game's), true from then until the next match
bool ResultChosen(std::string& choice);
// What the overlay over the result screen shows while the match's loser chooses the next match (Plan::choose): the
// chooser (0 player 1, 1 player 2), whether that is this PC's player, where their cursor is (0 REMATCH, 1 CHARACTER
// SELECT) and whether they chose. Any thread.
struct ResultChoiceView {
  bool shown = false;
  int chooser = 0;
  bool mine = false;
  int cursor = 0;
  bool chosen = false;
};
ResultChoiceView ResultChoiceNow();

// what the select screen made for each side in the present match (slot and costume; -1 before)
int PickedSlot(int side);
int PickedCostume(int side);

// main thread, at the top of each frame
void OnFrame(PPCContext& ctx, uint8_t* base);

}  // namespace sc5::autopilot
