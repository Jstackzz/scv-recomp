#include "TestDriver.h"

#include <QFile>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QTimer>

#include "AppPaths.h"
#include "GameProcess.h"
#include "MatchController.h"
#include "MenuBridge.h"
#include "ServerClient.h"

namespace {
QString s(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
}  // namespace

TestDriver::TestDriver(const LaunchOptions& opts, ServerClient* server, MatchController* controller, MenuBridge* bridge,
                       QObject* parent)
    : QObject(parent), opts_(opts), server_(server), controller_(controller), bridge_(bridge) {
    const QString character = opts_.character == QLatin1String("-") ? QString() : opts_.character;
    bridge_->setTestCharacter(character);
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        if (st == ServerClient::State::Online) login();
    });
    connect(server_, &ServerClient::loggedIn, this, [this] { QTimer::singleShot(300, this, &TestDriver::begin); });
    connect(server_, &ServerClient::message, this, &TestDriver::onServerMessage);
    connect(controller_, &MatchController::gameStatusChanged, this, [this] {
        if (!begun_) QTimer::singleShot(0, this, &TestDriver::begin);
    });
    // --auto-accept: MATCH FOUND answered after that many seconds, as the player would in the game
    connect(controller_, &MatchController::matchFound, this, [this](const QJsonObject& found) {
        if (opts_.autoAcceptSeconds < 0 || !found.value(QStringLiteral("accept_required")).toBool()) return;
        QTimer::singleShot(opts_.autoAcceptSeconds * 1000, this, [this] {
            if (controller_->phase() == MatchController::Phase::Found) ask(QStringLiteral("accept"));
        });
    });
    // --rematches: a casual series' next match, chosen at the result screen as the game's loser would
    connect(controller_, &MatchController::seriesChanged, this, [this](const QJsonObject& series) {
        if (opts_.queue.isEmpty() || series.value(QStringLiteral("ranked")).toBool() ||
            s(series, "state") != QLatin1String("between") || series.value(QStringLiteral("you_ready")).toBool())
            return;
        // the game's own result screen offers the choice (SCV 0.1.8: its loser's REMATCH, CHARACTER SELECT or LEAVE,
        // the stand-in's SC5_FAKE_CHOICE): nothing to stand in for
        // (or made it already: the launcher readies from there)
        if (controller_->choiceOnResultScreen() || controller_->phase() == MatchController::Phase::AtResult) return;
        if (rematchesChosen_ >= opts_.rematches) {
            if (opts_.leaveSeries) {
                say(QStringLiteral("leaving the series"));
                QTimer::singleShot(2000, this, [this] { ask(QStringLiteral("leave")); });
            }
            return;
        }
        const QStringList& choices = opts_.rematchChoices;
        const QString choice = choices.isEmpty() ? QStringLiteral("rematch")
                                                 : choices.value(qMin(rematchesChosen_, int(choices.size()) - 1));
        rematchesChosen_++;
        say(QStringLiteral("rematch %1 of %2 (%3)").arg(rematchesChosen_).arg(opts_.rematches).arg(choice));
        QTimer::singleShot(2000, this, [this, choice] { bridge_->chooseNext(choice); });
    });
    // --lobby: the next match's ready waits for the game to be done with the last one
    connect(controller_, &MatchController::phaseChanged, this, [this](MatchController::Phase p) {
        if (p == MatchController::Phase::Idle && !opts_.lobby.isEmpty() && !lastLobby_.isEmpty()) onLobby(lastLobby_);
        // --drop-at-result: the server connection drops while the game waits at its first result screen (the loser's
        // choice still to come, or made while it is off), as a network would; it comes back by itself
        if (p == MatchController::Phase::AtResult && opts_.dropAtResult > 0 && !dropped_) {
            dropped_ = true;
            QTimer::singleShot(opts_.dropAtResult * 1000, this, [this] {
                say(QStringLiteral("dropping the server connection at the result screen"));
                server_->dropForTest();
            });
        }
    });
    if (active() || opts_.play) QTimer::singleShot(500, this, &TestDriver::startGame);
}

bool TestDriver::active() const { return !opts_.queue.isEmpty() || !opts_.lobby.isEmpty(); }

void TestDriver::say(const QString& text) { applog::write(QStringLiteral("cli: ") + text); }

void TestDriver::ask(const QString& line) {
    say(QStringLiteral("ask %1").arg(line));
    bridge_->ask(line);
}

QString TestDriver::enc(const QString& text) {
    return QString::fromLatin1(text.toUtf8().toPercentEncoding(QByteArray(), QByteArray(), '%'));
}

void TestDriver::login() {
    if (opts_.user.isEmpty() || server_->isLoggedIn() || loginId_ >= 0 || dropped_) return;
    const QString pw = QProcessEnvironment::systemEnvironment().value(opts_.passwordEnv);
    if (pw.isEmpty()) {
        say(QStringLiteral("%1 is not set; not signing in").arg(opts_.passwordEnv));
        return;
    }
    triedRegister_ = !opts_.registerAccount;
    loginId_ = opts_.registerAccount ? server_->registerAccount(opts_.user, pw) : server_->login(opts_.user, pw);
    say(QStringLiteral("signing in as %1").arg(opts_.user));
}

void TestDriver::startGame() {
    if (gameAsked_) return;
    gameAsked_ = true;
    QString err;
    if (controller_->startPlayerGame(&err)) say(QStringLiteral("the game is starting, as PLAY does"));
    else say(QStringLiteral("the game could not start: %1").arg(err));
}

// once signed in and the game is up (at its menus): the search, or the lobby
void TestDriver::begin() {
    if (begun_ || !active() || !server_->isLoggedIn()) return;
    GameProcess* g = controller_->game();
    if (!g || !g->isRunning() || !g->isReady()) return;
    begun_ = true;
    if (!opts_.queue.isEmpty()) {
        say(QStringLiteral("queueing %1").arg(opts_.queue));
        ask(QStringLiteral("queue kind=%1").arg(opts_.queue));
        if (opts_.cancelSearchSeconds >= 0)
            QTimer::singleShot(opts_.cancelSearchSeconds * 1000, this, [this] { ask(QStringLiteral("cancel")); });
        return;
    }
    beginLobby();
}

// --lobby host: create the lobby; --lobby join: look for it by name (or its code in the code file) until it is there,
// then join; both get ready once two are in. --lobby watch: join it as a spectator, whose game plays its matches
void TestDriver::beginLobby() {
    if (lobbyBegun_) return;
    lobbyBegun_ = true;
    const QString name = opts_.lobbyName.isEmpty() ? QStringLiteral("e2e lobby") : opts_.lobbyName;
    const QString mode = opts_.mode.isEmpty() ? QStringLiteral("versus") : opts_.mode;
    if (opts_.lobby == QLatin1String("host")) {
        say(QStringLiteral("creating %1lobby \"%2\" (%3)").arg(opts_.koth ? QStringLiteral("King of the Hill ") : QString(), name, mode));
        ask(QStringLiteral("lobby_create name=%1 mode=%2 koth=%3 private=0").arg(enc(name), mode).arg(opts_.koth ? 1 : 0));
        return;
    }
    // --lobby-own-first: in a lobby of one's own when the one to join turns up (the join moves the player there)
    if (opts_.lobbyOwnFirst && opts_.lobby == QLatin1String("join")) {
        say(QStringLiteral("creating a lobby of my own first"));
        ask(QStringLiteral("lobby_create mode=%1 koth=0 private=0").arg(mode));
    }
    say(QStringLiteral("looking for lobby \"%1\"").arg(name));
    poll_ = new QTimer(this);
    connect(poll_, &QTimer::timeout, this, [this] {
        if (!opts_.lobbyCodeFile.isEmpty()) {
            QFile f(opts_.lobbyCodeFile);
            const QString code = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
            if (code.isEmpty() || lobbyJoined_ || (opts_.lobbyOwnFirst && !ownLobbyIn_)) return;
            lobbyJoined_ = true;
            poll_->stop();
            join(code, opts_.lobby == QLatin1String("watch"));
            return;
        }
        ask(QStringLiteral("lobbies"));
    });
    poll_->start(1000);
}

void TestDriver::join(const QString& code, bool watch) {
    say(QStringLiteral("%1 lobby %2").arg(watch ? QStringLiteral("watching") : QStringLiteral("joining"), code));
    ask(watch ? QStringLiteral("lobby_join code=%1 watch=1").arg(code) : QStringLiteral("lobby_join code=%1").arg(code));
}

void TestDriver::onServerMessage(const QJsonObject& msg) {
    const QString type = s(msg, "type");
    if (loginId_ >= 0 && msg.value(QStringLiteral("id")).toInt(-2) == loginId_) {
        loginId_ = -1;
        if (type == QLatin1String("error")) {
            const QString code = s(msg, "code");
            say(QStringLiteral("login failed: %1").arg(code));
            if (code == QLatin1String("name_taken") && !triedRegister_) {
                triedRegister_ = true;
                loginId_ = server_->login(opts_.user, QProcessEnvironment::systemEnvironment().value(opts_.passwordEnv));
            }
        }
        return;
    }
    if (opts_.lobby.isEmpty()) return;
    const bool watch = opts_.lobby == QLatin1String("watch");
    // --lobby-own-first: in the lobby of one's own before joining (its state comes back first)
    if (type == QLatin1String("lobby") && opts_.lobbyOwnFirst && !lobbyJoined_) ownLobbyIn_ = true;
    if (type == QLatin1String("lobbies") && (opts_.lobby == QLatin1String("join") || watch) && !lobbyJoined_ &&
        lobbyBegun_ && (!opts_.lobbyOwnFirst || ownLobbyIn_)) {
        const QString name = opts_.lobbyName.isEmpty() ? QStringLiteral("e2e lobby") : opts_.lobbyName;
        for (const auto& v : msg.value(QStringLiteral("lobbies")).toArray()) {
            const QJsonObject lb = v.toObject();
            // King of the Hill: a lobby whose places are taken has a line to wait in
            if (s(lb, "name") != name || (!watch && !lb.value(QStringLiteral("koth")).toBool() &&
                                          s(lb, "state") != QLatin1String("open")))
                continue;
            lobbyJoined_ = true;
            if (poll_) poll_->stop();
            join(s(lb, "id"), watch);
            break;
        }
        return;
    }
    if (type == QLatin1String("lobby")) onLobby(msg);
}

void TestDriver::onLobby(const QJsonObject& msg) {
    const QJsonObject lobby = msg.value(QStringLiteral("lobby")).toObject();
    const bool watch = opts_.lobby == QLatin1String("watch");
    if (opts_.lobbyOwnFirst && opts_.lobby == QLatin1String("join") && !lobbyJoined_) return;  // only a waiting room
    lastLobby_ = msg;
    if (opts_.lobby == QLatin1String("host") && !opts_.lobbyCodeFile.isEmpty() && !codeWritten_) {
        codeWritten_ = true;
        QFile f(opts_.lobbyCodeFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(s(lobby, "id").toLatin1());
        say(QStringLiteral("lobby code %1 in %2").arg(s(lobby, "id"), opts_.lobbyCodeFile));
    }
    // --rate: a spectator of a King of the Hill match rates its winner once their game has shown its end (the game's
    // menus have no rating yet: straight to the server)
    const QJsonObject rating = lobby.value(QStringLiteral("rating")).toObject();
    if (opts_.rate > 0 && !rating.isEmpty() && s(rating, "match") != ratedMatch_ &&
        rating.value(QStringLiteral("raters")).toArray().contains(server_->accountName()) &&
        !rating.value(QStringLiteral("rated")).toArray().contains(server_->accountName()) && !controller_->inMatch()) {
        ratedMatch_ = s(rating, "match");
        say(QStringLiteral("rating %1's win %2").arg(s(rating, "winner")).arg(opts_.rate));
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_rate")}, {QStringLiteral("match"), ratedMatch_},
                       {QStringLiteral("score"), opts_.rate}});
    }
    if (watch) return;  // a spectator never readies
    QJsonArray members;
    bool mePlaying = false;  // King of the Hill: one waiting in line readies once it is their turn
    for (const auto& v : lobby.value(QStringLiteral("members")).toArray()) {
        if (v.toObject().value(QStringLiteral("spectator")).toBool()) continue;
        members.append(v);
        mePlaying = mePlaying || s(v.toObject(), "name") == server_->accountName();
    }
    if (s(lobby, "state") == QLatin1String("in_match")) {
        // --leave-lobby-in-match: out that many seconds into the first match, which is still under way
        if (opts_.leaveLobbyInMatch > 0 && !lobbyInMatch_ && !lobbyLeft_ && lobbyDone_ == 0)
            QTimer::singleShot(opts_.leaveLobbyInMatch * 1000, this, [this] {
                if (lobbyLeft_ || !lobbyInMatch_) return;
                lobbyLeft_ = true;
                ask(QStringLiteral("lobby_leave"));
            });
        lobbyInMatch_ = true;
        return;
    }
    if (lobbyInMatch_) {  // the lobby is open again: that match is over
        lobbyInMatch_ = false;
        lobbyDone_++;
        say(QStringLiteral("lobby match %1 of %2 over").arg(lobbyDone_).arg(opts_.lobbyMatches));
    }
    if (opts_.leaveLobby && lobbyDone_ >= opts_.lobbyMatches && !lobbyLeft_) {
        lobbyLeft_ = true;
        ask(QStringLiteral("lobby_leave"));
        return;
    }
    if (members.size() < 2 || !mePlaying || lobbyDone_ >= opts_.lobbyMatches) return;
    if (controller_->phase() != MatchController::Phase::Idle) return;  // the game still plays the last match's end
    bool meReady = false;
    for (const auto& v : members)
        if (s(v.toObject(), "name") == server_->accountName()) meReady = v.toObject().value(QStringLiteral("ready")).toBool();
    if (!meReady && readyFor_ != lobbyDone_) {
        readyFor_ = lobbyDone_;
        ask(QStringLiteral("lobby_ready value=1"));
    }
}
