#pragma once
// Frame data in practice: a frame meter in the style of Street Fighter 6's over Training (offline) and Online Practice
// (docs\GAME.md, "Frame data in practice"). Per player a row of one-frame cells (startup green, impact red, recovery
// blue, hit or guard stun yellow, moving without an attack gray, free to act dark), each run's length at its end, a line
// per player (startup, impact frames, recovery, total, advantage) and a damage panel (the last hit's damage, the combo's,
// the attack's level). Shown while the launcher's "Frame data in practice" is on (SC5_FRAMEDATA, display.txt's
// frame_data; on in a game started by hand) and F7 in the game has not hidden it.
//
// It only reads the fighters' memory, from a hook after the game's step (sub_824A25C0: every run of a frame, a
// rollback's reruns too), and never calls the game's code otherwise: it cannot change what the game computes, so two
// games of an online match stay alike whatever either shows. Each frame's reading is kept under the fight's own frame
// count, which a rollback sets back and a pause stops: a frame run again replaces its first run, so what is shown is
// the frames' last runs, and a paused game adds nothing.
#include <cstdint>

struct ImFont;
struct ImFontAtlas;
namespace rex::ui {
class ImGuiDrawer;
}

namespace sc5::framemeter {

// the launcher's setting, at the start and whenever display.txt changes (any thread); F7 in the game flips it
void SetEnabled(bool on);

// Sc5App::OnConfigureFonts, before the SDK builds its font atlas: the meter's font (Segoe UI Bold, large)
void AddFonts(ImFontAtlas* atlas);
// that font, for the game's other overlays too (result_overlay.cpp); the runtime's own when it has none. UI thread.
ImFont* LargeFont();

// Sc5App::OnCreateDialogs: the overlay
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace sc5::framemeter
