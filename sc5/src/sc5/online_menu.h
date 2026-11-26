#pragma once
// The game's own online menus (docs\GAME.md, "The game's online menus"; docs\PROTOCOL.md 3.8). With SC5_NET=1 and
// SC5_MENUS=1 the main menu's Xbox LIVE entry, which cannot work here, becomes SCV Online: its label and description
// come from us through the game's text tables, and A on it opens our menus instead of Xbox LIVE. They are drawn over
// the main menu, which holds still behind them, in the look of the game's own menus (online_draw.cpp), and take the
// player's pad while they are up: Ranked Match, Casual Match, Lobbies, Practice (the game's own Training), Leaderboard,
// Preferred Side and Back, the search and the match found, the lobbies and a lobby's room, the series after a match,
// and whatever the launcher has to say (ui.json, sc5/ui_link.h). The player's choices go to the launcher as asks.
//
// While the menus are up the game is at its hold point: status.txt says `ready` and a `start` from the launcher is
// taken as before (the autopilot drives the game to the fight); after the match `park` brings it back to the main menu
// with the menus open again. Elsewhere in the game (single player, Creation, Options, the main menu itself) the player
// moves freely and the status says `away`.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sc5/input.h"

struct PPCContext;
struct ImFontAtlas;
namespace rex::ui {
class ImGuiDrawer;
}

namespace sc5::online {

// the entry's name in the main menu and the menus' title: one constant (online_menu.cpp)
extern const char* const kName;

// with the launcher's link (link::Init, before the autopilot's): SC5_MENUS=1
void Init();
bool On();

// main thread, at the top of each frame after the autopilot's: the player's pad, the screens, the asks
void OnFrame(PPCContext& ctx, uint8_t* base);
// the menus are open (the autopilot's hold point; they show while the game is at the main menu)
bool Open();
// The autopilot's pad override at its hold point (any thread): true when the game gets `out` instead of the pad (a
// still pad while the menus are up, and until every button that opened or closed them is let go)
bool HoldPad(uint32_t user, input::Pad& out);
// link.cpp: `park` came, the match is left: the menus show its series once the game is back at the main menu
void OnPark();
// The game's texts (dlc.cpp's hook of sub_82347670): the guest address of our string for a table's string, or 0 for the
// game's own. Main thread.
uint32_t Text(PPCContext& ctx, uint8_t* base, uint32_t table, uint32_t index);

// Sc5App::OnConfigureFonts and OnCreateDialogs (online_draw.cpp)
void AddFonts(ImFontAtlas* atlas);
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);

// --- what online_draw.cpp draws (a copy made on the main thread each frame) ---

struct Row {
  std::string label;   // the bar's text
  std::string value;   // a setting's value, after the label ("Off"); "" none
  std::string detail;  // smaller text at the bar's right end ("1/2 players"); "" none
  bool enabled = true;  // false: shown dimmed with "NOT AVAILABLE"
  bool mine = false;    // the player's own row (the leaderboard): marked "YOU"
  bool arrows = false;  // LEFT / RIGHT change the value: arrows around it
};
struct Line {
  std::string name, value;
  int mark = 0;  // 0 none, 1 READY (a tick), 2 NOT READY (an empty box)
  bool strong = false;
};
struct View {
  bool shown = false;
  std::string title;
  std::vector<Row> rows;
  int cursor = 0;
  std::string desc;  // under the chosen bar
  // the stage above the list (the search, a match found, a series' score)
  bool stage = false;
  std::string stage_head, stage_big, stage_sub;
  std::vector<std::string> stage_lines;
  // the panel on the right: the account, then a second card (a lobby's members, the series)
  std::string name, rank;
  std::vector<Line> account;
  std::string card_head;
  std::vector<Line> card;
  std::vector<std::pair<std::string, std::string>> news;  // a heading ("UPDATE READY", "SERVER NOTICE") and its text
  // a state the player must know (not signed in, the launcher not answering ...), over the description
  std::string banner_head, banner_text;
  bool banner_error = false;
  // a message to close with A (the launcher's `message`, or why something cannot be done)
  bool popup = false;
  bool popup_error = false;
  std::string popup_head, popup_text;
  std::vector<std::pair<std::string, std::string>> legend;  // button ("A", "B", "X", "Y", "LR", "UD"), what it does
};
View ViewNow();

}  // namespace sc5::online
