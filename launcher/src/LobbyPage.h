#pragma once
// Lobby browser and the current lobby: members, character, ready (both players ready starts the match after a 3-2-1
// countdown), chat, and spectators: members who watch the lobby's matches in their own game instead of playing them
// (docs/PROTOCOL.md 1.10.1). King of the Hill lobbies (1.10.2): the winner stays on as the king, the loser goes to the
// back of the line and the next in line plays the king; everyone else watches and rates each win from 1 to 10, the
// winner's Respect Points.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include "Widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QStackedWidget;
class QTimer;
class MatchController;
class ServerClient;

class LobbyPage : public Page {
    Q_OBJECT
public:
    LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void refresh();
    bool inLobby() const { return !lobby_.isEmpty(); }
    // a controller's START on this page presses Ready (a player's, not a spectator's); after a lobby match the focus
    // goes back to it, so A readies up for the next one (King of the Hill: to the rating first, while it is open)
    void pressReady();
    void focusReady();
    // join a lobby by its code as the Join code button does (development: --lobby-own-first)
    void joinByCode(const QString& code, bool spectate) { join(code, false, spectate); }
    // the lobby's mode is lobby-only (Online Practice): its next match comes by itself, nothing to ready up for
    bool lobbyOnly() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void onMessage(const QJsonObject& msg);
    void showLobby(const QJsonObject& lobby);
    void updateControls();  // what this member may do now: the lobby's state and this launcher's game
    void updateStatus();  // the countdown, MATCH IN PROGRESS, or how the next match starts
    QString kothText(qint64 now) const;  // King of the Hill: where this member stands
    void updateRating(qint64 now);  // the rating row, while this spectator may rate the last match's winner
    void rate(int score);
    void leftLobby(bool keepWatching = false);
    void join(const QString& code, bool locked, bool spectate = false);
    // a join or create on its way: a search running is cancelled first (the server takes no lobby from a player who
    // searches), and the request is kept for an older server that turns away a player still in a lobby (busy): this
    // one leaves it and asks again
    void sendJoin(const QJsonObject& msg);
    void showAlert(const QString& text);  // over the list, large: a join turned away, and why
    void updateVersionAlert();  // the lobby's players whose launchers cannot play this one's (versions)
    bool spectating() const;  // this player watches in the lobby they are in
    QJsonObject me() const;   // this player's entry in the lobby's members
    bool koth() const;
    void createLobby();
    void sendChat();
    void appendChat(const QString& from, const QString& text, double ts);
    QString savedCharacter(const QString& game, const QString& mode) const;

    ServerClient* server_;
    MatchController* controller_;
    QTableWidget* list_;
    QLineEdit* code_;
    QLabel* listInfo_;
    QLabel* listAlert_;     // a join or create turned away, and why (hidden otherwise)
    QLabel* versionAlert_;  // in the lobby: who cannot play whom, by their launchers' versions (hidden otherwise)
    QTimer* listTimer_;     // the list again every few seconds while the page shows
    QJsonArray lastList_;   // what it showed last (the same again: the table is left alone, its selection kept)
    QJsonObject lastJoin_;  // the last lobby_join or lobby_create sent
    bool retriedJoin_ = false;  // sent again after leaving the lobby for an older server
    bool switching_ = false;    // that leave is on its way: its lobby_left is a move to another lobby
    bool versionBlocked_ = false;  // the other player's launcher is another version: Ready stays off
    QStackedWidget* right_;
    QLabel* lobbyTitle_;
    QLabel* lobbyInfo_;
    QTableWidget* members_;
    QComboBox* character_;
    QPushButton* ready_;
    QLabel* countdown_;  // STARTING IN 3, 2, 1 / MATCH IN PROGRESS
    QLabel* nextInfo_;   // how the next match starts, when nothing counts down
    QWidget* ratePanel_;  // King of the Hill: rate the last match's winner, 1 to 10
    QLabel* rateLabel_;
    QList<QPushButton*> rateButtons_;
    QString ratedMatch_;  // the match this player sent a rating for
    qint64 rateEndsMs_ = 0;  // when the rating closes (the lobby state's rating.seconds), 0: none
    qint64 readyByMs_ = 0;   // King of the Hill: when the two players must have readied, 0: no such time
    QTimer* tick_;
    qint64 startsAtMs_ = 0;  // when the match starts by itself (the lobby state's starts_in), 0: no countdown
    QPushButton* leave_;
    QPushButton* role_;  // PLAY (a spectator takes a free place) / WATCH (a player steps aside); King of the Hill:
    QString roleAction_;  // GET IN LINE / LEAVE LINE too: the role it asks for
    LogView* chat_;
    QLineEdit* chatInput_;
    QJsonObject lobby_;
    QTimer* rejoinGrace_;  // a spectator's dropped connection: back in the lobby within this, or the watching stops
    bool watchingHere_ = false;  // a spectator in the lobby shown (kept: the account's name is gone with the connection)
};
