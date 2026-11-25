#pragma once
// What the game reads from its controllers. SC5 polls XamInputGetState for user indices 0-3 every frame
// (sub_824A2F58, at the top of the frame) and reads key presses with XamInputGetKeystrokeEx; this layer answers
// instead of the SDK: from the PC's own controllers, or from a source a test script or netplay installs. Ported
// from MK9 Online (mk9/input.h).
#include <cstdint>

namespace sc5::input {

// one Xbox 360 controller as the game sees it
struct Pad {
  bool connected = false;
  uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  // the XInput packet number when a source decides it (netplay: it must come out the same when a frame runs
  // again); 0: this layer counts changes itself
  uint32_t packet = 0;
};

constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// The PC's own controller for a user index as the SDK reads it. Any thread.
Pad ReadPhysical(uint32_t user);

// A source answers for user indices it owns (returns true) and leaves the rest to the PC's own controllers.
using Source = bool (*)(uint32_t user, Pad& out);
void SetSource(Source source);
// asked first, before the source (the launcher's menu autopilot)
void SetOverride(Source override_source);

// Rollback: the key events' state (XamInputGetKeystrokeEx reports changes since the last event) kept at the top of a
// frame, and put back when a rollback goes back to it (main thread).
void KeepKeys(uint64_t frame);
void KeysBack(uint64_t frame);
uint64_t KeystrokeReads();

// how often the game polled each user this run, and what its last XamInputGetState for a user got (diagnostics)
uint64_t Polls(uint32_t user);
Pad LastRead(uint32_t user);

}  // namespace sc5::input
