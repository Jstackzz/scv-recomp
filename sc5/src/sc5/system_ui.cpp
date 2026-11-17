// The system UI the game asks for: message boxes it would wait on, see system_ui.h.

#include "sc5/system_ui.h"

#include <string>

#include <fmt/format.h>

#include <rex/kernel/xam/ui.h>
#include <rex/logging.h>

namespace sc5::system_ui {
namespace {

// The prompts answered for the player, by the game's call that raises them (a return address in the chain, the same
// in every language). The game's message box helper is sub_824906D8(task, buttons, title id, text id).
struct Known {
  uint32_t caller;
  uint32_t button;
  const char* what;
};
constexpr Known kKnown[] = {
    // the save task finding no save data at the first START (title 14, text 18): "No save data found for
    // SOULCALIBUR V. You must create save data in order to save your progress." [Create Save Data] [Select Storage
    // Device]; the saves live in the user folder, there is no storage device to pick
    {0x824911C8, 0, "no save data: Create Save Data"},
};

std::optional<uint32_t> Answer(const rex::kernel::xam::MessageBoxRequest& r) {
  std::string buttons, callers;
  for (const auto& b : r.buttons) buttons += fmt::format(" [{}]", b);
  for (uint32_t c : r.callers) callers += fmt::format(" {:08X}", c);
  for (const Known& k : kKnown) {
    for (uint32_t c : r.callers) {
      if (c != k.caller || k.button >= r.buttons.size()) continue;
      REXLOG_INFO("sc5: message box answered ({}): {} |{}", k.what, r.text, buttons);
      return k.button;
    }
  }
  REXLOG_WARN("sc5: message box shown to the player: {} | {} |{} (focus {}) from{}", r.title, r.text, buttons,
              r.active_button, callers);
  return std::nullopt;
}

}  // namespace

void Init() { rex::kernel::xam::SetMessageBoxAnswer(&Answer); }

}  // namespace sc5::system_ui
