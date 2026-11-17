#include "GameData.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "X360.h"

const GameMode* Game::mode(const QString& modeId) const {
    for (const auto& m : modes)
        if (m.id == modeId) return &m;
    return nullptr;
}

QString Game::otherRelease(const QByteArray& xex) const {
    const QByteArray sha1 = QCryptographicHash::hash(xex, QCryptographicHash::Sha1).toHex();
    if (xexSha1.isEmpty() || QString::fromLatin1(sha1).compare(xexSha1, Qt::CaseInsensitive) == 0) return {};
    const auto tr = [](const char* text) { return QCoreApplication::translate("Game", text); };
    x360::XexInfo info;
    QString why;
    QString what;
    if (!x360::readXex(xex, &info, &why)) {
        what = tr("not an Xbox 360 game's default.xex (%1)").arg(why);
    } else if (!info.hasExecution) {
        what = tr("a default.xex without the title's execution info");
    } else if (info.titleId != titleIdValue()) {
        what = tr("another game (title ID %1)").arg(info.titleId, 8, 16, QLatin1Char('0')).toUpper();
    } else if (x360::versionText(info.version) != xexVersion) {
        what = tr("%1 version %2 (this build is made from version %3)")
                   .arg(title, x360::versionText(info.version), xexVersion);
    } else if (QStringLiteral("%1").arg(info.mediaId, 8, 16, QLatin1Char('0')).compare(mediaId, Qt::CaseInsensitive) != 0) {
        what = tr("another print or region of %1 (media ID %2, this build's is %3)")
                   .arg(title, QStringLiteral("%1").arg(info.mediaId, 8, 16, QLatin1Char('0')).toUpper(), mediaId);
    } else {
        what = tr("a modified copy of %1 (the same version, changed: a patched or damaged executable)").arg(title);
    }
    return tr("This disc is %1, not %2 (%3). SC5 Online builds the game from that release's default.xex only "
              "(SHA-1 %4): both players of a match must run the very same code, and the title update fits that "
              "version only. Use a disc image of %2 (%3).")
        .arg(what, title, edition, xexSha1);
}

QList<GameMode> Game::onlineModes() const {
    QList<GameMode> out;
    for (const auto& m : modes)
        if (m.online) out.append(m);
    return out;
}

QList<Character> Game::roster(const QString& modeId) const {
    const GameMode* m = mode(modeId);
    const QString key = m && !m->roster.isEmpty() ? m->roster : QStringLiteral("characters");
    return rosters.value(key, rosters.value(QStringLiteral("characters")));
}

QString Game::characterName(const QString& characterId) const {
    if (!characterId.isEmpty() && characterId.front().isDigit()) {
        bool ok = false;
        const int gameId = characterId.section(QLatin1Char('.'), 0, 0).toInt(&ok);
        return ok ? gameNames.value(gameId, characterId) : characterId;
    }
    for (auto it = rosters.cbegin(); it != rosters.cend(); ++it)
        for (const auto& c : it.value())
            if (c.id == characterId) return c.name;
    return characterId;
}

GameData& GameData::instance() {
    static GameData data;
    return data;
}

bool GameData::load(QString* error) {
    games_.clear();
    for (const char* id : {"sc5"}) {
        QFile f(QStringLiteral(":/data/%1.json").arg(QLatin1String(id)));
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("missing game data %1").arg(f.fileName());
            return false;
        }
        QJsonParseError perr{};
        const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &perr).object();
        if (perr.error != QJsonParseError::NoError) {
            if (error) *error = QStringLiteral("%1: %2").arg(f.fileName(), perr.errorString());
            return false;
        }
        Game g;
        g.id = o.value(QStringLiteral("id")).toString();
        g.title = o.value(QStringLiteral("title")).toString();
        g.shortName = o.value(QStringLiteral("short")).toString();
        g.edition = o.value(QStringLiteral("edition")).toString();
        g.titleId = o.value(QStringLiteral("title_id")).toString();
        g.mediaId = o.value(QStringLiteral("media_id")).toString();
        g.defaultExe = o.value(QStringLiteral("default_exe")).toString();
        g.xexSha1 = o.value(QStringLiteral("xex_sha1")).toString();
        g.xexVersion = o.value(QStringLiteral("xex_version")).toString();
        g.discName = o.value(QStringLiteral("disc_name")).toString();
        g.discSha1 = o.value(QStringLiteral("disc_sha1")).toString();
        const QJsonObject tu = o.value(QStringLiteral("title_update")).toObject();
        g.update.name = tu.value(QStringLiteral("name")).toString();
        g.update.package = tu.value(QStringLiteral("package")).toString();
        g.update.xexpSha1 = tu.value(QStringLiteral("xexp_sha1")).toString();
        g.update.fromVersion = tu.value(QStringLiteral("from_version")).toString();
        g.update.toVersion = tu.value(QStringLiteral("to_version")).toString();
        const QJsonObject files = tu.value(QStringLiteral("files")).toObject();
        for (auto it = files.begin(); it != files.end(); ++it) g.update.files.insert(it.key(), it.value().toString());
        g.notes = o.value(QStringLiteral("notes")).toString();
        for (const auto& mv : o.value(QStringLiteral("modes")).toArray()) {
            const QJsonObject mo = mv.toObject();
            GameMode m;
            m.id = mo.value(QStringLiteral("id")).toString();
            m.name = mo.value(QStringLiteral("name")).toString();
            m.description = mo.value(QStringLiteral("description")).toString();
            m.roster = mo.value(QStringLiteral("roster")).toString();
            m.characterSelect = mo.value(QStringLiteral("character_select")).toString(QStringLiteral("launcher"));
            m.online = mo.value(QStringLiteral("online")).toBool();
            m.ranked = mo.value(QStringLiteral("ranked")).toBool();
            for (const auto& r : mo.value(QStringLiteral("rounds")).toArray()) m.rounds.append(r.toInt());
            m.defaultRounds = mo.value(QStringLiteral("default_rounds")).toInt(3);
            m.lobbyOnly = mo.value(QStringLiteral("lobby_only")).toBool();
            g.modes.append(m);
        }
        const QJsonObject rosters = o.value(QStringLiteral("rosters")).toObject();
        for (auto it = rosters.begin(); it != rosters.end(); ++it) {
            QList<Character> list;
            for (const auto& cv : it.value().toArray()) {
                const QJsonObject co = cv.toObject();
                list.append({co.value(QStringLiteral("id")).toString(), co.value(QStringLiteral("name")).toString(),
                             co.value(QStringLiteral("secret")).toBool(), co.value(QStringLiteral("game_id")).toInt(-1)});
                if (list.last().gameId >= 0) g.gameNames.insert(list.last().gameId, list.last().name);
            }
            g.rosters.insert(it.key(), list);
        }
        for (const auto& nv : o.value(QStringLiteral("not_playable")).toArray()) {
            const QJsonObject no = nv.toObject();
            if (const int gameId = no.value(QStringLiteral("game_id")).toInt(-1); gameId >= 0)
                g.gameNames.insert(gameId, no.value(QStringLiteral("name")).toString());
            g.notPlayable.append(QStringLiteral("%1: %2").arg(no.value(QStringLiteral("name")).toString(),
                                                              no.value(QStringLiteral("note")).toString()));
        }
        games_.append(g);
    }
    return true;
}

const Game* GameData::game(const QString& id) const {
    for (const auto& g : games_)
        if (g.id == id) return &g;
    return nullptr;
}

const QList<QPair<QString, QString>>& GameData::regions() {
    static const QList<QPair<QString, QString>> list = {
        {QStringLiteral("na"), QStringLiteral("North America")}, {QStringLiteral("sa"), QStringLiteral("South America")},
        {QStringLiteral("eu"), QStringLiteral("Europe")},        {QStringLiteral("me"), QStringLiteral("Middle East")},
        {QStringLiteral("af"), QStringLiteral("Africa")},        {QStringLiteral("as"), QStringLiteral("Asia")},
        {QStringLiteral("oc"), QStringLiteral("Oceania")},
    };
    return list;
}

QString GameData::regionName(const QString& id) {
    for (const auto& r : regions())
        if (r.first == id) return r.second;
    return id;
}
