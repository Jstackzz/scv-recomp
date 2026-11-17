#pragma once
// A VS fight's setup for an online match (versus.cpp): the rounds to win, the stage and round time both games take
// from player 1's game, player 2 on the second pad.

namespace sc5::versus {

// the coming fights are an online match's: this game's side (0 player 1, 1 player 2) and the rounds to win; off: none
void SetOnline(bool on, int side, int wins);

}  // namespace sc5::versus
