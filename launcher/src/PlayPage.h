#pragma once
// The launcher's start page (SCV 0.1.8): the player's account (sign in, create one, the name, rank and record), one
// big PLAY button that starts the game (or brings it to the front), and in plain words how the game and the connection
// are doing, with the update offer. Everything online happens in the game's own menus (docs/PROTOCOL.md 3.8); the
// launcher keeps the connection and does what those menus ask in the background.

#include <QTimer>

#include "Widgets.h"

class QLabel;
class QPushButton;
class MatchController;
class MenuBridge;
class ServerClient;

class PlayPage : public Page {
    Q_OBJECT
public:
    PlayPage(ServerClient* server, MatchController* controller, MenuBridge* bridge, QWidget* parent = nullptr);
    // the update the server offers: the button's text and a sentence ("" hides it)
    void setUpdate(const QString& button, const QString& text);
    // what keeps the game from starting (the game not built, the wrong title update, PLAY's error), large over the
    // page with a button that opens Build game when that helps ("" hides it)
    void setProblem(const QString& text, bool offerBuild = false);
    void pressPlay();  // a controller's START: PLAY as a click would
    void focusPlay();

signals:
    void playClicked();
    void signInClicked(bool newAccount);
    void signOutClicked();
    void connectClicked();
    void updateClicked();
    void buildClicked();

private:
    void refresh();

    ServerClient* server_;
    MatchController* controller_;
    MenuBridge* bridge_;
    QLabel* badge_;
    QLabel* name_;
    QLabel* rank_;
    QLabel* record_;
    QPushButton* signIn_;
    QPushButton* create_;
    QPushButton* signOut_;
    QPushButton* connect_;
    QPushButton* play_;
    QLabel* gameLine_;
    QLabel* serverLine_;
    QLabel* onlineLine_;
    QLabel* problem_;
    QPushButton* openBuild_;
    QWidget* updateCard_;
    QLabel* updateText_;
    QPushButton* update_;
    QTimer tick_;
};
