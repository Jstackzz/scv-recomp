#pragma once
// The player's own save in an online session (docs\GAME.md, "The player's own save online"; docs\PROTOCOL.md 3.8).
//
// From 0.1.8 the launcher starts one game with the player's own save (SC5_USER_DIR is the game folder's user):
// single player, Creation, Training and the online matches all happen in it. An online session is the time from a
// start line of an online match (mode versus or sparring, or a spectator's) until the game is back at its hold point
// after the launcher's park. In between:
//   - the save as it was when the session began is kept aside, and put back as it was when the session ends: the game
//     keeps its records in the save image in memory as it goes (the play time and VS Battle's time every second, a
//     match's counts at its result, the player's level, the unlocks it records), and an online session changes some
//     of the image for its own ends (the other player's created characters in slots 49 and 50, the player's own that
//     cannot be picked taken out of the lists);
//   - a save the game makes meanwhile (it saves on its way back to the main menu after a match) writes savedata.bin
//     as the session found it, not as the session left it: nothing of the session reaches the player's files;
//   - the two display options that could reach the fight (Battle Messages and Custom Effects: the game's options table,
//     ownsave.cpp) are at the console's defaults in both games, whatever each player chose (put back at the end).
// So an online match writes nothing to the player's save. Their buttons (Options, Control Settings) are not pinned:
// controls.h turns their own pad into the game's default layout before the netplay sends it.
namespace sc5::ownsave {

// an online session begins (the game leaves its hold point for an online match or to watch one); main thread
void BeginOnline();
// the session is over (the game is at its hold point again): the save image put back; main thread
void EndOnline();
// a session is on (any thread)
bool Online();

}  // namespace sc5::ownsave
