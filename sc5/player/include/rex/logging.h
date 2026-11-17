// The translated code logs only guest traps (ppc_trap in the generated header); sc5.exe writes those
// (sc5/player/guestlog.cpp).
#pragma once
#include <cstddef>
#include <string_view>
extern "C" void Sc5GuestLog(int level, const char* format, const char* text, size_t size, unsigned value);
inline void Sc5GuestLogArg(int level, const char* format, std::string_view text) {
  Sc5GuestLog(level, format, text.data(), text.size(), 0);
}
inline void Sc5GuestLogArg(int level, const char* format, unsigned value) {
  Sc5GuestLog(level, format, nullptr, 0, value);
}
#define REXCPU_DEBUG(format, argument) Sc5GuestLogArg(0, format, argument)
#define REXCPU_WARN(format, argument) Sc5GuestLogArg(1, format, argument)
