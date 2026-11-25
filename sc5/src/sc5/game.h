#pragma once
// What the game is doing, for netplay, the launcher's autopilot and tests (docs\GAME.md: sequences, the battle).
#include <cstdint>

struct PPCContext;

namespace sc5::game {

// The game's sequences (its screens), by id: the last one created (sc5_codegen.toml hooks each creator).
enum Sequence : int {
  kSeqNone = 0,
  kSeqSetup = 1,
  kSeqAttract = 2,
  kSeqTitle = 3,
  kSeqAstralHome = 4,  // the main menu (also 18)
  kSeqEntry = 6,
  kSeqGame = 7,
  kSeqSignOut = 8,
  kSeqVersus = 9,  // VS Battle: the select screens and the fight (VSSpecial is 10)
  kSeqArcade = 11,
  kSeqCreation = 12,
  kSeqOption = 13,
  kSeqTraining = 14,
  kSeqLogo = 15,
  kSeqSample = 17,
  kSeqWorldMap = 19,  // Story
  kSeqOnline = 20,
  kSeqQuickBattle = 22,
  kSeqLegendarySouls = 23,
  kSeqInvite = 25,
};
int SequenceId();
const char* SequenceName(int id);

// The battle's state machine (0x8277B808): which of its embedded states is current, as its offset in the struct
// (0 when none). The ones a VS fight goes through:
constexpr uint32_t kBattleIdle = 0x18;        // menus
constexpr uint32_t kBattleLoading = 0xD0;
constexpr uint32_t kBattleIntro = 0xF0;
constexpr uint32_t kBattleRoundCall = 0x24;   // "BATTLE 1 - FIGHT!"
constexpr uint32_t kBattleFighting = 0x40;
constexpr uint32_t kBattleKO = 0x58;
constexpr uint32_t kBattleBetweenRounds = 0x128;
uint32_t BattleState();
const char* BattleStateName(uint32_t state);

// Frames since round 1's call began in the present fight (the first frame in kBattleRoundCall after the fight
// loaded is 0), or -1 outside a fight: what tests time their inputs by (the frames before depend on loading).
int64_t FightFrame();

// The seed every seeding of the game's random numbers uses from now on (sub_824B2EA0), or -1 for its own (the date
// and time at boot, its online code's at a battle). SC5_TEST_SEED=<n> sets it from the start.
void SetSeed(int64_t seed);
// the seed the fight's random numbers took last (the override when there is one), and the game's seeding run again
// with `seed` (main thread, between frames: netplay's barrier)
int64_t FightSeed();
void Reseed(PPCContext& ctx, uint8_t* base, uint32_t seed);
// the seedings from now on are not logged (netplay seeds every frame of a match: 60 lines a second otherwise)
void QuietSeeds(bool quiet);

// the main menu's (AstralHome's) object, made by its creator each time the menu comes; 0 before
uint32_t HomeObject();

// Called from the frame hook at the end of each frame (frame.cpp).
void OnFrameEnd(uint64_t frame);

// Guest memory, big-endian, for the game's state (any thread; addresses in the guest's virtual heap or image).
uint32_t ReadU32(uint32_t address);

}  // namespace sc5::game
