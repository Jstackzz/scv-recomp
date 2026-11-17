#pragma once
// The game, its modes and roster, from the bundled launcher/data/sc5.json: Soulcalibur V (Xbox 360), and what the
// Build game page accepts as the player's disc, title update and DLC.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

struct Character {
    QString id;
    QString name;
    bool secret = false;  // unlocked by playing (every online match and practice has it unlocked)
    int gameId = -1;      // the game's own character id, as its result lines name fighters ("3.0": 3, costume 0)
};

struct GameMode {
    QString id;
    QString name;
    QString description;
    QString roster;
    QString characterSelect;  // "launcher" or "in_game"
    bool online = false;
    bool ranked = false;
    QList<int> rounds;
    int defaultRounds = 3;
    bool lobbyOnly = false;   // played in a lobby a friend joins by its code, never searched for (Online Practice)
    bool picksInGame() const { return characterSelect == QLatin1String("in_game"); }
};

// the title update the build is made with: a LIVE package of the title's whose default.xexp turns the disc's
// default.xex (fromVersion) into toVersion; its other files go to the update folder
struct TitleUpdate {
    QString name;          // "Title Update 1"
    QString package;       // the file name Xbox Live gave it (TU_...), shown to the player
    QString xexpSha1;
    QString fromVersion, toVersion;
    QHash<QString, QString> files;  // the package's files the build needs (name -> SHA-1, empty: any)
};

struct Game {
    QString id;
    QString title;
    QString shortName;
    QString edition;
    QString titleId;   // "4E4D083D"
    QString mediaId;
    QString defaultExe;
    QString xexSha1;     // the disc's default.xex this build is made from
    QString xexVersion;  // its version, "0.0.0.2"
    QString discName;    // what the disc image is called (Redump), for the player
    QString discSha1;    // the whole image's SHA-1 (Redump), not checked (8 GB), named in the help
    TitleUpdate update;
    QString notes;
    QList<GameMode> modes;
    QHash<QString, QList<Character>> rosters;
    QStringList notPlayable;  // "<name>: <why>", e.g. downloadable characters only the player's own DLC brings
    QHash<int, QString> gameNames;  // the game's character ids -> names, the not playable ones included

    const GameMode* mode(const QString& modeId) const;
    QList<GameMode> onlineModes() const;
    QList<Character> roster(const QString& modeId) const;
    // a roster id's name, or a fighter the game named ("3.0", "21": its own id, the costume after a dot); unknown ids
    // come back as they are
    QString characterName(const QString& characterId) const;
    quint32 titleIdValue() const { return titleId.toUInt(nullptr, 16); }
    // what a disc's default.xex is, when not the one this build is made from (empty when it is): another game,
    // region, version or a modified copy, none of which the build takes (both players of a match must run the same
    // code). For the player.
    QString otherRelease(const QByteArray& xex) const;
};

class GameData {
public:
    static GameData& instance();
    bool load(QString* error);
    const QList<Game>& games() const { return games_; }
    const Game* game(const QString& id) const;

    static const QList<QPair<QString, QString>>& regions();  // id, display name
    static QString regionName(const QString& id);

private:
    QList<Game> games_;
};
