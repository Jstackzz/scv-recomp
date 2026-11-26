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
//
// What a button presses in a fight is the game's own Control Settings (Options, as on the console: the types A, B, C
// and the player's own), in every mode. Offline the game converts each pad with its own settings. Online the player's
// own pad turns into Type A's face buttons on their own PC before the netplay sends it (ToDefaultLayout, by their
// settings), and both games convert both players' inputs with Type A (controls.cpp's hook of sub_82172068), so each
// player's buttons do what they set on their own PC, the other game sees exactly the combination they meant, and
// neither game's settings play any part in the other player's inputs. (Until 0.1.7 the launcher's [fight] did this,
// the online games' fresh saves keeping Type A.)
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

// The game's own buttons in a fight, as its battle input carries them (sub_82172068)
constexpr uint8_t kGameA = 1, kGameB = 2, kGameK = 4, kGameG = 8;
// The player's own pad in the game's default layout (Type A: X horizontal attack, Y vertical, B kick, A guard), for the
// netplay: each of A B X Y LB LT RB RT pressed presses the face buttons that make what the player's own Control
// Settings give it (their controller's, pad 0's), and nothing of its own (the triggers 0). The game's Type A then turns
// them into those combinations on both PCs; the game's shortcuts come out the same as their buttons pressed together.
// Main thread (it reads the game's settings).
void ToDefaultLayout(sc5::input::Pad& pad);
// the player's own Control Settings as one line for the log ("Control Settings type B: a=G b=A ...")
std::string FightText();

}  // namespace sc5::controls
