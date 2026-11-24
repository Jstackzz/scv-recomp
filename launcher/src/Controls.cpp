#include "Controls.h"

#include <SDL3/SDL.h>

#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QRegularExpression>

#include <windows.h>

#include "AppPaths.h"
#include "WinUtil.h"

namespace controls {

namespace {

struct ControlInfo {
    const char* id;
    const char* label;
};
const ControlInfo kControls[Count] = {
    {"up", "D-pad up"},          {"down", "D-pad down"},        {"left", "D-pad left"},      {"right", "D-pad right"},
    {"a", "A"},                  {"b", "B"},                    {"x", "X"},                  {"y", "Y"},
    {"lb", "LB (left bumper)"},  {"rb", "RB (right bumper)"},   {"lt", "LT (left trigger)"}, {"rt", "RT (right trigger)"},
    {"start", "Start"},          {"back", "Back"},              {"ls", "Left stick click"},  {"rs", "Right stick click"},
};

// The runtime's key names (core/input.cpp kKeyNames) with what the page shows; no label: the sign the keyboard's
// layout puts on that key. Letters, digits, F1-F24 and Num0-Num9 are named by pattern.
struct KeyInfo {
    int vk;
    const char* name;
    const char* label;
};
const KeyInfo kKeys[] = {
    {VK_UP, "Up", "Up arrow"},
    {VK_DOWN, "Down", "Down arrow"},
    {VK_LEFT, "Left", "Left arrow"},
    {VK_RIGHT, "Right", "Right arrow"},
    {VK_SPACE, "Space", "Space"},
    {VK_RETURN, "Enter", "Enter"},
    {VK_BACK, "Backspace", "Backspace"},
    {VK_TAB, "Tab", "Tab"},
    {VK_CAPITAL, "CapsLock", "Caps Lock"},
    {VK_SHIFT, "Shift", "Shift"},
    {VK_LSHIFT, "LShift", "Left Shift"},
    {VK_RSHIFT, "RShift", "Right Shift"},
    {VK_CONTROL, "Ctrl", "Ctrl"},
    {VK_LCONTROL, "LCtrl", "Left Ctrl"},
    {VK_RCONTROL, "RCtrl", "Right Ctrl"},
    {VK_INSERT, "Insert", "Insert"},
    {VK_DELETE, "Delete", "Delete"},
    {VK_HOME, "Home", "Home"},
    {VK_END, "End", "End"},
    {VK_PRIOR, "PageUp", "Page Up"},
    {VK_NEXT, "PageDown", "Page Down"},
    {VK_MULTIPLY, "NumMultiply", "Num *"},
    {VK_ADD, "NumAdd", "Num +"},
    {VK_SUBTRACT, "NumSubtract", "Num -"},
    {VK_DECIMAL, "NumDecimal", "Num ."},
    {VK_DIVIDE, "NumDivide", "Num /"},
    {VK_OEM_1, "Semicolon", nullptr},
    {VK_OEM_PLUS, "Equals", nullptr},
    {VK_OEM_COMMA, "Comma", nullptr},
    {VK_OEM_MINUS, "Minus", nullptr},
    {VK_OEM_PERIOD, "Period", nullptr},
    {VK_OEM_2, "Slash", nullptr},
    {VK_OEM_3, "Grave", nullptr},
    {VK_OEM_4, "LBracket", nullptr},
    {VK_OEM_5, "Backslash", nullptr},
    {VK_OEM_6, "RBracket", nullptr},
    {VK_OEM_7, "Apostrophe", nullptr},
    {VK_OEM_8, "Oem8", nullptr},
    {VK_OEM_102, "Oem102", nullptr},
};

const KeyInfo* keyInfo(int vk) {
    for (const KeyInfo& k : kKeys)
        if (k.vk == vk) return &k;
    return nullptr;
}

bool letterOrDigit(int vk) { return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'); }

int sourceFromId(const QString& id) {
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id.compare(id, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

// a line without its comment: from a '#' outside quotes on
QString uncommented(const QString& line) {
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        if (line[i] == QLatin1Char('"')) quoted = !quoted;
        else if (line[i] == QLatin1Char('#') && !quoted) return line.left(i);
    }
    return line;
}

// the quoted strings of a value: "K" or ["W", "Up"]
QStringList strings(const QString& value) {
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]*)\""));
    QStringList out;
    for (auto it = quoted.globalMatch(value); it.hasNext();) out << it.next().captured(1);
    return out;
}

}  // namespace

QString controlId(int control) {
    return control >= 0 && control < Count ? QLatin1String(kControls[control].id) : QString();
}

QString controlLabel(int control) {
    return control >= 0 && control < Count ? QLatin1String(kControls[control].label) : QString();
}

// Soulcalibur V's buttons in a fight: the face buttons are its four by default (horizontal and vertical attack, kick
// and guard), the bumpers and triggers press them together; [fight] can give any of these any combination
QString controlText(int control, const QString& gameId, int fight) {
    Q_UNUSED(gameId);
    QString action;
    if (isFightButton(control)) {
        const int mask = fight >= 0 ? fight : fightDefault(control);
        switch (mask) {
        case kGameA: action = QStringLiteral("Horizontal attack"); break;
        case kGameB: action = QStringLiteral("Vertical attack"); break;
        case kGameK: action = QStringLiteral("Kick"); break;
        case kGameG: action = QStringLiteral("Guard"); break;
        case 0: if (control != LS && control != RS) action = QStringLiteral("Nothing in a fight"); break;
        default: action = comboText(mask); break;
        }
    } else {
        switch (control) {
        case Up: action = QStringLiteral("Jump"); break;
        case Down: action = QStringLiteral("Crouch"); break;
        case Start: action = QStringLiteral("Pause"); break;
        default: break;
        }
    }
    const QString button = controlLabel(control);
    return action.isEmpty() ? button : QStringLiteral("%1 (%2)").arg(action, button);
}

bool isFightButton(int control) {
    switch (control) {
    case A: case B: case X: case Y: case LB: case RB: case LT: case RT: case LS: case RS: return true;
    default: return false;
    }
}

// the console's Type A, which a fresh save has (an online game's): read from the game's own settings in memory
int fightDefault(int control) {
    switch (control) {
    case A: return kGameG;
    case B: return kGameK;
    case X: return kGameA;
    case Y: return kGameB;
    case LB: return kGameA | kGameG;
    case LT: return kGameB | kGameG;
    case RB: return kGameA | kGameB;
    case RT: return kGameA | kGameB | kGameK;
    default: return 0;
    }
}

QString comboText(int mask) {
    QStringList parts;
    const char* names[4] = {"A", "B", "K", "G"};
    for (int i = 0; i < 4; ++i)
        if (mask >> i & 1) parts << QLatin1String(names[i]);
    return parts.isEmpty() ? QStringLiteral("Nothing") : parts.join(QLatin1Char('+'));
}

int comboFromText(const QString& text) {
    int mask = 0;
    for (const QChar ch : text) {
        switch (ch.toUpper().unicode()) {
        case 'A': mask |= kGameA; break;
        case 'B': mask |= kGameB; break;
        case 'K': mask |= kGameK; break;
        case 'G': mask |= kGameG; break;
        case '+': case ' ': break;
        default: return -1;
        }
    }
    return mask;
}

int controlFromId(const QString& id) {
    for (int c = 0; c < Count; ++c)
        if (id.compare(QLatin1String(kControls[c].id), Qt::CaseInsensitive) == 0) return c;
    return -1;
}

const QList<Source>& sources() {
    static const QList<Source> all = {
        {QStringLiteral("south"), QStringLiteral("Bottom face button (A / Cross)"),
         QStringLiteral("Xbox A, PlayStation cross, Switch B"), A},
        {QStringLiteral("east"), QStringLiteral("Right face button (B / Circle)"),
         QStringLiteral("Xbox B, PlayStation circle, Switch A"), B},
        {QStringLiteral("west"), QStringLiteral("Left face button (X / Square)"),
         QStringLiteral("Xbox X, PlayStation square, Switch Y"), X},
        {QStringLiteral("north"), QStringLiteral("Top face button (Y / Triangle)"),
         QStringLiteral("Xbox Y, PlayStation triangle, Switch X"), Y},
        {QStringLiteral("right_shoulder"), QStringLiteral("Right bumper (RB / R1)"),
         QStringLiteral("Xbox RB, PlayStation R1, Switch R"), RB},
        {QStringLiteral("left_shoulder"), QStringLiteral("Left bumper (LB / L1)"),
         QStringLiteral("Xbox LB, PlayStation L1, Switch L"), LB},
        {QStringLiteral("right_trigger"), QStringLiteral("Right trigger (RT / R2)"),
         QStringLiteral("Xbox RT, PlayStation R2, Switch ZR"), RT},
        {QStringLiteral("left_trigger"), QStringLiteral("Left trigger (LT / L2)"),
         QStringLiteral("Xbox LT, PlayStation L2, Switch ZL"), LT},
        {QStringLiteral("dpad_up"), QStringLiteral("D-pad up"), QString(), Up},
        {QStringLiteral("dpad_down"), QStringLiteral("D-pad down"), QString(), Down},
        {QStringLiteral("dpad_left"), QStringLiteral("D-pad left"), QString(), Left},
        {QStringLiteral("dpad_right"), QStringLiteral("D-pad right"), QString(), Right},
        {QStringLiteral("start"), QStringLiteral("Menu / Start / Options"),
         QStringLiteral("Xbox Menu (Start on a 360 pad), PlayStation Options, Switch +"), Start},
        {QStringLiteral("back"), QStringLiteral("View / Back / Share"),
         QStringLiteral("Xbox View (Back on a 360 pad), PlayStation Share or Create, Switch -"), Back},
        {QStringLiteral("left_stick"), QStringLiteral("Left stick click (L3)"), QString(), LS},
        {QStringLiteral("right_stick"), QStringLiteral("Right stick click (R3)"), QString(), RS},
        {QStringLiteral("misc"), QStringLiteral("Extra middle button"),
         QStringLiteral("Xbox Series Share, PlayStation 5 mic, Switch Capture"), -1},
        {QStringLiteral("paddle1"), QStringLiteral("Back paddle P1 (upper right)"),
         QStringLiteral("Xbox Elite P1, DualSense Edge right back button"), -1},
        {QStringLiteral("paddle2"), QStringLiteral("Back paddle P2 (lower right)"),
         QStringLiteral("Xbox Elite P2, DualSense Edge right Fn"), -1},
        {QStringLiteral("paddle3"), QStringLiteral("Back paddle P3 (upper left)"),
         QStringLiteral("Xbox Elite P3, DualSense Edge left back button"), -1},
        {QStringLiteral("paddle4"), QStringLiteral("Back paddle P4 (lower left)"),
         QStringLiteral("Xbox Elite P4, DualSense Edge left Fn"), -1},
        {QStringLiteral("touchpad"), QStringLiteral("Touchpad click"), QStringLiteral("PlayStation"), -1},
    };
    return all;
}

// without the file: arrows or WASD move, J I L K are X Y B A (horizontal, vertical, kick, guard; Space guards too),
// U and O the bumpers, Y and P the triggers, Enter Start, Backspace Back
Map defaults() {
    Map m;
    m.keys[Up] = {VK_UP, 'W'};
    m.keys[Down] = {VK_DOWN, 'S'};
    m.keys[Left] = {VK_LEFT, 'A'};
    m.keys[Right] = {VK_RIGHT, 'D'};
    m.keys[A] = {'K', VK_SPACE};
    m.keys[B] = {'L'};
    m.keys[X] = {'J'};
    m.keys[Y] = {'I'};
    m.keys[LB] = {'U'};
    m.keys[RB] = {'O'};
    m.keys[LT] = {'Y'};
    m.keys[RT] = {'P'};
    m.keys[Start] = {VK_RETURN};
    m.keys[Back] = {VK_BACK};
    for (const Source& s : sources()) m.presses << s.control;
    return m;
}

QString file() { return QDir(paths::dataDir()).filePath(QStringLiteral("controls.toml")); }

QStringList connectedControllers() {
    QStringList out;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_Init(SDL_INIT_GAMEPAD)) return out;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        if (name && *name && !out.contains(QString::fromUtf8(name))) out << QString::fromUtf8(name);
    }
    SDL_free(ids);
    return out;
}

Map load() {
    Map m = defaults();
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly)) return m;
    QString section;
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);  // saved by Notepad
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = uncommented(raw).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (section.isEmpty() && key == QLatin1String("keyboard")) {
            m.keyboard = value != QLatin1String("false");
        } else if (section == QLatin1String("keys")) {
            const int c = controlFromId(key);
            if (c < 0) continue;
            m.keys[c].clear();
            for (const QString& name : strings(value)) {
                const int vk = keyFromName(name);
                if (vk && !m.keys[c].contains(vk) && m.keys[c].size() < kKeysPerButton) m.keys[c] << vk;
            }
        } else if (section == QLatin1String("ports")) {
            if (key == QLatin1String("player2")) {
                const QStringList v = strings(value);
                m.player2 = v.isEmpty() ? QString() : v.first();
            }
        } else if (section == QLatin1String("buttons")) {
            const int i = sourceFromId(key);
            if (i < 0) continue;
            const QStringList target = strings(value);
            m.presses[i] = target.isEmpty() || target.first().isEmpty() ? -1 : controlFromId(target.first());
        } else if (section == QLatin1String("fight")) {
            const int c = controlFromId(key);
            const QStringList v = strings(value);
            if (c >= 0 && isFightButton(c) && !v.isEmpty()) m.fight[c] = comboFromText(v.first());
        }
    }
    return m;
}

bool save(const Map& m, QString* error) {
    QString text = QStringLiteral(
        "# SCV Online controls. The launcher's Controls page writes this file; a game that is running reads it again\n"
        "# within a second of a change. The buttons are the Xbox 360 controller's: up down left right (the d-pad),\n"
        "# a b x y, lb rb (bumpers), lt rt (triggers), start back, ls rs (stick clicks).\n"
        "\n"
        "# The keyboard plays as player 1 while the game's window is in front.\n");
    text += QStringLiteral("keyboard = %1\n\n").arg(m.keyboard ? QStringLiteral("true") : QStringLiteral("false"));
    text += QStringLiteral("# The keys that press each button.\n[keys]\n");
    for (int c = 0; c < Count; ++c) {
        QStringList names;
        for (int vk : m.keys[c]) names << QLatin1Char('"') + keyName(vk) + QLatin1Char('"');
        text += QStringLiteral("%1 = [%2]\n").arg(controlId(c), names.join(QStringLiteral(", ")));
    }
    text += QStringLiteral("\n# What each button of a PC controller presses, by where the button sits (\"\" nothing).\n"
                           "[buttons]\n");
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i)
        text += QStringLiteral("%1 = \"%2\"\n").arg(all[i].id, controlId(i < m.presses.size() ? m.presses[i] : -1));
    text += QStringLiteral("\n# Player 2's controller (the offline versus) by its name; \"\" the second one plugged in. While a\n"
                           "# name is here, every other controller keeps clear of player 2's port.\n[ports]\n");
    QString name = m.player2;
    name.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    text += QStringLiteral("player2 = \"%1\"\n").arg(name);
    text += QStringLiteral(
        "\n# What a button presses in a fight, in every mode, online and offline: any of the game's own four buttons\n"
        "# together, A horizontal attack, B vertical attack, K kick, G guard (\"\" nothing). A button not listed keeps\n"
        "# the game's own Control Settings (online the console's Type A).\n[fight]\n");
    for (int c = 0; c < Count; ++c)
        if (isFightButton(c) && m.fight[c] >= 0)
            text += QStringLiteral("%1 = \"%2\"\n").arg(controlId(c), m.fight[c] ? comboText(m.fight[c]) : QString());

    // whole or not at all: a game reading it at that moment holds up the swap for an instant
    QString why;
    if (winutil::writeFileAtomic(file(), text.toUtf8(), &why)) return true;
    if (error) *error = QStringLiteral("Could not save the controls: %1").arg(why);
    return false;
}

QString keyName(int vk) {
    if (letterOrDigit(vk)) return QString(QLatin1Char(char(vk)));
    if (const KeyInfo* k = keyInfo(vk)) return QLatin1String(k->name);
    if (vk >= VK_F1 && vk <= VK_F24) return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return QStringLiteral("Num%1").arg(vk - VK_NUMPAD0);
    return QStringLiteral("0x") + QStringLiteral("%1").arg(vk, 2, 16, QLatin1Char('0')).toUpper();
}

int keyFromName(const QString& name) {
    const QString s = name.trimmed();
    if (s.size() == 1 && s[0].isLetterOrNumber() && s[0].unicode() < 128) return s[0].toUpper().unicode();
    for (const KeyInfo& k : kKeys)
        if (s.compare(QLatin1String(k.name), Qt::CaseInsensitive) == 0) return k.vk;
    bool ok = false;
    if (s.size() >= 2 && s[0].toUpper() == QLatin1Char('F')) {
        const int n = s.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24 && s.mid(1).at(0).isDigit()) return VK_F1 + n - 1;
    }
    if (s.size() == 4 && s.left(3).compare(QLatin1String("Num"), Qt::CaseInsensitive) == 0 && s[3].isDigit())
        return VK_NUMPAD0 + s[3].digitValue();
    if (s.size() > 2 && s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const int v = s.mid(2).toInt(&ok, 16);
        if (ok && v > 0 && v < 0xFF) return v;
    }
    return 0;
}

QString keyLabel(int vk) {
    if (letterOrDigit(vk)) return QString(QLatin1Char(char(vk)));
    if (const KeyInfo* k = keyInfo(vk); k && k->label) return QLatin1String(k->label);
    if (vk >= VK_F1 && vk <= VK_F24) return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return QStringLiteral("Num %1").arg(vk - VK_NUMPAD0);
    // a sign key: what this keyboard's layout prints on it (the high bit marks a dead key)
    const UINT sign = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (sign > 0x20) return QString(QChar(char16_t(sign)));
    wchar_t text[64] = {};
    const LONG scan = LONG(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    if (scan && GetKeyNameTextW(scan, text, 64) > 0) return QString::fromWCharArray(text);
    return keyName(vk);
}

int keyFromEvent(const QKeyEvent* event) {
    int vk = int(event->nativeVirtualKey());
    const quint32 scan = event->nativeScanCode();  // bit 8: an extended key (the right-hand Ctrl)
    if (vk == VK_SHIFT) vk = (scan & 0xFF) == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) vk = (scan & 0x100) ? VK_RCONTROL : VK_LCONTROL;
    switch (vk) {
        case VK_ESCAPE:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_F10:
        case VK_F3:
        case VK_F6:  // kept for the game's practice tools (MK3D's: the dummy's tech roll)
        case VK_F7:  // frame data in practice, the dummy's guard
        case VK_F8:
        case VK_F9:  // the practice record: record, play (F10 above), slot, playback mode
        case VK_F11:
        case VK_F12:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_NUMLOCK:
        case VK_SCROLL:
        case VK_PAUSE:
        case VK_SNAPSHOT:
        case VK_PROCESSKEY:
        case VK_PACKET:
            return 0;
        default:
            return vk > 0 && vk < 0xFF ? vk : 0;
    }
}

}  // namespace controls
