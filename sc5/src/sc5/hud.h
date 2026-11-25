#pragma once
// The fight's HUD and its round markers (docs\GAME.md, "Rollback": the HUD's round markers).
#include <cstdint>

namespace sc5::hud {

// The HUD's object while a fight shows one (0x82A06B38: +0 the current HUD's id, +4 the object, current while its +48
// is that id), else 0
uint32_t Object();
// The HUD's own count of each side's rounds won (+0x94 player 1, +0x98 player 2), which its markers show: kept and put
// back with each frame of a fight (battle.cpp's DynamicRegions) unless SC5_HUD_KEEP=0 (tests: the markers as before)
constexpr uint32_t kMarkers = 0x94, kMarkersSize = 8;
bool KeepMarkers();
// At each frame's end: a frame's first run whose markers show other than the rounds won is logged (tests)
void OnFrameEnd();

}  // namespace sc5::hud
