#include "MenuBridge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <algorithm>

#include <windows.h>

#include "AppPaths.h"
#include "GameData.h"
#include "GameProcess.h"
#include "MatchController.h"
#include "ServerClient.h"
#include "WinUtil.h"

namespace {
QString s(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

const QString kGame = QStringLiteral("sc5");  // SCV Online's one game
const QString kMode = QStringLiteral("versus");  // its Ranked and Casual mode

// a newer than b, part by part (0.1.10 after 0.1.9), as the updater compares them
bool versionNewer(const QString& a, const QString& b) {
    const QStringList x = a.split(QLatin1Char('.')), y = b.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < std::max(x.size(), y.size()); i++) {
        const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
        if (p != q) return p > q;
    }
    return false;
}

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

// "ask n=3 lobby_create name=Friday%20sets mode=versus": the verb and its keys, text values percent-decoded (3.8)
QString parseAsk(const QString& line, QHash<QString, QString>* keys, int* n) {
    QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!tokens.isEmpty() && tokens.first() == QLatin1String("ask")) tokens.removeFirst();
    if (!tokens.isEmpty() && tokens.first().startsWith(QLatin1String("n="))) {
        if (n) *n = tokens.first().mid(2).toInt();
        tokens.removeFirst();
    }
    if (tokens.isEmpty()) return {};
    const QString verb = tokens.takeFirst();
    for (const QString& t : std::as_const(tokens)) {
        const qsizetype eq = t.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        keys->insert(t.left(eq), QString::fromUtf8(QByteArray::fromPercentEncoding(t.mid(eq + 1).toLatin1())));
    }
    return verb;
}

bool flag(const QHash<QString, QString>& keys, const char* key) {
    const QString v = keys.value(QLatin1String(key));
    return v == QLatin1String("1") || v == QLatin1String("true");
}
}  // namespace

MenuBridge::MenuBridge(ServerClient* server, MatchController* controller, QObject* parent)
    : QObject(parent), server_(server), controller_(controller) {
    // what the game writes is taken within a tenth of a second; ui.json goes out once a second at least, and at once
    // (within 50 ms, several changes together) whenever anything in it changes
    askTimer_.setInterval(100);
    connect(&askTimer_, &QTimer::timeout, this, &MenuBridge::pollAsk);
    askTimer_.start();
    writeTimer_.setInterval(1000);
    connect(&writeTimer_, &QTimer::timeout, this, &MenuBridge::writeUi);
    writeTimer_.start();
    dirtyTimer_.setSingleShot(true);
    dirtyTimer_.setInterval(50);
    connect(&dirtyTimer_, &QTimer::timeout, this, &MenuBridge::writeUi);
    // the open lobbies, kept current while the game's lobby menu wants them (a minute after its last `lobbies`)
    listTimer_.setInterval(4000);
    connect(&listTimer_, &QTimer::timeout, this, [this] {
        if (!listWanted_.isValid() || listWanted_.elapsed() > 60000) {
            listTimer_.stop();
            return;
        }
        if (server_->isLoggedIn() && !controller_->inMatch())
            server_->send({{QStringLiteral("type"), QStringLiteral("lobby_list")}, {QStringLiteral("game"), kGame}});
    });
    // a spectator's dropped connection: the server keeps the place for 60 s, and the game watches on meanwhile
    rejoinGrace_.setSingleShot(true);
    rejoinGrace_.setInterval(70000);
    connect(&rejoinGrace_, &QTimer::timeout, this, [this] {
        if (lobby_.isEmpty()) controller_->stopWatching();
    });

    connect(server_, &ServerClient::message, this, &MenuBridge::onServerMessage);
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        if (st == ServerClient::State::LoggedIn) {
            accountError_.clear();
        } else if (!lobby_.isEmpty()) {
            // signed out or offline: the server took this player out of the lobby (a dropped connection gets the place
            // back at the next login within its 60 s; a spectator's game keeps watching meanwhile)
            const bool dropped = st == ServerClient::State::Offline && watchingHere_;
            lobby_ = {};
            if (dropped) rejoinGrace_.start();
            else controller_->stopWatching();
        }
        if (st != ServerClient::State::LoggedIn) {
            series_ = {};
            board_ = {};
        }
        markDirty();
    });
    connect(server_, &ServerClient::loggedIn, this, [this](const QJsonObject& account) {
        stats_ = account.value(QStringLiteral("profile")).toObject().value(QStringLiteral("stats")).toObject();
        markDirty();
    });
    connect(server_, &ServerClient::connectionError, this, [this](const QString& text) {
        accountError_ = text;
        markDirty();
    });
    connect(server_, &ServerClient::kicked, this, [this](const QString& text) {
        accountError_ = text;
        markDirty();
    });

    connect(controller_, &MatchController::phaseChanged, this, &MenuBridge::markDirty);
    connect(controller_, &MatchController::gameStatusChanged, this, &MenuBridge::markDirty);
    connect(controller_, &MatchController::matchFound, this, [this](const QJsonObject& found) {
        // a series' next match or a lobby's needs no accept: it is connecting from the start
        accepted_ = !found.value(QStringLiteral("accept_required")).toBool();
        peerSeen_ = accepted_;
        acceptTimeout_ = found.value(QStringLiteral("accept_timeout")).toInt(15);
        foundClock_.start();
        markDirty();
    });
    connect(controller_, &MatchController::matchCancelled, this, [this](const QString& reason, bool requeued) {
        if (reason == QLatin1String("declined") && !accepted_) return;  // this player's own decline
        static const QHash<QString, QString> why = {
            {QStringLiteral("declined"), tr("the other player declined")},
            {QStringLiteral("accept_timeout"), tr("it was not accepted in time")},
            {QStringLiteral("punch_failed"), tr("the two of you could not connect")},
            {QStringLiteral("ping_too_high"), tr("the ping was above the limit")},
            {QStringLiteral("ping_timeout"), tr("the connection test took too long")},
            {QStringLiteral("no_udp"), tr("the server got nothing from your game port")},
            {QStringLiteral("peer_left"), tr("the other player left")},
        };
        showMessage(requeued ? tr("The match was called off (%1): you are searching again.").arg(why.value(reason, reason))
                             : tr("The match was called off (%1).").arg(why.value(reason, reason)),
                    !requeued);
    });
    connect(controller_, &MatchController::matchFinished, this, [this](const QJsonObject& r) {
        const QString opp = s(r, "opponent"), status = s(r, "status");
        // a lobby's matches this player played: who won how many, for its series line
        if (!s(controller_->currentFound(), "lobby").isEmpty() && r.contains(QStringLiteral("you_won")))
            lobbyWins_[r.value(QStringLiteral("you_won")).toBool() ? server_->accountName() : opp]++;
        QString text;
        if (status == QLatin1String("agreed") && r.contains(QStringLiteral("you_won")))
            text = r.value(QStringLiteral("you_won")).toBool() ? tr("You beat %1, %2.").arg(opp, s(r, "score"))
                                                                : tr("You lost to %1, %2.").arg(opp, s(r, "score"));
        else if (status == QLatin1String("draw"))
            text = tr("A draw with %1, %2: no win or loss for either of you.").arg(opp, s(r, "score"));
        else if (status == QLatin1String("disputed"))
            text = tr("Your two games reported different results: the match does not count.");
        else if (status == QLatin1String("abandoned"))
            text = r.value(QStringLiteral("left_side")).toInt() == r.value(QStringLiteral("side")).toInt()
                       ? tr("You left the match against %1.").arg(opp)
                       : tr("%1 left the match.").arg(opp);
        else
            text = tr("The match against %1 does not count (%2).").arg(opp, QString(status).replace(QLatin1Char('_'), QLatin1Char(' ')));
        showMessage(text);
        server_->send({{QStringLiteral("type"), QStringLiteral("profile")}});  // the record, a set's Elo
    });
    connect(controller_, &MatchController::seriesChanged, this, [this](const QJsonObject& series) {
        if (s(series, "state") == QLatin1String("over")) {
            series_ = {};
            seriesOver_ = series;
            seriesOverClock_.start();
            showMessage(seriesOverText(series));
            server_->send({{QStringLiteral("type"), QStringLiteral("profile")}});
        } else {
            series_ = series;
            seriesOver_ = {};
        }
        markDirty();
    });
    // what the launcher had to do by itself (a match that could not be played, a game that did not come back)
    connect(controller_, &MatchController::notice, this, [this](const QString& text) { showMessage(text, true); });
    connect(controller_, &MatchController::playerGameChanged, this, [this](bool running) {
        if (!running) onGameEnded();
        markDirty();
    });
}

// --- ui.json ---------------------------------------------------------------------------------------

void MenuBridge::markDirty() {
    if (!dirtyTimer_.isActive()) dirtyTimer_.start();
    emit changed();
}

QString MenuBridge::sessionDir() const {
    GameProcess* g = controller_->game();
    return g && g->menus() && g->isRunning() ? g->sessionDir() : QString();
}

QJsonObject MenuBridge::accountJson() const {
    QString state;
    switch (server_->state()) {
    case ServerClient::State::Offline: state = QStringLiteral("offline"); break;
    case ServerClient::State::Connecting: state = QStringLiteral("connecting"); break;
    case ServerClient::State::Online: state = QStringLiteral("signed_out"); break;
    case ServerClient::State::LoggedIn: state = QStringLiteral("signed_in"); break;
    }
    const QJsonObject ranked = versusStats().value(QStringLiteral("ranked")).toObject();
    const bool in = server_->isLoggedIn();
    return {{QStringLiteral("state"), state},
            {QStringLiteral("name"), in ? server_->accountName() : QString()},
            {QStringLiteral("rank"), in ? s(ranked, "title") : QString()},
            {QStringLiteral("elo"), in ? ranked.value(QStringLiteral("elo")).toInt() : 0},
            {QStringLiteral("wins"), in ? ranked.value(QStringLiteral("wins")).toInt() : 0},
            {QStringLiteral("losses"), in ? ranked.value(QStringLiteral("losses")).toInt() : 0},
            {QStringLiteral("error"), in ? QString() : accountError_}};
}

QJsonObject MenuBridge::versusStats() const {
    return stats_.value(kGame).toObject().value(kMode).toObject();
}

QJsonObject MenuBridge::queueJson() const {
    using P = MatchController::Phase;
    const P p = controller_->phase();
    const QJsonObject found = controller_->currentFound();
    const bool queueMatch = found.isEmpty() || s(found, "lobby").isEmpty();
    QString state = QStringLiteral("idle");
    if (p == P::Searching) state = QStringLiteral("searching");
    else if (queueMatch && p == P::Found) state = QStringLiteral("found");
    else if (queueMatch && p == P::Connecting) state = peerSeen_ ? QStringLiteral("connecting") : QStringLiteral("accepted");
    else if (queueMatch && p == P::Starting) state = QStringLiteral("starting");
    const bool active = state != QLatin1String("idle");
    const bool ranked = !found.isEmpty() ? found.value(QStringLiteral("ranked")).toBool() : controller_->lastRequest().ranked;
    const QJsonObject opp = found.value(QStringLiteral("opponent")).toObject();
    const QJsonObject conn = found.value(QStringLiteral("connection")).toObject();
    const int ping = conn.value(QStringLiteral("estimate")).toInt(-1) >= 0 ? conn.value(QStringLiteral("estimate")).toInt()
                                                                          : conn.value(QStringLiteral("ping")).toInt(0);
    const int left = state == QLatin1String("found") && foundClock_.isValid()
                         ? std::max(0, acceptTimeout_ - int(foundClock_.elapsed() / 1000))
                         : 0;
    const int secs = controller_->searchSeconds();
    QString text;
    if (state == QLatin1String("searching")) {
        const int raised = controller_->raisedMaxPing();
        text = tr("Searching for a %1 match: %2:%3.")
                   .arg(ranked ? tr("ranked") : tr("casual"))
                   .arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
        if (raised) text += QLatin1Char(' ') + tr("The ping limit is raised to %1 ms to find you someone.").arg(raised);
    } else if (state == QLatin1String("found")) {
        text = s(opp, "name").isEmpty() ? tr("Match found: accept within %1 seconds.").arg(left)
                                        : tr("Match found against %1: accept within %2 seconds.").arg(s(opp, "name")).arg(left);
    } else if (state == QLatin1String("accepted")) {
        text = tr("Accepted. Waiting for your opponent.");
    } else if (active) {
        text = controller_->phaseText();
    }
    return {{QStringLiteral("state"), state},
            {QStringLiteral("kind"), active ? (ranked ? QStringLiteral("ranked") : QStringLiteral("casual")) : QString()},
            {QStringLiteral("secs"), state == QLatin1String("searching") ? secs : 0},
            {QStringLiteral("accept_left"), left},
            {QStringLiteral("opponent"), active ? s(opp, "name") : QString()},
            {QStringLiteral("opponent_rank"), active ? s(opp, "title") : QString()},
            {QStringLiteral("ping"), active && state != QLatin1String("searching") ? ping : 0},
            {QStringLiteral("text"), text}};
}

QString MenuBridge::seriesOverText(const QJsonObject& series) const {
    const QString me = server_->accountName();
    const QString opp = s(series, "opponent"), outcome = s(series, "outcome");
    const QString score = QStringLiteral("%1-%2").arg(series.value(QStringLiteral("you")).toInt())
                              .arg(series.value(QStringLiteral("them")).toInt());
    const bool iLeft = s(series, "left") == me;
    QString text;
    if (!series.value(QStringLiteral("ranked")).toBool()) {
        if (outcome == QLatin1String("left"))
            text = iLeft ? tr("You left the series against %1 (%2).").arg(opp, score)
                         : tr("%1 left the series (%2).").arg(opp, score);
        else if (outcome == QLatin1String("disconnected"))
            text = tr("The connection to %1 was lost: the series is over (%2).").arg(opp, score);
        else
            text = tr("No rematch with %1 (%2).").arg(opp, score);
    } else if (outcome == QLatin1String("won")) {
        text = s(series, "winner") == me ? tr("You won the set against %1, %2!").arg(opp, score)
                                         : tr("%1 won the set, %2.").arg(opp, score);
    } else if (outcome == QLatin1String("forfeit")) {
        text = iLeft ? tr("You left the set against %1 at %2: it counts as a loss.").arg(opp, score)
                     : tr("%1 left the set at %2: you win it.").arg(opp, score);
    } else {
        text = tr("The set against %1 ended without a result (%2): no Elo change.").arg(opp, score);
    }
    const QJsonObject e = series.value(QStringLiteral("elo")).toObject().value(me).toObject();
    if (!e.isEmpty()) {
        const int delta = e.value(QStringLiteral("delta")).toInt();
        text += QLatin1Char(' ') + tr("Elo %1 to %2 (%3%4).").arg(e.value(QStringLiteral("before")).toInt())
                                       .arg(e.value(QStringLiteral("after")).toInt())
                                       .arg(delta >= 0 ? QStringLiteral("+") : QString()).arg(delta);
        const QString was = s(e, "title_before"), rank = s(e, "title_after");
        if (!rank.isEmpty() && was != rank)
            text += QLatin1Char(' ') + (delta > 0 ? tr("Ranked up to %1!") : tr("Ranked down to %1.")).arg(rank);
    }
    return text;
}

QString MenuBridge::seriesText() const {
    const QString opp = s(series_, "opponent");
    const bool ranked = series_.value(QStringLiteral("ranked")).toBool();
    const int you = series_.value(QStringLiteral("you")).toInt(), them = series_.value(QStringLiteral("them")).toInt();
    const QString score = tr("You %1, %2 %3.").arg(you).arg(opp).arg(them);
    if (s(series_, "state") != QLatin1String("between"))
        return ranked ? tr("Ranked set against %1, first to %2. %3").arg(opp).arg(series_.value(QStringLiteral("need")).toInt()).arg(score)
                      : tr("Casual against %1. %2").arg(opp, score);
    const int left = series_.value(QStringLiteral("seconds_left")).toInt();
    if (ranked) {
        const QString keeps = s(series_, "keeps");
        QString who;
        if (keeps == server_->accountName()) who = tr("You keep your character, %1 picks again.").arg(opp);
        else if (!keeps.isEmpty()) who = tr("%1 keeps their character, you pick yours again.").arg(opp);
        return tr("%1 %2 The next match of the set starts once both games are ready. Leaving the set now counts as "
                  "losing it.").arg(score, who).simplified();
    }
    if (series_.value(QStringLiteral("you_ready")).toBool())
        return tr("%1 Waiting for %2 (%3 seconds).").arg(score, opp).arg(left);
    if (series_.value(QStringLiteral("them_ready")).toBool())
        return tr("%1 %2 wants to play again (%3 seconds).").arg(score, opp).arg(left);
    return tr("%1 Rematch, character select, or leave? (%2 seconds)").arg(score).arg(left);
}

QString MenuBridge::lobbyOpponent() const {
    const QString mine = server_->accountName();
    for (const auto& v : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject m = v.toObject();
        if (!m.value(QStringLiteral("spectator")).toBool() && s(m, "name") != mine) return s(m, "name");
    }
    return {};
}

QJsonObject MenuBridge::seriesJson() const {
    QJsonObject out{{QStringLiteral("kind"), QString()}, {QStringLiteral("opponent"), QString()},
                    {QStringLiteral("mine"), 0}, {QStringLiteral("theirs"), 0}, {QStringLiteral("to"), 0},
                    {QStringLiteral("state"), QString()}, {QStringLiteral("text"), QString()}};
    const QJsonObject& sr = !series_.isEmpty() ? series_ : seriesOver_;
    if (!sr.isEmpty() && (!series_.isEmpty() || (seriesOverClock_.isValid() && seriesOverClock_.elapsed() < 30000))) {
        const bool ranked = sr.value(QStringLiteral("ranked")).toBool();
        const QString st = s(sr, "state");
        out = {{QStringLiteral("kind"), ranked ? QStringLiteral("ranked") : QStringLiteral("casual")},
               {QStringLiteral("opponent"), s(sr, "opponent")},
               {QStringLiteral("mine"), sr.value(QStringLiteral("you")).toInt()},
               {QStringLiteral("theirs"), sr.value(QStringLiteral("them")).toInt()},
               {QStringLiteral("to"), ranked ? sr.value(QStringLiteral("need")).toInt() : 0},
               {QStringLiteral("state"), st == QLatin1String("over") ? QStringLiteral("over")
                                         : st == QLatin1String("between")
                                             ? (sr.value(QStringLiteral("you_ready")).toBool() ? QStringLiteral("waiting")
                                                                                               : QStringLiteral("choosing"))
                                             : QString()},
               {QStringLiteral("text"), st == QLatin1String("over") ? seriesOverText(sr) : seriesText()}};
        return out;
    }
    // a lobby's matches against the other player in it
    const QString opp = lobbyOpponent();
    if (!lobby_.isEmpty() && !me().value(QStringLiteral("spectator")).toBool() && !opp.isEmpty()) {
        const int mine = lobbyWins_.value(server_->accountName()), theirs = lobbyWins_.value(opp);
        const bool choosing = controller_->phase() == MatchController::Phase::AtResult &&
                              !s(controller_->currentFound(), "lobby").isEmpty();
        out = {{QStringLiteral("kind"), QStringLiteral("lobby")},
               {QStringLiteral("opponent"), opp},
               {QStringLiteral("mine"), mine},
               {QStringLiteral("theirs"), theirs},
               {QStringLiteral("to"), 0},
               {QStringLiteral("state"), choosing ? QStringLiteral("choosing") : QString()},
               {QStringLiteral("text"), choosing ? tr("You %1, %2 %3. The loser of the last match chooses what comes next.")
                                                       .arg(mine).arg(opp).arg(theirs)
                                                 : tr("You %1, %2 %3.").arg(mine).arg(opp).arg(theirs)}};
    }
    return out;
}

QJsonObject MenuBridge::me() const {
    for (const auto& v : lobby_.value(QStringLiteral("members")).toArray())
        if (s(v.toObject(), "name") == server_->accountName()) return v.toObject();
    return {};
}

QString MenuBridge::lobbyText() const {
    const bool inMatch = s(lobby_, "state") == QLatin1String("in_match");
    const QJsonObject self = me();
    const bool watching = self.value(QStringLiteral("spectator")).toBool();
    const bool hill = lobby_.value(QStringLiteral("koth")).toBool();
    const QString opp = lobbyOpponent();
    QStringList parts;
    if (!versionProblem_.isEmpty()) parts << versionProblem_;
    if (!lobbyPassword_.isEmpty() && s(lobby_, "host") == server_->accountName())
        parts << tr("Private lobby: your friend joins with the code %1 and the password %2.").arg(s(lobby_, "id"), lobbyPassword_);
    const double startsIn = lobby_.value(QStringLiteral("starts_in")).toDouble(-1);
    if (inMatch) {
        parts << (watching ? tr("A match is on: it plays in your game.") : tr("Match in progress."));
    } else if (startsIn >= 0) {
        parts << tr("Both players are ready: the match starts in %1 seconds.").arg(int(startsIn + 0.999));
    } else if (watching) {
        const int line = self.value(QStringLiteral("line")).toInt();
        parts << (line == 1   ? tr("You are next in line.")
                  : line > 1 ? tr("You are number %1 in line.").arg(line)
                             : tr("You are watching: each match plays in your game."));
    } else if (opp.isEmpty()) {
        parts << tr("Waiting for an opponent: share the code %1.").arg(s(lobby_, "id"));
    } else if (hill && s(lobby_, "king") == server_->accountName()) {
        parts << tr("You are the king: get ready to face %1.").arg(opp);
    } else if (self.value(QStringLiteral("ready")).toBool()) {
        parts << tr("You are ready. Waiting for %1.").arg(opp);
    } else {
        parts << tr("%1 is here: get ready, and the match starts once you both are.").arg(opp);
    }
    return parts.join(QLatin1Char(' '));
}

QJsonObject MenuBridge::lobbyJson() const {
    if (lobby_.isEmpty()) return {};
    QJsonArray members;
    for (const auto& v : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject m = v.toObject();
        members.append(QJsonObject{
            {QStringLiteral("name"), s(m, "name")},
            {QStringLiteral("ready"), m.value(QStringLiteral("ready")).toBool()},
            {QStringLiteral("role"), m.value(QStringLiteral("spectator")).toBool() ? QStringLiteral("spectator")
                                                                                  : QStringLiteral("player")},
            {QStringLiteral("wins"), lobbyWins_.value(s(m, "name"))}});
    }
    const bool myMatch = controller_->inMatch() && !s(controller_->currentFound(), "lobby").isEmpty();
    return {{QStringLiteral("code"), s(lobby_, "id")},
            {QStringLiteral("name"), s(lobby_, "name")},
            {QStringLiteral("host"), s(lobby_, "host")},
            {QStringLiteral("mode"), s(lobby_, "mode")},
            {QStringLiteral("koth"), lobby_.value(QStringLiteral("koth")).toBool()},
            {QStringLiteral("locked"), lobby_.value(QStringLiteral("locked")).toBool()},
            {QStringLiteral("password"), s(lobby_, "host") == server_->accountName() ? lobbyPassword_ : QString()},
            {QStringLiteral("ready"), me().value(QStringLiteral("ready")).toBool()},
            {QStringLiteral("state"), s(lobby_, "state") == QLatin1String("in_match") || myMatch ? QStringLiteral("match")
                                                                                                : QStringLiteral("waiting")},
            {QStringLiteral("text"), lobbyText()},
            {QStringLiteral("members"), members}};
}

QJsonObject MenuBridge::state() const {
    QJsonArray list;
    for (const auto& v : lobbies_) {
        const QJsonObject lb = v.toObject();
        list.append(QJsonObject{{QStringLiteral("code"), s(lb, "id")},
                                {QStringLiteral("name"), s(lb, "name")},
                                {QStringLiteral("host"), s(lb, "host")},
                                {QStringLiteral("players"), lb.value(QStringLiteral("players")).toInt()},
                                {QStringLiteral("max"), lb.value(QStringLiteral("max_players")).toInt(2)},
                                {QStringLiteral("mode"), s(lb, "mode")},
                                {QStringLiteral("koth"), lb.value(QStringLiteral("koth")).toBool()},
                                {QStringLiteral("locked"), lb.value(QStringLiteral("locked")).toBool()},
                                {QStringLiteral("region"), s(lb, "region")}});
    }
    QJsonArray rows;
    for (const auto& v : board_) {
        const QJsonObject r = v.toObject();
        rows.append(QJsonObject{{QStringLiteral("place"), r.value(QStringLiteral("rank")).toInt()},
                                {QStringLiteral("name"), s(r, "name")},
                                {QStringLiteral("rank"), s(r, "title")},
                                {QStringLiteral("elo"), r.value(QStringLiteral("elo")).toInt()},
                                {QStringLiteral("wins"), r.value(QStringLiteral("wins")).toInt()},
                                {QStringLiteral("losses"), r.value(QStringLiteral("losses")).toInt()}});
    }
    const QJsonObject game = online_.value(QStringLiteral("games")).toObject().value(kGame).toObject();
    const bool online = server_->isOnline();
    const QJsonObject lobby = lobbyJson();
    return {{QStringLiteral("v"), 1},
            {QStringLiteral("t"), now()},
            {QStringLiteral("account"), accountJson()},
            {QStringLiteral("server"),
             QJsonObject{{QStringLiteral("online"), online ? online_.value(QStringLiteral("count")).toInt() : 0},
                         {QStringLiteral("in_matches"),
                          online ? game.value(QStringLiteral("matches")).toInt(online_.value(QStringLiteral("playing")).toInt())
                                 : 0},
                         {QStringLiteral("notice"),
                          noticeClock_.isValid() && noticeClock_.elapsed() < 120000 ? notice_ : QString()}}},
            {QStringLiteral("side"), cfg::preferredSide()},
            {QStringLiteral("queue"), queueJson()},
            {QStringLiteral("series"), seriesJson()},
            {QStringLiteral("lobbies"), QJsonObject{{QStringLiteral("at"), lobbiesAt_}, {QStringLiteral("list"), list}}},
            {QStringLiteral("lobby"), lobby.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(lobby)},
            {QStringLiteral("leaderboard"), QJsonObject{{QStringLiteral("at"), boardAt_}, {QStringLiteral("rows"), rows}}},
            {QStringLiteral("message"), message_},
            {QStringLiteral("update"), update_}};
}

void MenuBridge::writeUi() {
    const QString dir = sessionDir();
    const bool running = !dir.isEmpty();
    if (running != gameWasRunning_ || dir != lastDir_) {
        gameWasRunning_ = running;
        lastDir_ = dir;
        asksTaken_ = 0;
    }
    if (!running) return;
    QString err;
    if (!winutil::writeFileAtomic(QDir(dir).filePath(QStringLiteral("ui.json")),
                                  QJsonDocument(state()).toJson(QJsonDocument::Compact) + '\n', &err))
        applog::write(QStringLiteral("bridge: %1").arg(err));
}

void MenuBridge::setUpdate(const QString& version, const QString& text) {
    update_ = {{QStringLiteral("version"), version}, {QStringLiteral("text"), text}};
    markDirty();
}

void MenuBridge::showMessage(const QString& text, bool error) {
    if (text.isEmpty()) return;
    message_ = {{QStringLiteral("id"), message_.value(QStringLiteral("id")).toInt() + 1},
                {QStringLiteral("kind"), error ? QStringLiteral("error") : QStringLiteral("info")},
                {QStringLiteral("text"), text}};
    applog::write(QStringLiteral("bridge: message %1").arg(text));
    markDirty();
}

// --- ask.txt ---------------------------------------------------------------------------------------

void MenuBridge::pollAsk() {
    const QString dir = sessionDir();
    if (dir.isEmpty()) return;
    const QString path = QDir(dir).filePath(QStringLiteral("ask.txt"));
    if (!QFileInfo::exists(path)) return;
    // claimed by renaming it, as the game claims cmd.txt (3.3): a line written while this reads is never lost
    const QString taken = path + QStringLiteral(".taken");
    const std::wstring from = QDir::toNativeSeparators(path).toStdWString(), to = QDir::toNativeSeparators(taken).toStdWString();
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return;  // the game is writing it: next time
    const QString line = QString::fromLatin1(winutil::readShared(taken)).section(QLatin1Char('\n'), 0, 0).trimmed();
    if (line.isEmpty()) return;
    asksTaken_++;
    // what the game said before it asked goes first (a result written just before leaving the lobby is reported as
    // the result, not as leaving the match)
    if (GameProcess* g = controller_->game()) g->readStatusNow();
    ask(line);
}

void MenuBridge::refuse(const QString& verb, const QString& why) {
    applog::write(QStringLiteral("bridge: %1 not done: %2").arg(verb, why));
    showMessage(why, true);
}

void MenuBridge::ask(const QString& line) {
    QHash<QString, QString> keys;
    int n = 0;
    const QString verb = parseAsk(line, &keys, &n);
    applog::write(QStringLiteral("bridge: %1").arg(line.startsWith(QLatin1String("ask ")) ? line.left(200)
                                                                                         : QStringLiteral("ask ") + line.left(200)));
    if (verb == QLatin1String("queue")) askQueue(keys);
    else if (verb == QLatin1String("cancel")) askCancel();
    else if (verb == QLatin1String("accept")) askAccept(true);
    else if (verb == QLatin1String("decline")) askAccept(false);
    else if (verb == QLatin1String("leave")) askLeave();
    else if (verb == QLatin1String("side")) askSide(keys.value(QStringLiteral("value")));
    else if (verb == QLatin1String("lobbies")) askLobbies();
    else if (verb == QLatin1String("lobby_create")) askLobbyCreate(keys);
    else if (verb == QLatin1String("lobby_join")) askLobbyJoin(keys);
    else if (verb == QLatin1String("lobby_ready")) askLobbyReady(flag(keys, "value"));
    else if (verb == QLatin1String("lobby_leave")) askLobbyLeave();
    else if (verb == QLatin1String("leaderboard")) askLeaderboard();
    else refuse(verb, tr("The launcher does not know what \"%1\" asks for: update SCV Online.").arg(verb.left(30)));
    markDirty();
}

void MenuBridge::askQueue(const QHash<QString, QString>& keys) {
    const QString kind = keys.value(QStringLiteral("kind"));
    if (kind != QLatin1String("ranked") && kind != QLatin1String("casual"))
        return refuse(QStringLiteral("queue"), tr("A search is ranked or casual."));
    if (!server_->isLoggedIn()) return refuse(QStringLiteral("queue"), tr("Sign in first, on SCV Online's Play page."));
    using P = MatchController::Phase;
    const P p = controller_->phase();
    if (p == P::Searching || p == P::Found)
        return refuse(QStringLiteral("queue"), tr("You are searching already."));
    if (!lobby_.isEmpty()) return refuse(QStringLiteral("queue"), tr("You are in a lobby: leave it first."));
    QueueRequest req;
    req.game = kGame;
    req.mode = kMode;
    req.ranked = kind == QLatin1String("ranked");
    req.region = cfg::region();
    req.maxPing = cfg::maxPing();
    req.strict = req.ranked && cfg::strict();
    req.character = testCharacter_;  // the game's own menus always pick on its select screen
    const int delay = cfg::inputDelay();
    req.delay = delay < 0 ? cfg::kAutoDelay : std::max(req.ranked ? 1 : 0, delay);  // ranked: at least a frame
    QString error;
    if (!controller_->findMatch(req, &error)) return refuse(QStringLiteral("queue"), error);
    seriesOver_ = {};
}

void MenuBridge::askCancel() {
    using P = MatchController::Phase;
    const P p = controller_->phase();
    const bool queueMatch = s(controller_->currentFound(), "lobby").isEmpty();
    if (p == P::Searching || p == P::Found || (p == P::Connecting && queueMatch && !accepted_))
        controller_->cancelSearch();
    else
        refuse(QStringLiteral("cancel"), p == P::Connecting || p == P::Starting
                                             ? tr("The match is already being set up.")
                                             : tr("You are not searching."));
}

void MenuBridge::askAccept(bool accept) {
    if (controller_->phase() != MatchController::Phase::Found)
        return refuse(accept ? QStringLiteral("accept") : QStringLiteral("decline"), tr("No match is waiting for an answer."));
    if (accept) {
        accepted_ = true;
        controller_->accept();
    } else {
        controller_->decline();
    }
}

void MenuBridge::askLeave() {
    if (controller_->series().isEmpty()) return refuse(QStringLiteral("leave"), tr("You are not in a set or a series."));
    controller_->seriesReady(false);  // the server tells the other player; a ranked set counted already is lost
}

void MenuBridge::chooseNext(const QString& choice) {
    const QJsonObject series = controller_->series();
    if (series.isEmpty() || s(series, "state") != QLatin1String("between"))
        return refuse(QStringLiteral("rematch"), tr("No next match is being chosen."));
    controller_->seriesReady(true, choice);
}

void MenuBridge::askSide(const QString& value) {
    if (value != QLatin1String("off") && value != QLatin1String("left") && value != QLatin1String("right"))
        return refuse(QStringLiteral("side"), tr("The preferred side is off, left or right."));
    cfg::set(QStringLiteral("display/preferred_side"), value);  // display.txt too: the game takes it from there
}

void MenuBridge::askLobbies() {
    if (!server_->isLoggedIn()) return refuse(QStringLiteral("lobbies"), tr("Sign in first, on SCV Online's Play page."));
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_list")}, {QStringLiteral("game"), kGame}});
    listWanted_.start();
    if (!listTimer_.isActive()) listTimer_.start();
}

void MenuBridge::sendJoin(const QJsonObject& msg) {
    if (controller_->phase() == MatchController::Phase::Searching)
        controller_->cancelSearch();  // queue_leave goes first, on the same connection
    lastJoin_ = msg;
    retriedJoin_ = false;
    server_->send(msg);
}

void MenuBridge::askLobbyCreate(const QHash<QString, QString>& keys) {
    if (!server_->isLoggedIn()) return refuse(QStringLiteral("lobby_create"), tr("Sign in first, on SCV Online's Play page."));
    if (controller_->inMatch() || controller_->phase() == MatchController::Phase::AtResult)
        return refuse(QStringLiteral("lobby_create"), tr("Your match is still on: finish it first."));
    const QString mode = keys.value(QStringLiteral("mode"), kMode);
    const Game* g = GameData::instance().game(kGame);
    if (!g || !g->mode(mode)) return refuse(QStringLiteral("lobby_create"), tr("A lobby is versus or Online Practice (sparring)."));
    const bool koth = flag(keys, "koth");
    if (koth && !server_->serverHas(QStringLiteral("koth")))
        return refuse(QStringLiteral("lobby_create"), tr("This server has no King of the Hill lobbies."));
    int rounds = keys.value(QStringLiteral("rounds"), QStringLiteral("5")).toInt();
    if (rounds != 1 && rounds != 3 && rounds != 5) rounds = 5;  // first to 3, as the console
    // a private lobby: a password this launcher makes up (the game's menu shows it to its host), unless one is given
    QString password = keys.value(QStringLiteral("password")).left(64);
    if (password.isEmpty() && flag(keys, "private"))
        password = QString::number(1000 + QRandomGenerator::global()->bounded(9000));
    pendingPassword_ = password;  // the lobby's once its state comes back (lobbyPassword_)
    QString name = keys.value(QStringLiteral("name")).simplified().left(32);
    if (name.isEmpty()) name = tr("%1's lobby").arg(server_->accountName());
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_create")},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("game"), kGame},
                    {QStringLiteral("mode"), mode},
                    {QStringLiteral("password"), password},
                    {QStringLiteral("rounds"), rounds},
                    {QStringLiteral("region"), cfg::region()},
                    {QStringLiteral("character"), testCharacter_},
                    {QStringLiteral("link"), server_->linkType()}};
    if (koth) msg.insert(QStringLiteral("koth"), true);
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    sendJoin(msg);
}

void MenuBridge::askLobbyJoin(const QHash<QString, QString>& keys) {
    if (!server_->isLoggedIn()) return refuse(QStringLiteral("lobby_join"), tr("Sign in first, on SCV Online's Play page."));
    const QString code = keys.value(QStringLiteral("code")).trimmed().toUpper();
    if (code.size() != 5) return refuse(QStringLiteral("lobby_join"), tr("Lobby codes have 5 characters."));
    if (code == s(lobby_, "id")) return refuse(QStringLiteral("lobby_join"), tr("You are in lobby %1 already.").arg(code));
    if (controller_->inMatch() || controller_->phase() == MatchController::Phase::AtResult)
        return refuse(QStringLiteral("lobby_join"), tr("Your match is still on: finish it first."));
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_join")},
                    {QStringLiteral("lobby"), code},
                    {QStringLiteral("password"), keys.value(QStringLiteral("password")).left(64)},
                    {QStringLiteral("character"), testCharacter_},
                    {QStringLiteral("spectate"), flag(keys, "watch")},
                    {QStringLiteral("link"), server_->linkType()}};
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    sendJoin(msg);
}

void MenuBridge::askLobbyReady(bool ready) {
    if (lobby_.isEmpty()) return refuse(QStringLiteral("lobby_ready"), tr("You are not in a lobby."));
    if (me().value(QStringLiteral("spectator")).toBool())
        return refuse(QStringLiteral("lobby_ready"), tr("You are watching this lobby: spectators do not get ready."));
    if (ready) {
        if (s(lobby_, "state") == QLatin1String("in_match") || controller_->inMatch())
            return refuse(QStringLiteral("lobby_ready"), tr("A match is on: get ready once it is over."));
        if (versionBlocked_) return refuse(QStringLiteral("lobby_ready"), versionProblem_);
        QString err;
        if (!controller_->prepareGame(s(lobby_, "game"), &err)) return refuse(QStringLiteral("lobby_ready"), err);
    }
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_ready")}, {QStringLiteral("ready"), ready},
                    {QStringLiteral("relay"), cfg::hideIp()}, {QStringLiteral("hide_always"), cfg::hideIpAlways()},
                    {QStringLiteral("link"), server_->linkType()}};
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    cfg::putDelay(msg, cfg::inputDelay());
    server_->send(msg);
}

void MenuBridge::askLobbyLeave() {
    if (lobby_.isEmpty()) return refuse(QStringLiteral("lobby_leave"), tr("You are not in a lobby."));
    controller_->stopWatching();
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
}

void MenuBridge::askLeaderboard() {
    if (!server_->isOnline()) return refuse(QStringLiteral("leaderboard"), tr("Not connected to the server."));
    server_->send({{QStringLiteral("type"), QStringLiteral("leaderboard")}, {QStringLiteral("game"), kGame},
                   {QStringLiteral("mode"), kMode}, {QStringLiteral("limit"), 100}, {QStringLiteral("by"), QStringLiteral("elo")}});
}

void MenuBridge::leftLobby() {
    lobby_ = {};
    lobbyWins_.clear();
    lobbyPassword_.clear();
    versionProblem_.clear();
    versionBlocked_ = false;
    watchingHere_ = false;
}

// The player's game closed (by the player, a crash): nothing online can go on without it. The controller has taken the
// match itself (a search left, a match reported as left); the set or series and the lobby are left here.
void MenuBridge::onGameEnded() {
    if (!controller_->series().isEmpty()) {
        applog::write(QStringLiteral("bridge: the game closed: leaving the set or series"));
        controller_->seriesReady(false);
    }
    if (!lobby_.isEmpty()) {
        applog::write(QStringLiteral("bridge: the game closed: leaving the lobby"));
        controller_->stopWatching();
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
    }
    using P = MatchController::Phase;
    if (controller_->phase() == P::Searching || controller_->phase() == P::Found) controller_->cancelSearch();
}

// --- the server ------------------------------------------------------------------------------------

void MenuBridge::onServerMessage(const QJsonObject& msg) {
    const QString type = s(msg, "type");
    if (type == QLatin1String("online")) {
        online_ = msg;
    } else if (type == QLatin1String("welcome")) {
        online_ = QJsonObject{{QStringLiteral("count"), msg.value(QStringLiteral("online"))}};
        if (!s(msg, "motd").isEmpty()) {
            notice_ = s(msg, "motd");
            noticeClock_.start();
        }
    } else if (type == QLatin1String("notice")) {
        notice_ = s(msg, "text");
        noticeClock_.start();
        showMessage(notice_, s(msg, "level") == QLatin1String("warn"));
    } else if (type == QLatin1String("profile")) {
        if (s(msg, "name") == server_->accountName()) stats_ = msg.value(QStringLiteral("stats")).toObject();
    } else if (type == QLatin1String("match_peer")) {
        if (s(msg, "match") == s(controller_->currentFound(), "match")) peerSeen_ = true;
    } else if (type == QLatin1String("spectate")) {
        // a lobby match this spectator watches: who plays which side, for the wins its end gives
        watchedSides_.insert(s(msg, "match"), {s(msg, "p1_name"), s(msg, "p2_name")});
        return;
    } else if (type == QLatin1String("spectate_end")) {
        const QStringList sides = watchedSides_.take(s(msg, "match"));
        const int winner = msg.value(QStringLiteral("winner")).toInt();
        if (s(msg, "status") != QLatin1String("agreed") || sides.size() != 2 || (winner != 1 && winner != 2)) return;
        lobbyWins_[sides.at(winner - 1)]++;
    } else if (type == QLatin1String("lobbies")) {
        lobbies_ = msg.value(QStringLiteral("lobbies")).toArray();
        lobbiesAt_ = now();
        emit lobbiesChanged(lobbies_);
    } else if (type == QLatin1String("lobby")) {
        const QJsonObject lb = msg.value(QStringLiteral("lobby")).toObject();
        if (s(lb, "id") != s(lobby_, "id")) {  // a lobby of its own now (created, joined, moved to)
            lobbyWins_.clear();
            lobbyPassword_ = s(lb, "host") == server_->accountName() ? pendingPassword_ : QString();
            pendingPassword_.clear();
            switching_ = false;
            seriesOver_ = {};
        }
        lobby_ = lb;
        const bool watching = me().value(QStringLiteral("spectator")).toBool();
        watchingHere_ = watching;
        // a player now: nothing more to watch (King of the Hill calls a spectator up while their game may still show
        // the match just played: it plays its end first)
        if (!watching && !lb.value(QStringLiteral("koth")).toBool()) controller_->stopWatching();
        else if (watching) rejoinGrace_.stop();
        updateVersionProblem();
    } else if (type == QLatin1String("lobby_left")) {
        if (!lobby_.isEmpty() && s(msg, "lobby") != s(lobby_, "id")) return;  // one left before the one shown
        // gone to another lobby (`joined`, or this launcher's own leave for an older server): the game goes on in the
        // new one. Left: the match of the old one, if any, ends for this player (the game stays)
        const bool moving = msg.contains(QStringLiteral("joined")) || switching_;
        controller_->stopWatching();
        leftLobby();
        if (moving) controller_->lobbySwitched();
        else controller_->closeGameWhenIdle();
    } else if (type == QLatin1String("leaderboard")) {
        if (s(msg, "by") == QLatin1String("elo") || !msg.contains(QStringLiteral("by"))) {
            board_ = msg.value(QStringLiteral("rows")).toArray();
            boardAt_ = now();
        }
    } else if (type == QLatin1String("error")) {
        const QString re = s(msg, "re"), code = s(msg, "code"), text = s(msg, "message");
        const bool joining = re == QLatin1String("lobby_join") || re == QLatin1String("lobby_create");
        if (code == QLatin1String("not_in_lobby")) {
            if (!lobby_.isEmpty()) {
                controller_->stopWatching();
                leftLobby();
                controller_->closeGameWhenIdle();
            }
        } else if (joining && code == QLatin1String("busy") && !lobby_.isEmpty() && !retriedJoin_ && !lastJoin_.isEmpty() &&
                   !controller_->inMatch() && controller_->phase() != MatchController::Phase::Searching &&
                   controller_->phase() != MatchController::Phase::Found) {
            // an older server turns away a join from a player still in a lobby, where a newer one moves them: left,
            // then asked again (the two go in order on the connection)
            retriedJoin_ = true;
            switching_ = true;
            server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
            server_->send(lastJoin_);
            return;
        } else if (re.startsWith(QLatin1String("lobby_")) || re.startsWith(QLatin1String("queue")) ||
                   re.startsWith(QLatin1String("series")) || re.startsWith(QLatin1String("match_")) ||
                   re == QLatin1String("leaderboard")) {
            if (joining) {
                switching_ = false;
                pendingPassword_.clear();
            }
            showMessage(re == QLatin1String("lobby_create") ? tr("The lobby could not be made: %1").arg(text)
                        : re == QLatin1String("lobby_join") ? tr("Could not join the lobby: %1").arg(text)
                                                            : text,
                        true);
        }
    } else {
        return;
    }
    markDirty();
}

// Each member's launcher version (and build) comes with the lobby's state (docs/PROTOCOL.md 1.10, SCV 0.1.4): two of
// different versions run different games, which cannot play each other, so the lobby says so before anyone readies,
// who must update, and Ready stays off against the other player. Another build of the same version may go out of step.
void MenuBridge::updateVersionProblem() {
    const QString mine = server_->accountName();
    const QString myVersion = QStringLiteral(SC5_LAUNCHER_VERSION), myBuild = paths::packageBuild();
    const bool meWatching = me().value(QStringLiteral("spectator")).toBool();
    QStringList said;
    bool blocked = false;
    for (const auto& v : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject m = v.toObject();
        const QString name = s(m, "name"), version = s(m, "version"), build = s(m, "build");
        if (name == mine || version.isEmpty()) continue;
        const bool plays = !m.value(QStringLiteral("spectator")).toBool() || m.value(QStringLiteral("line")).toInt() > 0;
        if (!plays && !meWatching) continue;
        if (meWatching && m.value(QStringLiteral("spectator")).toBool()) continue;
        if (version != myVersion) {
            said << (versionNewer(version, myVersion)
                         ? tr("%1 has SCV Online %2 and you have %3: your games cannot play each other. Update SCV "
                              "Online (close the game, then update on its Play page).").arg(name, version, myVersion)
                         : tr("%1 has SCV Online %2 and you have %3: your games cannot play each other. %1 must update.")
                               .arg(name, version, myVersion));
            if (!m.value(QStringLiteral("spectator")).toBool() && !meWatching) blocked = true;
        } else if (!build.isEmpty() && !myBuild.isEmpty() && build != myBuild) {
            said << tr("%1 has another build of SCV Online %2 than yours: your matches may go out of step.").arg(name, version);
        }
    }
    versionBlocked_ = blocked;
    versionProblem_ = said.join(QLatin1Char(' '));
}
