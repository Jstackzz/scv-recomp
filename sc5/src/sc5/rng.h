#pragma once
// The game's random numbers (docs\GAME.md, "Rollback"). Four generators, all seeded by sub_824A35A8: the C library's
// rand (state per guest thread at _getptd()+0x14), Park-Miller (sub_824A33D8 / sub_824A3248, 0x829249F8), TT800
// (sub_824A3030, 0x82924A20) and a Mersenne Twister ([0x82773FB0]). The fight and the visual systems draw from the same
// ones. The visual systems keep their state in heap objects that come and go (effects, the stage's wind and gusts),
// which rollback cannot put back, so a frame run again can draw a different number of times there, and every number
// the fight draws after that is another. Their updates therefore draw from a set of states of their own: swapped in
// around sub_82195D48 (effects), sub_821936C8 (the forces on cloth and hair), sub_82194210 (gusts) and two tasks
// (sub_8219D508, sub_824A5ED0), and never put back by rollback; the fight's states are what rollback keeps.
#include <cstdint>

struct PPCContext;

namespace sc5::rng {

// the main guest thread's C library rand state (_getptd() + 0x14), found on the first call (main thread)
uint32_t MainRandState(PPCContext& ctx, uint8_t* base);

// around a visual system's update (main thread; nests): its random numbers from the visual states
void EnterVisual(PPCContext& ctx, uint8_t* base);
void LeaveVisual();
// a visual system's update is running now (main thread)
bool InVisual();
// the visual states start again from the fight's at the next visual update (netplay's barrier: the two games' visual
// streams went apart before a match, and the intro draws from them)
void ResetVisual();

}  // namespace sc5::rng
