#pragma once
// A player's preferred side (mirror.cpp, docs\GAME.md "A player's preferred side"), as Tekken 8's: a player who always
// wants to start on the left (or on the right) sees the fight that way on their own screen, whichever player they are.
// This game shows the fight's 3D scene mirrored left to right (the GPU flips the draw that puts the scene's picture on
// the screen, so the HUD and all text drawn over it stay as they are), swaps the left and right of the player's own
// controller and keyboard in the fight before the game and the netplay take them, and swaps the sound's left and
// right. Nothing the fight reads changes: the other game, the spectators, rollback and every check see the same match.
#include <cstdint>

namespace sc5::input {
struct Pad;
}

namespace sc5::mirror {

enum class Side { kOff, kLeft, kRight };

// The launcher's setting (SC5_PREFERRED_SIDE, or preferred_side in its display file): off, left, right. Any thread.
void SetPreferred(Side side);
Side Preferred();
Side SideFromText(const char* text);
const char* SideName(Side side);

// Whether this game shows the fight mirrored now (the frame being drawn). Any thread.
bool Mirrored();

// The PC's own controller for a user index (input::ReadPhysical): its left and right swapped while this game shows the
// fight mirrored and that user is the player it is mirrored for, and the fight runs (not paused in a menu). Scripted
// test pads never come through here. Any thread.
void OwnPad(uint32_t user, input::Pad& pad);

// Main thread, before each frame's drawing (frame.cpp): works out whether this frame is mirrored, and tells the GPU
// where in its commands that starts or ends.
void BeforeDraw();

// The game's sound (display.cpp's hook of the frames the game hands the system, 6 channels of 256 big-endian floats
// one after the other): front and surround left and right swapped while the fight is mirrored.
void SwapSound(uint32_t* samples);

}  // namespace sc5::mirror
