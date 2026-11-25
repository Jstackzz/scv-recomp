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

// a title update of the game: a LIVE package of the title's whose default.xexp turns the disc's default.xex
// (fromVersion) into toVersion; its other files go to the update folder
struct TitleUpdate {
    int number = 0;        // 3: title update 3, as the console's list of the game's updates counts them
    QString package;       // the file name Xbox Live gave it (TU_...), shown to the player
    QString packageNote;   // how to read that name ("it ends in the capital letter O and the number 2"), or empty
    QString xexpSha1;
    QString fromVersion, toVersion;
    QHash<QString, QString> files;  // the package's files the build needs (name -> SHA-1, empty: any)
    QString name() const;           // "title update 3", for the middle of a sentence
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
    TitleUpdate update;               // the one the build takes (both players of a match must have the same)
    QList<TitleUpdate> olderUpdates;  // those it took before (title update 1 until 0.1.5), named when a player has one
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
    // which title update a default.xexp is, by its SHA-1: the one the build takes, an older one, or null
    const TitleUpdate* knownUpdate(const QString& xexpSha1) const;
    // what to tell a player whose title update is not the one the build takes, in plain words: `what` says what they
    // have ("The file you chose, TU_..., is"), then which update it is (known by `xexpSha1`, else by the version
    // `toVersion` it makes the game), that SCV Online needs the other one, its file's name and where to choose it
    QString otherUpdate(const QString& what, const QString& xexpSha1, const QString& toVersion) const;
    // the end of that: SCV Online needs this update, what its file is called, where a player has it
    QString updateNeeded() const;
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
