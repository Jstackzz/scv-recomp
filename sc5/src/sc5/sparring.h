#pragma once
// Online Practice ("sparring", docs\PROTOCOL.md 3.3; as MK3D Online's mk3d/sparring.h): two friends in a lobby fight as
// long as they like. VS Battle has no "no time limit" setting (Round Time 30, 45 or 60), so the round's clock is held
// where it starts; a fighter's health is back to full once it took no damage for a while, and at once below a floor,
// so nobody is knocked out (a ring out still ends a round; the match plays to 5 rounds). A player's BACK ends the match
// in sync (netplay.cpp: it goes over the wire, the game never sees it), the launcher reports it and the lobby's next
// match brings both players to the select screen again.
//
// The rules run at the top of every frame of the match in sync, each frame a rollback runs again included, the same on
// both games: what they remember is kept per frame, so a frame run again starts from what the frame before it left.
#include <cstdint>

namespace sc5::sparring {

// the match the launcher started is Online Practice (link.cpp, at each start)
void Set(bool on);
bool On();

// netplay, at the top of match frame `frame` (in sync), before the game runs it
void OnFrame(uint32_t frame);

}  // namespace sc5::sparring
