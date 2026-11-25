#pragma once
// The game's own battle state snapshots (docs\GAME.md, "The battle"), taken and put back by us: what the game saves
// about every 80 frames for its KO replay (sub_8215A298) and restores from (sub_8215A808), into streams of our own in
// guest memory. The candidate for rollback's saved states (net\synctest.cpp checks it).
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace sc5::battle {

// A stream as the game's are (vtable 0x820BA908: +4 written, +8 read, data from +12, 0x28000 bytes at most), allocated
// in guest memory; 0 when none could be.
uint32_t NewStream();

// The battle's state into a stream (from its start), on the main thread between frames. Returns the bytes written.
uint32_t Save(PPCContext& ctx, uint8_t* base, uint32_t stream);
// The battle's state back from a stream; false when the game refused it (it puts back only a snapshot of the same
// battle and round) or read less than was written.
bool Restore(PPCContext& ctx, uint8_t* base, uint32_t stream);

// a stream's written bytes, and their address on the host
uint32_t Size(uint32_t stream);
const uint8_t* Data(uint32_t stream);
// where the raw memory kept beside two snapshots differs: the first guest addresses, "" if nowhere
std::string RawDifferences(uint32_t a, uint32_t b, uint32_t& count);

// The blocks the game's last save into a stream wrote, in order, with the guest address each came from (Save records
// them through the stream's write, sc5_codegen.toml's hook at 0x8211CB84)
struct Block {
  uint32_t pos, size, source;
};
// tests: the raw memory kept beside a stream, its blocks (their places counted from `offset`) added to `blocks`
// (net/netplay.cpp DumpAt writes it beside the snapshot)
std::vector<uint8_t> RawBytes(uint32_t stream, uint32_t offset, std::vector<Block>& blocks);
const std::vector<Block>* Blocks(uint32_t stream);
// what a block's guest address holds: a player's pose (each player's three buffers of 6208 bytes, 0x8000 apart from
// about +0x3160: cloth and secondary bones that the stage's wind moves, which rollback cannot keep exact), a heap object
// (effects such as the weapon's trail), the stack (values the save works out), or the fight (the players' objects
// but their poses, the battle's state machine, the rest of the image)
// A pointer is one of the 12 words in each player's object that the snapshot carries raw: pointers into what the game
// loaded, the same in every run of a frame on one PC but not from one PC (or one process) to another (found by shifting
// one game's heap: SC5_TEST_HEAP_SHIFT).
enum class Kind { Pose, Heap, Stack, Pointer, Fight };
// one of the three buffers of 0x800 bytes a player's pose fills for the drawing (tests write into them)
uint32_t DrawnBuffer(int player, uint32_t k);
Kind KindOf(uint32_t source);
// A hash of the fight in the last save into a stream: the blocks of the fight (Kind::Fight) and nothing that differs
// from one PC to the other or one run of a frame to its rerun. What two players' games compare (netplay's checks)
uint64_t FightHash(uint32_t stream);
// the stream write hook's report (debug_watch.cpp's Sc5DbgSnapWrite)
void OnSnapWrite(uint32_t stream, uint32_t pos, uint32_t size, uint32_t source);
// A KO's replay is playing (the replay's manager, sub_8211A9E8, in its state 2): the battle state machine says
// "fighting", but the game plays a stretch of the round back and puts the present back at its end (battle.cpp)
bool InKoReplay();

}  // namespace sc5::battle
