#pragma once
// The launcher window (SCV 0.1.8): the header, four pages (Play, Build game, Settings, Support) and a footer. Everything
// online is in the game's own menus (docs/PROTOCOL.md 3.8): the window builds the game, starts it (PLAY), signs the
// player in and offers updates; the connection, the matchmaking, the lobbies and the matches run in the background
// (ServerClient, MatchController) for the game, through MenuBridge.

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include "TestDriver.h"

class QButtonGroup;
class QLabel;
class QPushButton;
class QStackedWidget;
class BuildPage;
class GamepadNav;
class MatchController;
class MenuBridge;
class Page;
class PlayPage;
class ServerClient;
class SettingsPage;
class UpdateDialog;
class Updater;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const LaunchOptions& opts, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildHeader();
    QWidget* buildNav();
    QWidget* buildFooter();
    void selectPage(int index);
    void connectServer();
    void updateFooter();
    void play();  // PLAY: the player's own game, or its window to the front when it runs
    void openAccountDialog(bool newAccount);
    void askForAccount();
    void warnSmartAppControl();  // it blocks the games: what it is and where to switch it off
    void checkGameBuilt();       // the Play page says when the game must be built (again) first
    void onServerMessage(const QJsonObject& msg);
    void popUp(QWidget* dialog);  // in front, the launcher's window restored first when minimized
    void focusGame();             // the game's window in front: the player is back in it
    void showNotice(const QString& text, bool warn);
    // a release newer than this launcher (welcome, an update push, or the reply turning away a launcher too old)
    void offerUpdate(const QJsonValue& release, bool required);
    void showUpdateDialog(bool required);
    void startUpdate();
    void installUpdateWhenFree();
    // what keeps the game's files in use (an update waits for it: no install under a running game), as the end of
    // "once ..."; empty when nothing does
    QString gamesBusy() const;
    void snap(const QString& name);

    LaunchOptions opts_;
    ServerClient* server_;
    MatchController* controller_;
    MenuBridge* bridge_;
    TestDriver* driver_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QList<Page*> pages_;
    PlayPage* play_ = nullptr;
    SettingsPage* settings_ = nullptr;
    BuildPage* build_ = nullptr;
    class UpnpMapper* upnp_ = nullptr;
    QButtonGroup* navGroup_ = nullptr;
    QLabel* notice_ = nullptr;
    QLabel* connDot_ = nullptr;
    QLabel* connText_ = nullptr;
    QTimer noticeTimer_;
    GamepadNav* pad_ = nullptr;  // the launcher with a controller
    QString lastError_;
    QString playError_;  // why PLAY could not start the game, last (cleared once it runs)
    Updater* updater_ = nullptr;
    QPointer<UpdateDialog> updateDialog_;
    QJsonObject update_;                   // the release offered
    QString updateDismissed_;              // the version LATER was chosen for this run
    bool installPending_ = false;          // downloaded, waiting for the game to be closed
    bool offerPending_ = false;            // offered while the game runs: its window comes once it is closed
    bool offerRequired_ = false;
    bool askedForAccount_ = false;  // the account window opened by itself this run
    bool darkApplied_ = false;
};
