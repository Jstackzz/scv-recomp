#pragma once
// The launcher's test switches (docs/LAUNCHER.md, "Command line"): --user signs in, and --queue, --lobby, --auto-accept,
// --rematches and the rest play the player's part as the game's own menus would, through MenuBridge::ask (the asks of
// docs/PROTOCOL.md 3.8), so tools\online-e2e.ps1 runs the same code a player's game drives. With any of them (or
// --play) the player's game is started first, as PLAY does.

#include <QJsonObject>
#include <QObject>
#include <QStringList>

class QTimer;
class MatchController;
class MenuBridge;
class ServerClient;

struct LaunchOptions {
    QString server;
    QString fingerprint;
    QString user;
    QString passwordEnv = QStringLiteral("SC5_DEV_PASSWORD");
    bool registerAccount = false;
    bool play = false;       // start the game once the window is up, as PLAY does
    QString queue;           // "casual" or "ranked"
    QString game;
    QString mode;
    QString character;       // the roster id searches and lobbies go with ("" or "-": picked in the game)
    QString page;
    int autoAcceptSeconds = -1;
    int cancelSearchSeconds = -1;  // --queue: cancel the search after this many seconds
    bool localTest = false;  // start the two-instance loopback test right away
    QString extractImage;    // open "Build game" and extract this disc image right away
    QString lobby;           // "host": create a lobby and ready; "join": join it by name and ready; "watch"
    QString lobbyName;
    QString lobbyCodeFile;   // the host writes its lobby's code here, a joiner reads it (lobbies the list leaves out)
    int lobbyMatches = 1;    // matches to play there, one after the other
    bool leaveLobby = false; // once they are played, leave the lobby
    int leaveLobbyInMatch = 0;  // seconds into its first match: leave the lobby then
    bool koth = false;       // --lobby host: a King of the Hill lobby (join: one that is full puts you in line)
    int rate = 0;            // King of the Hill: rate each match watched, 1 to 10 (0: never)
    QString snapDir;         // development: a picture of the --page once the window is up
    int rematches = 0;       // --queue casual: rematches to choose after the first match
    QStringList rematchChoices;  // what each of them chooses, "rematch" or "select" (the last one repeats)
    bool leaveSeries = false;    // once those are chosen, leave the series at once instead of letting it run out
    bool lobbyOwnFirst = false;  // --lobby join: a lobby of one's own first, left by joining the named one
    bool upnpProbe = false;  // find the router and its public address, open nothing
    QString updatedFrom;     // --updated: the version this launcher just replaced (Updater)
    bool autoUpdate = false; // development: an update offered installs at once, without asking
    int dropAtResult = 0;    // seconds after the game's first wait at its result screen: the server connection drops
};

class TestDriver : public QObject {
    Q_OBJECT
public:
    TestDriver(const LaunchOptions& opts, ServerClient* server, MatchController* controller, MenuBridge* bridge,
               QObject* parent = nullptr);
    bool active() const;  // any switch that plays a part

private:
    void login();
    void startGame();  // the player's game, as PLAY: then the search or the lobby once it is up
    void begin();
    void beginLobby();
    void join(const QString& code, bool watch);
    void onServerMessage(const QJsonObject& msg);
    void onLobby(const QJsonObject& msg);
    void say(const QString& text);
    void ask(const QString& line);
    static QString enc(const QString& text);  // a text value of an ask, percent-encoded (3.8)

    LaunchOptions opts_;
    ServerClient* server_;
    MatchController* controller_;
    MenuBridge* bridge_;
    int loginId_ = -1;
    bool triedRegister_ = false;
    bool begun_ = false, gameAsked_ = false;
    int rematchesChosen_ = 0;
    bool lobbyBegun_ = false, lobbyJoined_ = false, lobbyInMatch_ = false, lobbyLeft_ = false;
    bool ownLobbyIn_ = false, codeWritten_ = false;
    int lobbyDone_ = 0, readyFor_ = -1;
    QTimer* poll_ = nullptr;
    QJsonObject lastLobby_;
    QString ratedMatch_;
    bool dropped_ = false;  // --drop-at-result done: the login token signs in again, not the password
};
