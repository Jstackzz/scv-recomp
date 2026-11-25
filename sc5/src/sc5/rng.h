#pragma once
// The game's random numbers (docs\GAME.md, "Rollback"). Four generators, all seeded by sub_824B2EA0: the C library's
// rand (state per guest thread at _getptd()+0x14), Park-Miller (sub_824B2CD0 / sub_824B2B40, 0x8293857C), TT800
// (sub_824B2928, 0x82938580) and a Mersenne Twister ([0x82787AB0]). The fight and the visual systems draw from the same
// ones. The visual systems keep their state in heap objects that come and go (effects, the stage's wind and gusts),
// which rollback cannot put back, so a frame run again can draw a different number of times there, and every number
// the fight draws after that is another. Their updates therefore draw from a set of states of their own: swapped in
// around sub_8219B548 (effects), sub_82193EB0 (the forces on cloth and hair), sub_821949F8 (gusts) and two tasks
// (sub_8219DD98, sub_824B58E8), and never put back by rollback; the fight's states are what rollback keeps.
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

// tests: SC5_DBG_DRAWS=<file>: every Park-Miller number the fight draws (not the visual systems'), by match frame, as
// the frame's last run drew them: "frame: caller:state ..." (the caller's return address, the generator's state before
// the draw), written once the frame's inputs are all known. Two games' files side by side show the first frame whose
// draws differ and who drew: a system that draws another number of times when a frame runs again. net/netplay.cpp
// says which match frame runs (DrawsFrame at each run's top) and which are final (DrawsSettled).
void DrawsFrame(uint32_t match_frame);
void DrawsSettled(uint32_t below);

}  // namespace sc5::rng
