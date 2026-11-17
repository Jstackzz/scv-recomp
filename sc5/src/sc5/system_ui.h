#pragma once
// The system UI the game asks for. On a console the player answers the system's message boxes; here the ones the
// game raises in the normal course of things are answered for the player (the launcher's menu autopilot and online
// matches cannot wait on a dialog), and the rest are shown as usual.
#include <cstdint>
#include <optional>

namespace sc5::system_ui {

// installs the answers (once, at startup)
void Init();

}  // namespace sc5::system_ui
