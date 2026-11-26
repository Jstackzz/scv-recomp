// The overlays over the game's own screens, see result_overlay.h.

#include "sc5/result_overlay.h"

#include <algorithm>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

#include "sc5/autopilot.h"
#include "sc5/framemeter.h"
#include "sc5/net/netplay.h"

namespace sc5::overlay {
namespace {

// SCV Online's colours (the launcher's Style.cpp): navy, gold for titles, white for what is chosen
const ImVec4 kBack(0.03f, 0.06f, 0.14f, 0.92f);
const ImVec4 kGold(0.95f, 0.78f, 0.33f, 1.0f);
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kDim(0.62f, 0.66f, 0.74f, 1.0f);
const ImVec4 kChosen(0.20f, 0.42f, 0.85f, 1.0f);

// The loser of a lobby match chooses the next one on the Results screen: REMATCH or CHARACTER SELECT, the cursor where
// their UP and DOWN put it, on both players' screens
class ResultChoiceDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ResultChoiceDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const autopilot::ResultChoiceView v = autopilot::ResultChoiceNow();
    if (!v.shown || io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
    // text a 22nd of the screen's height (49 px at 1080 lines), in the frame meter's bold font: large enough for a
    // player who sees poorly; the spacing scales with it
    const float px = std::clamp(io.DisplaySize.y / 22.0f, 16.0f, 100.0f), scale = px / 10.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBack);
    ImGui::PushStyleColor(ImGuiCol_Border, kGold);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(2.0f, scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 3.0f * scale));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushFont(framemeter::LargeFont(), px);
    if (ImGui::Begin("##sc5_result_choice", nullptr, flags)) {
      const int player = v.chooser + 1;
      if (v.mine)
        ImGui::TextColored(kGold, v.chosen ? "YOU CHOSE" : "YOU LOST: CHOOSE THE NEXT MATCH");
      else if (v.chosen)
        ImGui::TextColored(kGold, "PLAYER %d CHOSE", player);
      else
        ImGui::TextColored(kGold, "PLAYER %d CHOOSES THE NEXT MATCH", player);
      for (int i = 0; i < 2; i++) {
        const char* label = i ? "CHARACTER SELECT" : "REMATCH";
        if (v.cursor == i)
          ImGui::TextColored(v.chosen ? kChosen : kWhite, "> %s <", label);
        else if (!v.chosen)
          ImGui::TextColored(kDim, "  %s", label);
      }
      if (v.mine && !v.chosen) ImGui::TextColored(kDim, "UP / DOWN, then A");
      if (v.chosen) ImGui::TextColored(kDim, "The next match is coming");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
  }
};

// The select screen online: the player's own created characters this game leaves out, which and why, at the bottom of
// the screen in the same large bold text (autopilot.h, HeldOutView)
class HeldOutDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit HeldOutDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const autopilot::HeldOutView v = autopilot::HeldOutNow();
    if (!v.shown || v.names.empty() || io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
    // a little smaller than the result's choice (a 26th of the height: 42 px at 1080 lines), it sits under the grid
    const float px = std::clamp(io.DisplaySize.y / 26.0f, 16.0f, 90.0f), scale = px / 10.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.98f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.96f, io.DisplaySize.y * 0.6f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBack);
    ImGui::PushStyleColor(ImGuiCol_Border, kGold);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(2.0f, scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 1.5f * scale));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushFont(framemeter::LargeFont(), px);
    if (ImGui::Begin("##sc5_held_out", nullptr, flags)) {
      const size_t n = v.names.size(), shown = n > 4 ? 3 : n;
      ImGui::TextColored(kGold, n == 1 ? "1 OF YOUR CREATED CHARACTERS CANNOT BE PICKED ONLINE"
                                       : "%d OF YOUR CREATED CHARACTERS CANNOT BE PICKED ONLINE", int(n));
      for (size_t i = 0; i < shown; i++) {
        ImGui::TextColored(kWhite, "%s:", v.names[i].c_str());
        ImGui::SameLine();
        ImGui::TextColored(kWhite, "%s", v.whys[i].c_str());
      }
      if (shown < n) ImGui::TextColored(kWhite, "and %d more (the game's log names them)", int(n - shown));
      ImGui::TextColored(kGold, "This is for online matches only");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
  }
};

// The costume panel online, its cursor on one of the player's own created characters that the pick holds back, waits
// on, or shows without its equipment (autopilot.h, HeldOutView: pick_*): which one and why, in the middle of the screen,
// larger than the list below and in plain words, white on navy with the gold border
class PickDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PickDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const autopilot::HeldOutView v = autopilot::HeldOutNow();
    if (!v.pick_shown || io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
    // a 20th of the screen's height (54 px at 1080 lines); the lines wrap at nine tenths of its width
    const float px = std::clamp(io.DisplaySize.y / 20.0f, 18.0f, 110.0f), scale = px / 10.0f;
    const float width = io.DisplaySize.x * 0.9f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, io.DisplaySize.y * 0.8f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(kBack.x, kBack.y, kBack.z, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, kGold);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(3.0f, scale * 1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 5.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 2.5f * scale));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushFont(framemeter::LargeFont(), px);
    if (ImGui::Begin("##sc5_pick", nullptr, flags)) {
      const bool waiting = v.pick_held && v.pick_why.rfind("waiting", 0) == 0;
      ImGui::TextColored(kGold, "%s", waiting      ? "PLEASE WAIT"
                                      : v.pick_held ? "THIS CHARACTER CANNOT BE PICKED IN THIS MATCH"
                                                    : "IN THIS MATCH IT FIGHTS WITHOUT ITS EQUIPMENT");
      ImGui::TextColored(kWhite, "%s", v.pick_name.c_str());
      ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
      ImGui::TextWrapped("Why: %s.", v.pick_why.c_str());
      ImGui::PopStyleColor();
      ImGui::TextColored(kGold, "%s", waiting      ? "A picks it in a moment"
                                      : v.pick_held ? "B: choose another one"
                                                    : "A picks it like this, B chooses another one");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
  }
};

// A round started again after the two games went out of step (net/netplay.cpp, RoundAgain): said to both players for a
// few seconds, in the same large bold text, over the top of the fight
class RoundAgainDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit RoundAgainDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!net::RoundAgainNotice() || io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
    const float px = std::clamp(io.DisplaySize.y / 22.0f, 16.0f, 100.0f), scale = px / 10.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.30f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBack);
    ImGui::PushStyleColor(ImGuiCol_Border, kGold);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(2.0f, scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 3.0f * scale));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushFont(framemeter::LargeFont(), px);
    if (ImGui::Begin("##sc5_round_again", nullptr, flags)) {
      ImGui::TextColored(kGold, "OUT OF SYNC");
      ImGui::TextColored(kWhite, "THIS ROUND STARTS AGAIN");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
  }
};

}  // namespace

void Install(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer) return;
  new ResultChoiceDialog(drawer);  // a dialog keeps itself (rex/ui/imgui_dialog.h)
  new HeldOutDialog(drawer);
  new PickDialog(drawer);
  new RoundAgainDialog(drawer);
}

}  // namespace sc5::overlay
