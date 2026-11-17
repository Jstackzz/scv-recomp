// What a player's sc5game.dll asks sc5.exe to log (sc5/player/include/rex/logging.h): the translated code's only log
// lines, the guest's traps. Built only into the package's ready-made sc5.exe (SC5_PLAYER_HOST, README.md here).

#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/logging.h>

extern "C" __declspec(dllexport) void Sc5GuestLog(int level, const char* format, const char* text, size_t size,
                                                  unsigned value) {
  const std::string line = text ? fmt::format(fmt::runtime(format), std::string_view(text, size))
                                : fmt::format(fmt::runtime(format), value);
  if (level == 0) {
    REXCPU_DEBUG("{}", line);
  } else {
    REXCPU_WARN("{}", line);
  }
}
