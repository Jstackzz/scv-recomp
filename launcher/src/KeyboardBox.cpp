#include "KeyboardBox.h"

#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "Widgets.h"
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
        setMinimumWidth(420);
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

using namespace controls;
// the buttons in the order a player thinks of them: moving, the four (horizontal, vertical, kick, guard), the
// shortcuts, the rest; two columns of eight
constexpr Control kOrder[] = {Up, Down, Left, Right, X, Y, B, A, RB, RT, LB, LT, Start, Back, LS, RS};
static_assert(std::size(kOrder) == Count);

}  // namespace

KeyboardBox::KeyboardBox(QWidget* parent) : QGroupBox(tr("KEYBOARD"), parent) {
    auto* v = new QVBoxLayout(this);
    v->setSpacing(8);
    auto* note = ui::label(tr("For playing without a controller. Each key presses a button of the Xbox 360 controller; "
                              "what the buttons do is set in the game's own Options."), "Muted");
    note->setWordWrap(true);
    v->addWidget(note);
    keyboard_ = new QCheckBox(tr("Play with the keyboard (while the game's window is in front)"));
    v->addWidget(keyboard_);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    constexpr int kRows = Count / 2;
    for (int i = 0; i < Count; ++i) {
        const int c = kOrder[i];
        const int row = i % kRows, col = (i / kRows) * 4;
        grid->addWidget(ui::label(name(c)), row, col);
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            auto* b = new QPushButton;
            b->setMinimumWidth(110);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tr("Click, then press the key for %1").arg(name(c)));
            b->setAccessibleName(slot == 0 ? tr("Key for %1").arg(name(c)) : tr("Second key for %1").arg(name(c)));
            connect(b, &QPushButton::clicked, this, [this, c, slot] { pickKey(c, slot); });
            keyButtons_[c][slot] = b;
            grid->addWidget(b, row, col + 1 + slot);
        }
    }
    grid->setColumnMinimumWidth(3, 28);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(4, 1);
    v->addLayout(grid);
    auto* row = new QHBoxLayout;
    auto* reset = new QPushButton(tr("Default keys"));
    row->addWidget(reset);
    status_ = ui::label(QString(), "Muted");
    status_->setWordWrap(true);
    row->addWidget(status_, 1);
    v->addLayout(row);

    connect(keyboard_, &QCheckBox::toggled, this, [this](bool on) {
        map_.keyboard = on;
        store();
        showMap();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        const Map d = defaults();
        map_.keys = d.keys;
        map_.keyboard = true;
        store();
        showMap();
        say(tr("The keyboard has its default keys again."));
    });

    map_ = load();
    // the IN A FIGHT button combinations of the Controls page before 0.1.8 would still change what the buttons do,
    // where nobody could see them any more: the game's own Options set that now
    bool hadFight = false;
    for (int& f : map_.fight) {
        hadFight = hadFight || f >= 0;
        f = -1;
    }
    if (hadFight) {
        applog::write(QStringLiteral("controls: the IN A FIGHT button combinations taken out of %1 (the game's own "
                                     "Options set the buttons)").arg(QDir::toNativeSeparators(controls::file())));
        store();
    }
    showMap();
}

QString KeyboardBox::name(int control) const { return controls::controlText(control, QStringLiteral("sc5")); }

void KeyboardBox::showEvent(QShowEvent* event) {
    QGroupBox::showEvent(event);
    map_ = controls::load();  // someone may have edited the file
    map_.fight.fill(-1);
    showMap();
}

void KeyboardBox::showMap() {
    {
        const QSignalBlocker block(keyboard_);
        keyboard_->setChecked(map_.keyboard);
    }
    for (int c = 0; c < Count; ++c)
        for (int slot = 0; slot < kKeysPerButton; ++slot) {
            QPushButton* b = keyButtons_[c][slot];
            b->setText(slot < map_.keys[c].size() ? keyLabel(map_.keys[c][slot]) : QStringLiteral("—"));
            b->setEnabled(map_.keyboard && slot <= map_.keys[c].size());  // the second key once there is a first
        }
}

void KeyboardBox::pickKey(int control, int slot) {
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

void KeyboardBox::store() {
    QString error;
    if (!controls::save(map_, &error)) say(error, true);
}

void KeyboardBox::say(const QString& text, bool warn) {
    status_->setObjectName(warn ? QStringLiteral("Warning") : QStringLiteral("Muted"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
    status_->setText(text);
}
