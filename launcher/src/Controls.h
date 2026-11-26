#pragma once
// The controls file the game reads (docs/PROTOCOL.md 3.1, SC5_CONTROLS): which keys press each button of the Xbox 360
// controller (the keyboard plays as player 1 while the game's window is in front), and what each button of a PC
// controller presses. Settings, Keyboard edits its keys (KeyboardBox); a game that is running reads it again within a
// second, and the launcher hands its path to every game it starts.

#include <QList>
#include <QString>
#include <QStringList>

#include <array>

class QKeyEvent;

namespace controls {

// the Xbox 360 controller's buttons, in the order the page lists them (the file's names: up down left right a b x y
// lb rb lt rt start back ls rs)
enum Control { Up, Down, Left, Right, A, B, X, Y, LB, RB, LT, RT, Start, Back, LS, RS, Count };
QString controlId(int control);     // the file's name: "up", "a", "lt"
QString controlLabel(int control);  // "D-pad up", "A", "LT (left trigger)"
// what the button does, then the button: "Horizontal attack (X)", "Guard (A)", "A+G (LB)" for a button that presses
// the game's buttons together in a fight (`fight`: its combination, kGame* bits; -1 the console's default); the button
// alone where it has no one job ("D-pad left", "Left stick click")
QString controlText(int control, const QString& gameId, int fight = -1);
int controlFromId(const QString& id);

// In a fight a button presses any combination of the game's own four buttons: A horizontal attack, B vertical attack,
// K kick, G guard (the game's own Control Settings offer 13 of the 16). The file's [fight] names it for the buttons
// the player set; a button it leaves out keeps the game's own setting (the console's Type A online, below).
constexpr int kGameA = 1, kGameB = 2, kGameK = 4, kGameG = 8;
bool isFightButton(int control);   // a b x y lb rb lt rt ls rs
int fightDefault(int control);     // Type A: A guard, B kick, X horizontal, Y vertical, LB A+G, LT B+G, RB A+B, RT A+B+K
QString comboText(int mask);       // "A+G", "Nothing"
int comboFromText(const QString& text);  // "a+g", "A + G" -> kGameA | kGameG; "" -> 0; -1 when it is not one

// a PC controller's button, named by where it sits (the bottom face button is Xbox A, PlayStation cross, Switch B)
struct Source {
    QString id;     // the file's name: "south", "left_shoulder", "paddle1"
    QString label;  // "Bottom face button (A / Cross)"
    QString hint;   // what it is called on each kind of controller
    int control;    // what it presses unless changed, -1 nothing
};
const QList<Source>& sources();

constexpr int kKeysPerButton = 2;

struct Map {
    bool keyboard = true;                      // the keyboard plays as player 1
    std::array<QList<int>, Count> keys;        // Windows virtual keys, at most kKeysPerButton each
    QList<int> presses;                        // per sources() entry: the control it presses, -1 nothing
    QString player2;                           // the controller on port 1 by its name (offline versus); empty: the second plugged in
    // in a fight: the game's buttons a button presses (kGame*), -1 the game's own
    std::array<int, Count> fight;
    Map() { fight.fill(-1); }
    int fightOf(int control) const {           // what the button presses in a fight: as set, else Type A
        return control >= 0 && control < Count && fight[control] >= 0 ? fight[control] : fightDefault(control);
    }
};
// the controllers plugged in now, by name as the games see them (SDL), for the player 2 choice
QStringList connectedControllers();
Map defaults();
QString file();  // controls.toml beside the launcher's settings file
Map load();      // the file's controls over the defaults
bool save(const Map& map, QString* error);

QString keyName(int vk);  // as the file writes it: "K", "Space", "LShift", a code the names miss as "0xE5"
int keyFromName(const QString& name);
QString keyLabel(int vk);  // for people: "K", "Left Shift", the sign this keyboard's layout puts on the key
// the virtual key of a press, left and right Shift and Ctrl apart; 0 for a key a game cannot use (Alt and F10 open
// the window menu, the Windows keys and Esc belong to Windows and to leaving, F3 is a game's diagnostics key)
int keyFromEvent(const QKeyEvent* event);

}  // namespace controls
