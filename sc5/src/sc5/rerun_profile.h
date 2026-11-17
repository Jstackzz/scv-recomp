#pragma once
// SC5_TEST_RERUN_PROFILE=1 (tests): the game's tasks timed one by one in frames run again (rerun_profile.cpp).
#include <cstdint>

namespace sc5::rerun_profile {

bool On();
// main thread, before the step of a frame run again: the task tree looked through for classes not timed yet
void Scan();
// every so many frames run again: the tasks that took the most, per frame run again
void Report(uint64_t frames);

}  // namespace sc5::rerun_profile
