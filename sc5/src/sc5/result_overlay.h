#pragma once
// What the game draws over its own picture with the SDK's ImGui drawer (result_overlay.cpp): the choice of a lobby
// match's loser on the result screen (sc5/autopilot.h, ResultChoiceView), large and high-contrast. It only shows what
// the autopilot decided; it reads no input and changes nothing the game computes.

namespace rex::ui {
class ImGuiDrawer;
}

namespace sc5::overlay {

// once the window's ImGui drawer is there (Sc5App::OnCreateDialogs)
void Install(rex::ui::ImGuiDrawer* drawer);

}  // namespace sc5::overlay
