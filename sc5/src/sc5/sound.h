#pragma once
// The game's sound (sound.cpp): a rollback's frames run again reconcile their sounds with the first run's (none starts
// twice, one from a wrong guess is stopped), and the launcher's music, effects and voices levels scale the volumes the
// game sets for its sound categories.
#include <cstdint>

struct PPCContext;

namespace sc5::sound {

// a rollback (main thread): before the first frame runs again (`first_frame` its frame count), and once the frames
// have run again (the sounds of a wrong guess stopped)
void RollbackBegin(uint64_t first_frame);
void RollbackEnd(PPCContext& ctx, uint8_t* base);

// the levels as factors (1: the game's own level; 0 to 1.5); from any thread, in force from the next frame
void SetLevels(float music, float effects, float voices);

}  // namespace sc5::sound
