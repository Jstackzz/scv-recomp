#pragma once
// The single-PC rollback check (as MK9 Online's net/synctest): with SC5_SYNCTEST=<n>, every frame of a fight the
// battle is saved (battle.h), put back to n frames earlier, and those frames run again with the same inputs
// (frame::RunAgain); the state they reach must be the one saved, byte for byte. SC5_SYNCTEST_EVERY=<k> checks only
// every k-th frame (default 1). What differs is logged with its offset in the snapshot.
namespace sc5::synctest {

// reads the environment and, when asked, takes the frame's top hook (once, at startup)
void Init();

}  // namespace sc5::synctest
