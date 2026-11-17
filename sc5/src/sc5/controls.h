#pragma once
// The launcher's controls file (docs\PROTOCOL.md 3.1, SC5_CONTROLS; ported from MK9 Online's mk9/controls.h): which keys
// press each Xbox 360 button (the keyboard plays as player 1 while the game's window is in front), what each button
// of a PC controller presses, and which controller is player 2's. Read at start and again within a second whenever
// the file changes, so the launcher's Controls page applies at once.
//
//   keyboard = true
//   [keys]                       # Windows virtual key names, at most two per button
//   up = ["Up", "W"]
//   a = ["K", "Space"]
//   [buttons]                    # a PC controller's button (where it sits) -> the button it presses ("" nothing)
//   south = "a"
//   right_shoulder = "rb"
//   [ports]
//   player2 = ""                 # player 2's controller (the offline versus) by its name; "" the second plugged in
//
// Xbox 360 buttons: up down left right a b x y lb rb lt rt start back ls rs. PC controller buttons: south east west
// north left_shoulder right_shoulder left_trigger right_trigger dpad_up dpad_down dpad_left dpad_right start back
// left_stick right_stick misc paddle1-4 touchpad (the last six through SDL, which the SDK's state leaves out).
#include <cstdint>
#include <string>

namespace sc5::input {
struct Pad;
}

namespace sc5::controls {

// the controllers' assignment to the game's users ([ports]) in place of the SDK's (once the input system is up)
void Install();
// a pad of user `user` as the file says: its buttons remapped, and for user 0 the keyboard's added
void Apply(uint32_t user, sc5::input::Pad& pad);
std::string FilePath();

}  // namespace sc5::controls
