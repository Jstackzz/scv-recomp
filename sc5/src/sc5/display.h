#pragma once
// The launcher's display and sound settings (docs\PROTOCOL.md 3.1): SC5_DISPLAY, SC5_RESOLUTION, SC5_RENDER_SCALE,
// SC5_ANTI_ALIASING, SC5_ANISOTROPIC, SC5_VSYNC, SC5_ADAPTIVE_SYNC, SC5_BRIGHTNESS, SC5_FRAMEDATA, SC5_VOLUME_* at
// start, and SC5_DISPLAY_FILE (the same as key=value lines) read again whenever it changes. Each only changes how a
// frame looks or sounds on this PC, never what the game computes: two players may set them differently in one match.
//
// What applies at once: v-sync, texture filtering, brightness (a curve over the game's own gamma ramp), the master
// volume (the frames the game's audio hands the system, scaled), the music, effects and voices levels (the game's
// sound categories, sound.h), the frame meter in practice (SC5_FRAMEDATA, framemeter.h). The window's mode and size:
// the next time the game's window is in front. The internal resolution, anti-aliasing and adaptive sync: at the next
// start.
#include <cstdint>
#include <functional>

namespace sc5::display {

// before the runtime and the window exist (Sc5App::OnConfigurePaths): the start's settings as the SDK's options,
// unless the command line set them
void ConfigureFromEnvironment();
// once the game runs (OnPostSetup): watch SC5_DISPLAY_FILE. `window` applies a mode and size to the game's window on
// the UI thread (fullscreen; width and height 0: the screen's), and says whether the game's window is in front
struct WindowHooks {
  std::function<void(bool fullscreen, uint32_t width, uint32_t height)> apply;
  std::function<bool()> in_front;
};
void Start(WindowHooks hooks);

// the master volume as a factor (1: the game's own level)
float MasterVolume();

}  // namespace sc5::display
