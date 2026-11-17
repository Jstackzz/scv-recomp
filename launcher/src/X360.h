#pragma once
// Xbox 360 files the Build game page reads, straight from the player's own files, without Python (the same formats as
// tools/py/x360.py):
//  - Disc images: the Xbox Game Disc file system (GDFX, the original Xbox's XDVDFS): the volume descriptor
//    "MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition, each folder a binary tree of entries. The game
//    partition starts at 0 (an XISO, the partition alone), 0xFD90000 (a whole XGD2 dump) or 0x2080000 (XGD3, which
//    Soulcalibur V is).
//  - STFS packages (LIVE, PIRS, CON): title updates (content type 0x000B0000) and downloadable content (0x00000002).
//    The header names the package's title, content type and versions; the files lie in 4 KB blocks between hash
//    tables, which a reader steps over (free60 / Velocity's block numbering).
//  - XEX2 headers: the executable's (default.xex) execution info, and a title update's delta patch (default.xexp,
//    which turns one version of default.xex into the next).

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

#include <atomic>
#include <functional>

namespace x360 {

// "0.0.1.2" from a packed version (major 4 bits, minor 4, build 16, qfe 8)
QString versionText(quint32 v);

// --- disc images ------------------------------------------------------------------------------------------------

struct Entry {
    QString path;  // relative, '/' separated
    quint32 sector = 0;
    quint32 size = 0;
    bool dir = false;
};

class Disc {
public:
    bool open(const QString& path, QString* error);
    const QList<Entry>& entries() const { return entries_; }
    qint64 base() const { return base_; }  // where the game partition starts in the image
    QString layout() const;                // "Xbox 360 XGD3 disc", "XISO", ...: what that offset says the image is
    // the files' bytes and count, those `skip` leaves out not counted (null: every file)
    qint64 totalBytes(const std::function<bool(const QString&)>& skip = {}) const;
    int fileCount(const std::function<bool(const QString&)>& skip = {}) const;
    const Entry* find(const QString& path) const;  // case-insensitive
    bool read(const Entry& e, QByteArray* out, QString* error);
    // every file under outDir but those `skip` names (a path, '/' separated: a folder leaves out everything in it);
    // each written as <name>.part, then renamed; a file already there with the right size is kept, so a cancelled
    // extraction goes on where it stopped. bytesDone and filesDone count up as it goes; false on an error or when
    // cancel is set.
    bool extractAll(const QString& outDir, const std::function<bool(const QString&)>& skip,
                    const std::atomic<bool>& cancel, std::atomic<qint64>& bytesDone, std::atomic<int>& filesDone,
                    QString* error);

private:
    bool findBase(QString* error);
    bool walk(quint32 sector, quint32 size, const QString& prefix, int depth, QString* error);

    QFile f_;
    qint64 base_ = 0;
    QList<Entry> entries_;
};

// --- STFS packages ----------------------------------------------------------------------------------------------

constexpr quint32 kContentTitleUpdate = 0x000B0000;
constexpr quint32 kContentMarketplace = 0x00000002;  // downloadable content (DLC)
QString contentTypeName(quint32 type);

struct PackageFile {
    QString path;  // '/' separated, as the package's file table nests it
    bool dir = false;
    bool consecutive = false;  // its blocks follow each other (else each one's hash entry names the next)
    quint32 blocks = 0;
    quint32 start = 0;
    qint16 parent = -1;
    quint32 size = 0;
};

class Package {
public:
    bool open(const QString& path, QString* error);
    QString magic() const { return magic_; }  // "LIVE", "PIRS" or "CON"
    quint32 contentType() const { return contentType_; }
    quint32 titleId() const { return titleId_; }
    quint32 mediaId() const { return mediaId_; }
    quint32 version() const { return version_; }
    quint32 baseVersion() const { return baseVersion_; }
    QString displayName() const { return displayName_; }
    QString titleName() const { return titleName_; }
    quint32 headerSize() const { return headerSize_; }
    QByteArray header() const { return header_; }  // the package's header, its first headerSize() bytes
    const QList<PackageFile>& files() const { return files_; }
    const PackageFile* find(const QString& path) const;  // case-insensitive
    qint64 totalBytes() const;
    bool read(const PackageFile& f, QByteArray* out, QString* error);
    // every file under outDir (folders made as the file table has them); false on an error
    bool extractAll(const QString& outDir, QString* error);

private:
    qint64 blockOffset(quint32 block) const;
    qint64 hashEntryOffset(quint32 block) const;
    bool nextBlock(quint32 block, quint32* next, QString* error);
    bool readBlock(quint32 block, QByteArray* out, QString* error);

    QFile f_;
    QString magic_;
    QByteArray header_;
    quint32 headerSize_ = 0, contentType_ = 0, mediaId_ = 0, version_ = 0, baseVersion_ = 0, titleId_ = 0;
    quint8 blockSeparation_ = 0;
    int sex_ = 0;  // 0: one hash table per level ("female"), 1: two ("male")
    quint16 tableBlocks_ = 0;
    quint32 tableBlock_ = 0;
    qint64 firstBlock_ = 0;
    QString displayName_, titleName_;
    QList<PackageFile> files_;
};

// --- XEX2 headers -----------------------------------------------------------------------------------------------

struct XexInfo {
    quint32 moduleFlags = 0;
    bool patch = false;  // a delta patch (default.xexp), module flags 0x10 or 0x20 (0x50: patch, title module)
    bool hasExecution = false;
    quint32 titleId = 0, mediaId = 0, version = 0, baseVersion = 0;
    int disc = 0, discs = 0;
    bool hasDelta = false;  // the delta patch descriptor: which version it patches, into which
    quint32 deltaSource = 0, deltaTarget = 0;
    QByteArray deltaSourceDigest;
    QString originalName;  // the PE name the executable was built as
};
bool readXex(const QByteArray& data, XexInfo* info, QString* error);

}  // namespace x360
