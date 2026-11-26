#include "ControlsPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "WinUtil.h"

namespace {

// Waits for one key, Tab, Enter and Space included: accepted with that key, done(kNoKey) to take the key away, rejected
// by Esc or Cancel.
class KeyDialog : public QDialog {
public:
    static constexpr int kNoKey = 2;

    KeyDialog(const QString& button, const QString& now, QWidget* parent) : QDialog(parent) {
        setWindowTitle(tr("Change key"));
        setModal(true);
        setMinimumWidth(380);
        setFocusPolicy(Qt::StrongFocus);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(24, 20, 24, 18);
        v->setSpacing(12);
        v->addWidget(ui::label(tr("Press the key for %1").arg(button), "Big"));
        hint_ = ui::label(now.isEmpty() ? tr("Esc cancels.") : tr("It is %1 now. Esc cancels.").arg(now), "Muted");
        hint_->setWordWrap(true);
        v->addWidget(hint_);
        auto* row = new QHBoxLayout;
        row->addStretch();
        auto* none = new QPushButton(tr("No key"));
        auto* cancel = new QPushButton(tr("Cancel"));
        for (QPushButton* b : {none, cancel}) {
            b->setFocusPolicy(Qt::NoFocus);  // every key comes to the dialog
            b->setAutoDefault(false);
            row->addWidget(b);
        }
        v->addLayout(row);
        connect(none, &QPushButton::clicked, this, [this] { done(kNoKey); });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    }
    int key() const { return key_; }

protected:
    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        setFocus();
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() != QEvent::KeyPress) return QDialog::event(event);
        const auto* k = static_cast<QKeyEvent*>(event);
        if (k->isAutoRepeat()) return true;
        if (k->key() == Qt::Key_Escape) {
            reject();
        } else if (const int vk = controls::keyFromEvent(k)) {
            key_ = vk;
            accept();
        } else {
            QString name = QKeySequence(k->key()).toString(QKeySequence::NativeText);
            if (name.isEmpty()) name = tr("That key");
            hint_->setText(tr("%1 is kept for Windows or the game; press another key. Esc cancels.").arg(name));
        }
        return true;
    }

private:
    QLabel* hint_;
    int key_ = 0;
};

QString keyText(const QList<int>& keys, int slot) {
    return slot < keys.size() ? controls::keyLabel(keys[slot]) : QStringLiteral("—");
}

// the buttons in the order a player thinks of them: moving, the four (horizontal, vertical, kick, guard), the
// shortcuts, the rest
using namespace controls;
constexpr Control kOrder[] = {Up, Down, Left, Right, X, Y, B, A, RB, RT, LB, LT, Start, Back, LS, RS};
static_assert(std::size(kOrder) == Count);

// IN A FIGHT's rows, two to a row: the four face buttons and the left stick's click, the bumpers and triggers and the
// right stick's
constexpr Control kFightOrder[] = {X, RB, Y, RT, B, LB, A, LT, LS, RS};
// every combination of the game's four, the single buttons first
constexpr int kCombos[] = {0,
                           kGameA,
                           kGameB,
                           kGameK,
                           kGameG,
                           kGameA | kGameB,
                           kGameA | kGameK,
                           kGameA | kGameG,
                           kGameB | kGameK,
                           kGameB | kGameG,
                           kGameK | kGameG,
                           kGameA | kGameB | kGameK,
                           kGameA | kGameB | kGameG,
                           kGameA | kGameK | kGameG,
                           kGameB | kGameK | kGameG,
                           kGameA | kGameB | kGameK | kGameG};

QString comboLabel(int mask, int byDefault) {
    QString text;
    switch (mask) {
    case kGameA: text = QStringLiteral("A (horizontal attack)"); break;
    case kGameB: text = QStringLiteral("B (vertical attack)"); break;
    case kGameK: text = QStringLiteral("K (kick)"); break;
    case kGameG: text = QStringLiteral("G (guard)"); break;
    default: text = comboText(mask); break;
    }
    return mask == byDefault ? QStringLiteral("%1 (default)").arg(text) : text;
}

}  // namespace

ControlsPage::ControlsPage(QWidget* parent) : Page(parent) {
    using namespace controls;
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(10);
    root->addWidget(ui::pageHeader(
        tr("Controls"), tr("For the game on this PC: they only change what your own keys and buttons press, so "
                           "they work the same in online matches. A game that is running picks up a change within a "
                           "second.")));

    // the side the player wants to start every fight on, whichever player they are (as Tekken 8's): the game shows
    // the fight mirrored when they would start on the other side; three large buttons, white on dark, the one chosen
    // bright blue with a gold frame (SideChoice, Style.cpp)
    auto* sideBox = new QGroupBox(tr("PREFERRED SIDE"));
    auto* sv = new QVBoxLayout(sideBox);
    sv->addWidget(ui::label(tr("Start every fight on the side you choose"), "Big"));
    auto* sideRow = new QHBoxLayout;
    sideRow->setSpacing(12);
    auto* sideGroup = new QButtonGroup(sideBox);
    sideGroup->setExclusive(true);
    const struct {
        const char* value;
        const char* text;
        const char* say;
    } sides[] = {
        {"off", QT_TR_NOOP("Off"), QT_TR_NOOP("Preferred side off: fights are shown as they are.")},
        {"left", QT_TR_NOOP("Left"), QT_TR_NOOP("Preferred side: left. You start every fight on the left.")},
        {"right", QT_TR_NOOP("Right"), QT_TR_NOOP("Preferred side: right. You start every fight on the right.")},
    };
    const QString chosen = cfg::preferredSide();
    for (const auto& s : sides) {
        auto* b = new QPushButton(tr(s.text));
        b->setObjectName(QStringLiteral("SideChoice"));
        b->setCheckable(true);
        b->setChecked(chosen == QLatin1String(s.value));
        b->setMinimumWidth(150);
        b->setCursor(Qt::PointingHandCursor);
        b->setAccessibleName(tr("Preferred side: %1").arg(tr(s.text)));
        sideGroup->addButton(b);
        sideRow->addWidget(b);
        const QString value = QString::fromLatin1(s.value), said = tr(s.say);
        connect(b, &QPushButton::toggled, this, [this, value, said](bool on) {
            if (!on) return;
            cfg::set(QStringLiteral("display/preferred_side"), value);  // a running game follows it (the display file)
            say(said);
        });
    }
    sideRow->addStretch();
    sv->addLayout(sideRow);
    auto* sideNote = ui::label(tr("Off: player 1 starts on the left and player 2 on the right, as always. Left or "
                                  "Right: when you would start on the other side, your game shows the fight mirrored, "
                                  "so you start on your side, and your left and right in the fight are swapped with "
                                  "it, so forward is still toward your opponent. The text stays readable; the health "
                                  "bars stay where the game puts them (player 1's on the left). Only your screen "
                                  "changes: your opponent may choose the same side, and each of you sees it your own "
                                  "way. Online, Online Practice, and offline with one player (VS CPU, Training, "
                                  "Arcade); not with two players on one PC, and never when watching."),
                              "Muted");
    sideNote->setWordWrap(true);
    sv->addWidget(sideNote);
    root->addWidget(sideBox);

    // in a fight: what each button presses of the game's own four, any of them together
    auto* fightBox = new QGroupBox(tr("IN A FIGHT"));
    auto* fv = new QVBoxLayout(fightBox);
    auto* fightNote = ui::label(tr("What each button presses in a fight, in every mode, online and offline: A "
                                   "horizontal attack, B vertical attack, K kick, G guard, or several of them at once "
                                   "from one button (A+G throws, A+B+K is a Critical Edge's buttons). Your opponent's "
                                   "game sees your presses as you set them here. A button at its default keeps, "
                                   "offline, what the game's own Control Settings give it."),
                                "Muted");
    fightNote->setWordWrap(true);
    fv->addWidget(fightNote);
    auto* fightGrid = new QGridLayout;
    fightGrid->setHorizontalSpacing(10);
    fightGrid->setVerticalSpacing(4);
    for (int i = 0; i < int(std::size(kFightOrder)); ++i) {
        const int c = kFightOrder[i];
        const int row = i / 2, col = (i % 2) * 3;
        auto* box = new QComboBox;
        box->setMinimumWidth(200);
        for (int mask : kCombos) box->addItem(comboLabel(mask, fightDefault(c)), mask);
        box->setToolTip(tr("What %1 presses in a fight").arg(controlLabel(c)));
        connect(box, &QComboBox::currentIndexChanged, this, [this, c, box] {
            const int mask = box->currentData().toInt();
            map_.fight[c] = mask == fightDefault(c) ? -1 : mask;
            store();
            relabel();
            showMap();
            say(tr("In a fight %1 presses %2.").arg(controlLabel(c), comboText(mask)));
        });
        fightBoxes_[c] = box;
        fightGrid->addWidget(ui::label(controlLabel(c)), row, col);
        fightGrid->addWidget(box, row, col + 1);
    }
    fightGrid->setColumnMinimumWidth(2, 24);
    fightGrid->setColumnStretch(5, 1);
    fv->addLayout(fightGrid);
    auto* resetFight = new QPushButton(tr("Default fight buttons"));
    auto* fightButtonsRow = new QHBoxLayout;
    fightButtonsRow->addWidget(resetFight);
    fightButtonsRow->addStretch();
    fv->addLayout(fightButtonsRow);
    root->addWidget(fightBox);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    // keyboard: each Xbox button and its keys
    auto* keyBox = new QGroupBox(tr("KEYBOARD"));
    auto* kv = new QVBoxLayout(keyBox);
    keyboard_ = new QCheckBox(tr("Play with the keyboard as player 1 (while the game's window is in front)"));
    kv->addWidget(keyboard_);
    auto* keyGrid = new QGridLayout;
    keyGrid->setHorizontalSpacing(10);
    keyGrid->setVerticalSpacing(4);
    keyGrid->addWidget(ui::label(tr("In the game (Xbox button)"), "Muted"), 0, 0);
    keyGrid->addWidget(ui::label(tr("Key"), "Muted"), 0, 1);
    keyGrid->addWidget(ui::label(tr("Second key"), "Muted"), 0, 2);
    for (int row = 0; row < Count; ++row) {
        const int c = kOrder[row];
        names_[c] = new QLabel;
        keyGrid->addWidget(names_[c], row + 1, 0);
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            auto* b = new QPushButton;
            b->setMinimumWidth(130);
            b->setCursor(Qt::PointingHandCursor);
            connect(b, &QPushButton::clicked, this, [this, c, slot] { pickKey(c, slot); });
            keyButtons_[c][slot] = b;
            keyGrid->addWidget(b, row + 1, slot + 1);
        }
    }
    keyGrid->setColumnStretch(0, 1);
    kv->addLayout(keyGrid);
    auto* keyNote = ui::label(tr("Click a key to change it. A key does one thing: giving it to another button takes it "
                                 "from the one that had it."), "Muted");
    keyNote->setWordWrap(true);
    kv->addWidget(keyNote);
    auto* resetKeys = new QPushButton(tr("Default keys"));
    auto* keyButtonsRow = new QHBoxLayout;
    keyButtonsRow->addWidget(resetKeys);
    keyButtonsRow->addStretch();
    kv->addLayout(keyButtonsRow);
    kv->addStretch();
    columns->addWidget(keyBox, 1);

    // controllers: each button of a PC controller and what it presses
    auto* padBox = new QGroupBox(tr("CONTROLLER"));
    auto* pv = new QVBoxLayout(padBox);
    auto* padNote = ui::label(tr("What each button of your controller presses, for every controller plugged in: Xbox, "
                                 "PlayStation and Switch controllers, their buttons named by where they sit."), "Muted");
    padNote->setWordWrap(true);
    pv->addWidget(padNote);
    auto* padGrid = new QGridLayout;
    padGrid->setHorizontalSpacing(10);
    padGrid->setVerticalSpacing(4);
    padGrid->addWidget(ui::label(tr("Your controller"), "Muted"), 0, 0);
    padGrid->addWidget(ui::label(tr("Presses"), "Muted"), 0, 1);
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i) {
        auto* button = new QLabel(all[i].label);
        button->setToolTip(all[i].hint);
        auto* box = new QComboBox;
        box->setMinimumWidth(170);
        box->addItem(tr("Nothing"), -1);
        for (int c : kOrder) box->addItem(name(c), c);
        box->setToolTip(all[i].hint);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i, box] {
            map_.presses[i] = box->currentData().toInt();
            store();
            showMap();
        });
        pressBoxes_ << box;
        padGrid->addWidget(button, i + 1, 0);
        padGrid->addWidget(box, i + 1, 1);
    }
    padGrid->setColumnStretch(0, 1);
    pv->addLayout(padGrid);
    unpressed_ = ui::label(QString(), "Warning");
    unpressed_->setWordWrap(true);
    pv->addWidget(unpressed_);
    auto* resetPad = new QPushButton(tr("Default buttons"));
    auto* padButtonsRow = new QHBoxLayout;
    padButtonsRow->addWidget(resetPad);
    padButtonsRow->addStretch();
    pv->addLayout(padButtonsRow);
    pv->addStretch();
    columns->addWidget(padBox, 1);

    auto* game = ui::label(tr("Your keys and your controller's buttons press the Xbox 360 controller's buttons named "
                              "above; IN A FIGHT says what those do in a fight."), "Muted");
    game->setWordWrap(true);
    root->addWidget(game);
    status_ = ui::label(QString(), "Muted");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(status_);
    root->addStretch();

    connect(keyboard_, &QCheckBox::toggled, this, [this](bool on) {
        map_.keyboard = on;
        store();
        showMap();
    });
    connect(resetKeys, &QPushButton::clicked, this, [this] {
        const Map d = defaults();
        map_.keys = d.keys;
        map_.keyboard = true;
        store();
        showMap();
        say(tr("The keyboard has its default keys again."));
    });
    connect(resetPad, &QPushButton::clicked, this, [this] {
        map_.presses = defaults().presses;
        store();
        showMap();
        say(tr("The controller has its default buttons again."));
    });
    connect(resetFight, &QPushButton::clicked, this, [this] {
        map_.fight.fill(-1);
        store();
        relabel();
        showMap();
        say(tr("Every button presses its default in a fight again."));
    });

    map_ = load();
    relabel();
    showMap();
}

void ControlsPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    relabel();
    showMap();
}

void ControlsPage::relabel() {
    using namespace controls;
    for (int c = 0; c < Count; ++c) {
        names_[c]->setText(name(c));
        for (QPushButton* b : keyButtons_[c]) b->setToolTip(tr("Click, then press the key for %1").arg(name(c)));
    }
    for (QComboBox* box : std::as_const(pressBoxes_)) {
        const QSignalBlocker block(box);
        for (int c = 0; c < Count; ++c) box->setItemText(box->findData(c), name(c));
    }
}

void ControlsPage::showEvent(QShowEvent* event) {
    Page::showEvent(event);
    map_ = controls::load();  // someone may have edited the file
    relabel();
    showMap();
    say(tr("Kept in %1").arg(QDir::toNativeSeparators(controls::file())));
}

void ControlsPage::showMap() {
    using namespace controls;
    {
        const QSignalBlocker block(keyboard_);
        keyboard_->setChecked(map_.keyboard);
    }
    for (int c = 0; c < Count; ++c)
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            QPushButton* b = keyButtons_[c][slot];
            b->setText(keyText(map_.keys[c], slot));
            // the second key once there is a first
            b->setEnabled(map_.keyboard && slot <= map_.keys[c].size());
        }
    QStringList unpressed;
    for (int c : kOrder)
        if (!map_.presses.contains(c)) unpressed << name(c);
    for (int i = 0; i < pressBoxes_.size(); ++i) {
        const QSignalBlocker block(pressBoxes_[i]);
        pressBoxes_[i]->setCurrentIndex(qMax(0, pressBoxes_[i]->findData(map_.presses.value(i, -1))));
    }
    for (int c = 0; c < Count; ++c) {
        if (!fightBoxes_[c]) continue;
        const QSignalBlocker block(fightBoxes_[c]);
        fightBoxes_[c]->setCurrentIndex(qMax(0, fightBoxes_[c]->findData(map_.fightOf(c))));
    }
    unpressed_->setText(unpressed.isEmpty() ? QString()
                                            : tr("No button of the controller presses %1.").arg(unpressed.join(QStringLiteral(", "))));
    unpressed_->setVisible(!unpressed.isEmpty());
}

void ControlsPage::pickKey(int control, int slot) {
    using namespace controls;
    const QList<int>& before = map_.keys[control];
    KeyDialog dlg(name(control), slot < before.size() ? keyLabel(before[slot]) : QString(), this);
    winutil::useDarkTitleBar(&dlg);
    const int result = dlg.exec();
    QList<int>& keys = map_.keys[control];
    if (result == KeyDialog::kNoKey) {
        if (slot < keys.size()) keys.removeAt(slot);
        store();
        showMap();
        say(tr("%1 has one key less.").arg(name(control)));
        return;
    }
    if (result != QDialog::Accepted) return;
    const int vk = dlg.key();
    QString tookFrom;
    for (int c = 0; c < Count; ++c)
        if (c != control && map_.keys[c].removeAll(vk) > 0) tookFrom = name(c);
    const int at = qMin(slot, int(keys.size()));
    if (at < keys.size()) keys[at] = vk;
    else keys.append(vk);
    for (int i = 0; i < keys.size(); ++i)  // it was this button's other key
        if (i != at && keys[i] == vk) {
            keys.removeAt(i);
            break;
        }
    store();
    showMap();
    if (tookFrom.isEmpty()) say(tr("%1 presses %2.").arg(keyLabel(vk), name(control)));
    else say(tr("%1 presses %2 now, no longer %3.").arg(keyLabel(vk), name(control), tookFrom));
}

void ControlsPage::store() {
    QString error;
    if (!controls::save(map_, &error)) say(error, true);
}

void ControlsPage::say(const QString& text, bool warn) {
    status_->setObjectName(warn ? QStringLiteral("Warning") : QStringLiteral("Muted"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
    status_->setText(text);
}
