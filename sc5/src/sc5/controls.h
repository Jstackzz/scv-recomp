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
//   [fight]                      # what a button presses in a fight, of the game's own four buttons ("" nothing)
//   lb = "A+B+K+G"
//
// Xbox 360 buttons: up down left right a b x y lb rb lt rt start back ls rs. PC controller buttons: south east west
// north left_shoulder right_shoulder left_trigger right_trigger dpad_up dpad_down dpad_left dpad_right start back
// left_stick right_stick misc paddle1-4 touchpad (the last six through SDL, which the SDK's state leaves out).
//
// [fight]: the player's own button settings for fights, in place of the game's Control Settings (which an online
// game's fresh save keeps at Type A, and which offer only 13 of the 16 combinations). For each of a b x y lb rb lt rt
// ls rs it names the game's buttons the button presses: any of A (horizontal attack), B (vertical), K (kick) and G
// (guard) joined by +. A button not listed keeps the game's own setting. Online the player's pad turns into Type A's
// face buttons before the netplay sends it (ToDefaultLayout), so both games and the spectators see the combination
// the player meant whatever each one set; offline the battle's conversion of player 1's pad takes it (controls.cpp's
// hook of sub_82171AE8).
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

// The game's own buttons in a fight, as its battle input carries them (sub_82171AE8)
constexpr uint8_t kGameA = 1, kGameB = 2, kGameK = 4, kGameG = 8;
// The player's own pad in the game's default layout (Type A: X horizontal attack, Y vertical, B kick, A guard), for the
// netplay: each button [fight] sets presses the face buttons that make its combination there and nothing else. The
// game's Type A then turns them into the combination on both PCs. Buttons [fight] leaves alone stay as they are.
void ToDefaultLayout(sc5::input::Pad& pad);
// [fight] as one line for the log ("lb=A+B+K+G lt=K+G"; "" when it sets nothing)
std::string FightText();

}  // namespace sc5::controls
