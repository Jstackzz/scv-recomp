#pragma once
// The game's side of the launcher protocol (docs\PROTOCOL.md 3): with SC5_NET=1 the launcher owns this process;
// commands come in cmd.txt, the game's state goes out in status.txt. See link.cpp.

namespace sc5::link {

// at startup (after the netplay's environment): SC5_NET=1 with SC5_CMD and SC5_STATUS
void Init();
bool On();

}  // namespace sc5::link
