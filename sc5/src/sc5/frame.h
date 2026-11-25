#pragma once
// The game's frame boundary. main (sub_82189A00) sets the game up (sub_82163900) and then runs
// `while (sub_82164070()) {}` on the main guest thread: sub_82164070 is one frame. It reads the pads
// (sub_824A2F58), runs the game's sequences (sub_824A3780) and tasks (sub_824A4030), draws (sub_824AAC38,
// sub_824AD398) and presents (sub_824A3988). Its call from main (0x82189A3C) is the top of a frame: nothing of
// the game's but main's own frame is on the guest stack there, and the pads are not read yet.
#include <cstdint>

struct PPCContext;

namespace sc5::frame {

// frames run so far on the main thread
uint64_t Count();
// the host thread id of the game's main guest thread, 0 before the first frame
uint32_t MainThreadId();
bool OnMainThread();

// hooks other modules register (each at most once): the top of a frame, before it reads the pads, and its end,
// right after it presented
using Hook = void (*)(PPCContext& ctx, uint8_t* base);
void SetTopHook(Hook hook);
void SetEndHook(Hook hook);
// the launcher's link and autopilot: at the top of each frame, before the top hook (never in a frame run again)
void SetMenuHook(Hook hook);
// a call at the top of the next frame on the main thread (guest code may run there): from any thread, once however
// often it is posted before
void Post(Hook hook);

// Rollback: from the top hook, after putting the battle back to an earlier frame (and the count with SetCount), run
// one frame again: the whole frame function, its drawing (phases 0x200 and 0x400), its end (0x500, the present) and
// the models' pose (sub_822C9B38: nothing in the fight reads it, frame.cpp) left out, then the end-of-frame
// bookkeeping (the count, game::OnFrameEnd; not the end hook). Main thread.
void RunAgain(PPCContext& ctx, uint8_t* base);
void SetCount(uint64_t n);
bool Replaying();

}  // namespace sc5::frame
