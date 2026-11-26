#pragma once
// The launcher's side of the game's own online menus (docs/PROTOCOL.md 3.8, SCV 0.1.8). The player does everything
// online in the game PLAY started; the launcher keeps the connection, the matchmaking, the lobbies and the matches in
// the background, and this class is where the two meet:
//
//  - ui.json: the launcher's state for the game's menus (the account, the server's counts, the search, the series, the
//    lobbies and the lobby the player is in, the leaderboard, a message, an update), written into the game's session
//    folder whenever it changes and once a second;
//  - ask.txt: what the player chose in those menus, one line at a time, taken (renamed to ask.txt.taken) and done
//    through the code the launcher's own pages called before 0.1.8: the match controller, and the lobby and leaderboard
//    messages to the server.
//
// Every ask is safe in any state: one that does not fit (a search while searching, Ready outside a lobby) is left
// undone, and ui.json's message says why in one plain sentence. The launcher's test switches (--queue, --lobby, ...)
// stand in for the game's menus through ask() too.

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class MatchController;
class ServerClient;

class MenuBridge : public QObject {
    Q_OBJECT
public:
    MenuBridge(ServerClient* server, MatchController* controller, QObject* parent = nullptr);

    // One ask, as the game writes it ("ask n=3 queue kind=casual"; the "ask n=<count>" part may be left out, as the
    // test switches do). Done at once, or refused with a message.
    void ask(const QString& line);
    // The series' next match chosen at the result screen: a casual "rematch" or "select". The game's result screen
    // makes this choice through status.txt (3.3, the loser's choice); the test switches (--rematches) stand in here.
    void chooseNext(const QString& choice);

    // what ui.json says now (the Play page shows the account and the search from it too)
    QJsonObject state() const;
    // the player's ranked and casual record in versus (the server's profile stats), {} before signing in
    QJsonObject versusStats() const;
    QJsonObject lobby() const { return lobby_; }  // the server's state of the lobby the player is in, {} none
    QJsonArray lobbies() const { return lobbies_; }  // the last list of open lobbies
    // the release the server offers ({} none): ui.json's update, so the game says so too
    void setUpdate(const QString& version, const QString& text);
    // ui.json's message: shown once by the game (a new id each time)
    void showMessage(const QString& text, bool error = false);
    // development (--character): the roster id the test switches search and join with ("" picks in the game, the way
    // the game's own menus always do)
    void setTestCharacter(const QString& id) { testCharacter_ = id; }

signals:
    void changed();  // something in state() changed
    void lobbiesChanged(const QJsonArray& lobbies);

private:
    void onServerMessage(const QJsonObject& msg);
    void refuse(const QString& verb, const QString& why);
    void markDirty();
    void writeUi();
    void pollAsk();
    QString sessionDir() const;  // the player's own game's, empty while it does not run
    void onGameEnded();          // the player's game closed: out of the search, the set or series and the lobby

    // the asks (3.8's table)
    void askQueue(const QHash<QString, QString>& keys);
    void askCancel();
    void askAccept(bool accept);
    void askLeave();
    void askSide(const QString& value);
    void askLobbies();
    void askLobbyCreate(const QHash<QString, QString>& keys);
    void askLobbyJoin(const QHash<QString, QString>& keys);
    void askLobbyReady(bool ready);
    void askLobbyLeave();
    void askLeaderboard();
    // a lobby create or join on its way: a search running is cancelled first (the server takes no lobby from a player
    // who searches); kept for an older server that turns away a player still in a lobby (busy): left, then asked again
    void sendJoin(const QJsonObject& msg);
    void leftLobby();

    QJsonObject accountJson() const;
    QJsonObject queueJson() const;
    QJsonObject seriesJson() const;
    QJsonObject lobbyJson() const;
    QString seriesText() const;
    QString seriesOverText(const QJsonObject& s) const;
    QString lobbyText() const;
    QJsonObject me() const;  // this player's entry in the lobby's members
    QString lobbyOpponent() const;
    int lobbyWinsOf(const QString& name) const;  // a member's wins in the lobby: the server's, else the ones seen here
    void updateVersionProblem();  // the lobby's players whose launchers cannot play this one's (their versions)

    ServerClient* server_;
    MatchController* controller_;
    QTimer askTimer_, writeTimer_, dirtyTimer_, listTimer_;
    QString lastDir_;          // the session folder ui.json went to last (another one: the game started again)
    bool gameWasRunning_ = false;
    int asksTaken_ = 0;
    QString testCharacter_;

    QJsonObject online_;       // the server's last online counts
    QString notice_;           // the server's last notice or message of the day, for a while
    QElapsedTimer noticeClock_;
    QJsonObject stats_;        // the player's profile stats
    QString accountError_;     // why the connection or the sign-in failed, last
    // the match found: accepted by this player, the other's address come (match_peer), since when
    bool accepted_ = false, peerSeen_ = false;
    QElapsedTimer foundClock_;
    int acceptTimeout_ = 0;
    QJsonObject series_;       // the server's series this player is in ({} none)
    QJsonObject seriesOver_;   // the last series that ended, shown as over for a while
    QElapsedTimer seriesOverClock_;
    QJsonArray lobbies_;
    qint64 lobbiesAt_ = 0;
    QElapsedTimer listWanted_;  // the game asked for the list lately: it is kept current meanwhile
    QJsonObject lobby_;
    QHash<QString, int> lobbyWins_;  // the lobby's matches this player played or watched, won by name
    QHash<QString, QStringList> watchedSides_;  // a watched match's players, side 1 then side 2
    QString lobbyPassword_;    // a private lobby this launcher created: its password
    QString pendingPassword_;  // the one the lobby_create on its way asked for
    QString versionProblem_;   // who in the lobby cannot play this player, and why ("" none)
    bool versionBlocked_ = false;
    QJsonObject lastJoin_;
    bool retriedJoin_ = false, switching_ = false;
    QTimer rejoinGrace_;       // a spectator's dropped connection: back in the lobby within this, or watching stops
    bool watchingHere_ = false;
    bool gameAway_ = false;    // the game said `away` last (its online menus left): a lobby's Ready taken back once
    QJsonArray board_;
    qint64 boardAt_ = 0;
    QJsonObject message_{{QStringLiteral("id"), 0}, {QStringLiteral("kind"), QStringLiteral("info")},
                         {QStringLiteral("text"), QString()}};
    QJsonObject update_{{QStringLiteral("version"), QString()}, {QStringLiteral("text"), QString()}};
};
