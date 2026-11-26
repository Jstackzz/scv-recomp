#pragma once
// One game process and its session folder: environment, cmd.txt (launcher -> game) and
// status.txt (game -> launcher), docs/PROTOCOL.md section 3.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

class GameProcess : public QObject {
    Q_OBJECT
public:
    GameProcess(const QString& gameId, int instance, QObject* parent = nullptr);
    ~GameProcess() override;

    // Starts `exe` (a .py file is run with the configured Python, for the development stand-in).
    // `netMode` false starts it plainly (first-launch compile, single player), without SC5_NET.
    bool start(const QString& exe, QString* error, bool netMode = true);
    // one more environment variable for the next start (SC5_TRAINING for practice while a search waits)
    void setEnv(const QString& name, const QString& value) { extraEnv_.insert(name, value); }
    // more command-line arguments for the next start (--prepare)
    void setArgs(const QStringList& args) { extraArgs_ = args; }
    // the player's own game (PLAY, docs/PROTOCOL.md 3.8): SC5_NET=1 and SC5_MENUS=1 on the player's own save, the
    // game's online menus talking to the launcher through ui.json and ask.txt in the session folder
    void setMenus(bool on) { menus_ = on; }
    bool menus() const { return menus_; }
    // "quit" command (a plain game: its window is closed, as the player would), then kill after 5 s
    void quit();
    void kill();
    bool isRunning() const;
    qint64 pid() const;  // 0 when not running

    QString gameId() const { return gameId_; }
    int instance() const { return instance_; }
    QString sessionDir() const { return sessionDir_; }
    QString status() const { return status_; }  // first line of status.txt
    int exitCode() const { return exitCode_; }  // the process's, once it has ended
    QString state() const;                     // first word of it
    bool isReady() const { return state() == QLatin1String("ready"); }
    // the player's own game (3.8) is elsewhere than its online menus (single player, Creation, Training ...): it takes
    // no start until it says ready again
    bool isAway() const { return state() == QLatin1String("away"); }
    // between matches as a game should be: coming up (just started, booting), at its hold point, or the player's own
    // game away from its online menus (it left the match: the player went on to something else). Anything else while
    // nothing of the launcher's runs on it (a match's result, a desync, in_match) is a game that never came back from
    // its last match
    bool betweenMatches() const {
        return status_ == QLatin1String("starting") || state() == QLatin1String("booting") || isReady() || isAway();
    }

    // Write one command line; if the previous command has not been taken yet it waits up to 2 s
    // (a newer command replaces a waiting one).
    void command(const QString& line);

    // status.txt read now, not at the next tick: what the game wrote before an ask of its menus (a match's result,
    // then leaving the lobby) is taken first (docs/PROTOCOL.md 3.8)
    void readStatusNow() { poll(); }

    static QHash<QString, QString> parseKeys(const QString& line);  // key=value tokens after the verb

    // what to tell a player whose Windows blocks the games (`blocked`: it just did): Smart App Control and how to
    // switch it off
    static QString smartAppControlHelp(bool blocked = true);
    // what to tell a player whose antivirus took a game's file (`file`, its name): a false alarm, as a new release's
    // files and the code each PC builds for itself are new to it, and where Windows Security gives it back
    static QString antivirusHelp(const QString& file);
    // a game built before (its code DLL beside it) whose code no longer matches its executable, as after an update:
    // its next start builds the code again (runtime app/game_code.cpp, compiler\work\<exe>\built.txt)
    static bool codeStale(const QString& exe);
    // the game's extracted default.xex (Build game), in the folder the game reads (SC5_GAME_DIR); empty before then
    static QString xexFile(const QString& gameId);
    // a game built from another release than the package's (games\sc5\built.txt, which build-game.cmd copies from
    // project\VERSION, differs from project\VERSION): an update changed the game, and it must be built again
    static bool needsRebuild(const QString& gameId);
    // what is wrong with the title update in the game folder (disc\default.xexp, update\patch.cpk) for this release,
    // for the player: title update 1 where SCV Online now needs title update 3, say. Empty when it is the right one,
    // and when there is no default.xexp at all (nothing extracted yet: the game's files are simply not there)
    static QString updateProblem(const QString& gameId);
    // what the player does about it: "Open Build game, choose title update 3 in step 2 ..."
    static QString updateFix(const QString& gameId);

signals:
    void statusChanged(const QString& line);
    void outputLine(const QString& line);
    void exited(int exitCode, bool crashed);

private:
    void poll();
    void writeCommand(const QString& line);

    QString gameId_;
    int instance_;
    QProcess* proc_ = nullptr;
    QTimer poll_;
    QString sessionDir_, cmdPath_, statusPath_;
    QString status_;
    QString pendingCmd_;
    QElapsedTimer lastWrite_;
    bool netMode_ = true;
    bool menus_ = false;
    bool quitting_ = false;
    int exitCode_ = 0;
    QByteArray outBuf_;
    QHash<QString, QString> extraEnv_;
    QStringList extraArgs_;
};
