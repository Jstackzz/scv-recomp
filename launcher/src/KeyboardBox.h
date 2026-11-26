#pragma once
// Settings, KEYBOARD: which keys press each button of the Xbox 360 controller, for a player without a controller
// (Controls.h, controls.toml, docs/PROTOCOL.md 3.1). What the buttons do is set in the game's own Options; before 0.1.8
// a Controls page had this, the PC controller's buttons and the IN A FIGHT button combinations as well.

#include <QGroupBox>

#include <array>

#include "Controls.h"

class QCheckBox;
class QLabel;
class QPushButton;

class KeyboardBox : public QGroupBox {
    Q_OBJECT
public:
    explicit KeyboardBox(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    QString name(int control) const;
    void pickKey(int control, int slot);
    void showMap();  // the widgets from map_
    void store();    // map_ into the file the game reads
    void say(const QString& text, bool warn = false);

    controls::Map map_;
    QCheckBox* keyboard_ = nullptr;
    std::array<std::array<QPushButton*, controls::kKeysPerButton>, controls::Count> keyButtons_{};
    QLabel* status_ = nullptr;
};
