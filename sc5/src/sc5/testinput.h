#pragma once
// Scripted controllers for tests (ported from MK9 Online's mk9/testinput.h):
//   SC5_INPUT_P1..P4  a script per player: comma-separated <frame>:<buttons> steps, a frame as a number or +<n>
//                     after the step before, buttons joined with + (UP DOWN LEFT RIGHT A B X Y LB RB LT RT START
//                     BACK LS RS) or - for none; each step holds until the next. "300:START,+10:-,+120:A,+10:-"
//   SC5_FIGHT_INPUT_P1..P4  the same timed in fight frames (game::FightFrame, 0 = round 1's call), used once a fight's
//                     clock has started; the menus before it keep the SC5_INPUT_Pn script
//   SC5_SELECT_INPUT_P1..P4  the same timed in the select screen's frames (select::Age, 0 = the screen set up), used
//                     while a select screen is up: a player's picks however long the way there took
//   SC5_PADS=<n>      users 0..n-1 connected even without a controller
//   SC5_NO_PHYSICAL=1 the PC's own controllers are not read at all
#include <cstdint>

#include "sc5/input.h"

namespace sc5::testinput {

// reads the environment and installs the scripts as the input source (once, at startup)
void Init();
bool Active();
// a scripted user's pad now (its fight script in a fight, else its menu script): the netplay's local input in tests
bool ScriptedPad(uint32_t user, input::Pad& out);

}  // namespace sc5::testinput
