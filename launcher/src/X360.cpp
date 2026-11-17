#include "X360.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace x360 {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

// a name from a disc or package that is safe to make a file of: no separators, no drive, not . or ..
bool safeName(const QString& name) {
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) return false;
    for (QChar c : name)
        if (c == u'/' || c == u'\\' || c == u':' || c.unicode() < 0x20) return false;
    return true;
}

quint16 le16(const char* p) { return qFromLittleEndian<quint16>(p); }
quint32 le32(const char* p) { return qFromLittleEndian<quint32>(p); }
quint32 le24(const char* p) { return quint32(quint8(p[0])) | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])) << 16; }
quint32 be24(const char* p) { return quint32(quint8(p[0])) << 16 | quint32(quint8(p[1])) << 8 | quint32(quint8(p[2])); }
quint32 be32(const char* p) { return qFromBigEndian<quint32>(p); }
quint16 be16(const char* p) { return qFromBigEndian<quint16>(p); }

// --- disc
constexpr qint64 kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";  // the volume descriptor, at sector 32 of the file system
constexpr int kMagicSize = 20;
constexpr quint8 kAttrDirectory = 0x10;
struct KnownBase {
    qint64 offset;
    const char* layout;
};
// where the game partition starts: an XISO (the partition alone), then whole-disc dumps: XGD2, XGD3, original XGD1
constexpr KnownBase kKnownBases[] = {
    {0, "XISO (the game partition alone)"},
    {0xFD90000, "Xbox 360 XGD2 disc"},
    {0x2080000, "Xbox 360 XGD3 disc"},
    {0x18300000, "original Xbox XGD1 disc"},
};
constexpr qint64 kScanLimit = qint64(2) << 30;
constexpr int kMaxDepth = 32;
constexpr int kMaxEntries = 200000;
constexpr qint64 kChunk = 4 << 20;

// --- packages
constexpr qint64 kBlock = 0x1000;
constexpr int kHeaderRead = 0x971A + 0x1000;  // the header, its metadata and the first hash table's start
constexpr int kMaxPackageFiles = 65536;

}  // namespace

QString versionText(quint32 v) {
    return QStringLiteral("%1.%2.%3.%4").arg((v >> 28) & 0xF).arg((v >> 24) & 0xF).arg((v >> 8) & 0xFFFF).arg(v & 0xFF);
}

// --- Disc ----------------------------------------------------------------------------------------------------

bool Disc::open(const QString& path, QString* error) {
    f_.setFileName(path);
    if (!f_.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot open %1: %2").arg(native(path), f_.errorString());
        return false;
    }
    if (!findBase(error)) return false;
    QByteArray vd;
    if (!f_.seek(base_ + 32 * kSector) || (vd = f_.read(28)).size() != 28) {
        *error = QStringLiteral("The image is cut short.");
        return false;
    }
    entries_.clear();
    return walk(le32(vd.constData() + 20), le32(vd.constData() + 24), QString(), 0, error);
}

QString Disc::layout() const {
    for (const KnownBase& b : kKnownBases)
        if (b.offset == base_) return QLatin1String(b.layout);
    return QStringLiteral("a disc dump with its game partition at an unusual offset");
}

bool Disc::findBase(QString* error) {
    const QByteArray magic(kMagic, kMagicSize);
    for (const KnownBase& b : kKnownBases) {
        if (f_.seek(b.offset + 32 * kSector) && f_.read(kMagicSize) == magic) {
            base_ = b.offset;
            return true;
        }
    }
    // else the volume descriptor at any sector of the first part of the image
    for (qint64 pos = 0; pos < kScanLimit; pos += kChunk) {
        if (!f_.seek(pos)) break;
        const QByteArray data = f_.read(kChunk);
        if (data.size() < kMagicSize) break;
        for (qsizetype at = 0; at + kMagicSize <= data.size(); at += kSector) {
            if (pos + at >= 32 * kSector && std::memcmp(data.constData() + at, kMagic, kMagicSize) == 0) {
                base_ = pos + at - 32 * kSector;
                return true;
            }
        }
    }
    *error = QStringLiteral("This is not an Xbox 360 disc image (no Xbox file system in it).");
    return false;
}

bool Disc::walk(quint32 sector, quint32 size, const QString& prefix, int depth, QString* error) {
    if (size == 0) return true;
    if (depth > kMaxDepth) {
        *error = QStringLiteral("The disc's folders nest too deep.");
        return false;
    }
    QByteArray data;
    if (!f_.seek(base_ + qint64(sector) * kSector) || (data = f_.read(size)).size() != qsizetype(size)) {
        *error = QStringLiteral("The image is cut short (a folder lies past its end).");
        return false;
    }
    // each folder is a binary tree of entries: u16 left, u16 right (in 4-byte units), u32 sector, u32 size,
    // u8 attributes, u8 name length, the name
    QList<qsizetype> stack{0};
    QSet<qsizetype> seen;
    while (!stack.isEmpty()) {
        const qsizetype off = stack.takeLast();
        if (seen.contains(off) || off + 14 > data.size()) continue;
        seen.insert(off);
        const char* p = data.constData() + off;
        const quint16 left = le16(p), right = le16(p + 2);
        if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
        const quint8 attrs = quint8(p[12]), nameLen = quint8(p[13]);
        if (off + 14 + nameLen > data.size()) continue;
        const QString name = QString::fromLatin1(p + 14, nameLen);
        if (!safeName(name)) {
            *error = QStringLiteral("The disc has a file name that cannot be used: %1").arg(name);
            return false;
        }
        if (entries_.size() >= kMaxEntries) {
            *error = QStringLiteral("The disc lists too many files.");
            return false;
        }
        const Entry e{prefix + name, le32(p + 4), le32(p + 8), (attrs & kAttrDirectory) != 0};
        entries_.append(e);
        if (e.dir && !walk(e.sector, e.size, e.path + u'/', depth + 1, error)) return false;
        if (left) stack.append(qsizetype(left) * 4);
        if (right) stack.append(qsizetype(right) * 4);
    }
    return true;
}

qint64 Disc::totalBytes(const std::function<bool(const QString&)>& skip) const {
    qint64 n = 0;
    for (const Entry& e : entries_)
        if (!e.dir && !(skip && skip(e.path))) n += e.size;
    return n;
}

int Disc::fileCount(const std::function<bool(const QString&)>& skip) const {
    return int(std::count_if(entries_.cbegin(), entries_.cend(),
                             [&skip](const Entry& e) { return !e.dir && !(skip && skip(e.path)); }));
}

const Entry* Disc::find(const QString& path) const {
    for (const Entry& e : entries_)
        if (!e.dir && e.path.compare(path, Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

bool Disc::read(const Entry& e, QByteArray* out, QString* error) {
    if (!f_.seek(base_ + qint64(e.sector) * kSector) || (*out = f_.read(e.size)).size() != qsizetype(e.size)) {
        *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e.path);
        return false;
    }
    return true;
}

bool Disc::extractAll(const QString& outDir, const std::function<bool(const QString&)>& skip,
                      const std::atomic<bool>& cancel, std::atomic<qint64>& bytesDone, std::atomic<int>& filesDone,
                      QString* error) {
    const QDir out(outDir);
    // in the order the files lie on the disc: one pass from start to end
    QList<const Entry*> order;
    for (const Entry& e : entries_)
        if (!(skip && skip(e.path))) order.append(&e);
    std::stable_sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) { return a->sector < b->sector; });
    QByteArray buf(kChunk, Qt::Uninitialized);
    for (const Entry* e : order) {
        const QString target = out.filePath(e->path);
        if (e->dir) {
            if (!QDir().mkpath(target)) {
                *error = QStringLiteral("Cannot make the folder %1.").arg(native(target));
                return false;
            }
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            *error = QStringLiteral("Cannot make the folder %1.").arg(native(QFileInfo(target).absolutePath()));
            return false;
        }
        // there already, whole: an extraction that stopped before, or the same disc again
        if (QFileInfo(target).size() == qint64(e->size) && QFileInfo(target).isFile()) {
            bytesDone += e->size;
            ++filesDone;
            continue;
        }
        const QString part = target + QStringLiteral(".part");
        QFile dst(part);
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *error = QStringLiteral("Cannot write %1: %2").arg(native(part), dst.errorString());
            return false;
        }
        if (!f_.seek(base_ + qint64(e->sector) * kSector)) {
            *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e->path);
            return false;
        }
        for (qint64 left = e->size; left > 0;) {
            if (cancel) {
                dst.close();
                dst.remove();
                *error = QStringLiteral("Cancelled.");
                return false;
            }
            const qint64 n = qMin(left, kChunk);
            if (f_.read(buf.data(), n) != n) {
                dst.close();
                dst.remove();
                *error = QStringLiteral("The image is cut short (%1 lies past its end).").arg(e->path);
                return false;
            }
            if (dst.write(buf.constData(), n) != n) {
                const QString why = dst.errorString();
                dst.close();
                dst.remove();
                *error = QStringLiteral("Cannot write %1: %2 (is the disk full?)").arg(native(target), why);
                return false;
            }
            left -= n;
            bytesDone += n;
        }
        dst.close();
        QFile::remove(target);
        if (!QFile::rename(part, target)) {
            *error = QStringLiteral("Cannot put %1 in place.").arg(native(target));
            return false;
        }
        ++filesDone;
    }
    return true;
}

// --- Package -------------------------------------------------------------------------------------------------

QString contentTypeName(quint32 type) {
    switch (type) {
    case 0x000B0000: return QStringLiteral("title update");
    case 0x00000002: return QStringLiteral("downloadable content");
    case 0x00000001: return QStringLiteral("saved game");
    case 0x00080000: return QStringLiteral("game demo");
    case 0x00030000: return QStringLiteral("theme");
    case 0x00004000: return QStringLiteral("installed game");
    case 0x00007000: return QStringLiteral("game on demand");
    case 0x00090000: return QStringLiteral("video");
    case 0x00020000: return QStringLiteral("gamer picture");
    default: return QStringLiteral("content type %1").arg(type, 8, 16, QLatin1Char('0')).toUpper();
    }
}

bool Package::open(const QString& path, QString* error) {
    f_.setFileName(path);
    if (!f_.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot open %1: %2").arg(native(path), f_.errorString());
        return false;
    }
    const QByteArray h = f_.read(kHeaderRead);
    if (h.size() < 0x1691 + 0x80 || !(h.startsWith("LIVE") || h.startsWith("PIRS") || h.startsWith("CON "))) {
        *error = QStringLiteral("%1 is not an Xbox 360 package (no LIVE, PIRS or CON header).").arg(QFileInfo(path).fileName());
        return false;
    }
    const char* p = h.constData();
    magic_ = QString::fromLatin1(p, 4).trimmed();
    headerSize_ = be32(p + 0x340);
    contentType_ = be32(p + 0x344);
    mediaId_ = be32(p + 0x354);
    version_ = be32(p + 0x358);
    baseVersion_ = be32(p + 0x35C);
    titleId_ = be32(p + 0x360);
    // the STFS volume descriptor: size, reserved, block separation, file table blocks (u16 LE) and first block (u24 LE)
    const char* vd = p + 0x379;
    blockSeparation_ = quint8(vd[2]);
    tableBlocks_ = le16(vd + 3);
    tableBlock_ = le24(vd + 5);
    sex_ = (~blockSeparation_) & 1;
    firstBlock_ = (qint64(headerSize_) + 0xFFF) & ~qint64(0xFFF);
    const auto utf16be = [](const char* at, int bytes) {
        QString s;
        for (int i = 0; i + 1 < bytes; i += 2) {
            const char16_t c = be16(at + i);
            if (!c) break;
            s.append(QChar(c));
        }
        return s;
    };
    displayName_ = utf16be(p + 0x411, 0x80);
    titleName_ = utf16be(p + 0x1691, 0x80);
    if (headerSize_ < 0x971A || headerSize_ > 0x100000 || tableBlocks_ == 0 || tableBlocks_ > 0x400) {
        *error = QStringLiteral("%1 has a package header this reader does not know (an SVOD package, or a damaged "
                                "file).").arg(QFileInfo(path).fileName());
        return false;
    }
    if (!f_.seek(0) || (header_ = f_.read(headerSize_)).size() != qsizetype(headerSize_)) {
        *error = QStringLiteral("%1 is cut short.").arg(QFileInfo(path).fileName());
        return false;
    }
    // the file table: its blocks, chained through the hash tables
    QByteArray table;
    quint32 block = tableBlock_;
    for (int i = 0; i < tableBlocks_; ++i) {
        QByteArray b;
        if (!readBlock(block, &b, error)) return false;
        table += b;
        if (i + 1 < tableBlocks_ && !nextBlock(block, &block, error)) return false;
    }
    files_.clear();
    for (qsizetype i = 0; i + 0x40 <= table.size(); i += 0x40) {
        const char* e = table.constData() + i;
        const int nameLen = quint8(e[0x28]) & 0x3F;
        if (nameLen == 0) continue;
        PackageFile f;
        f.path = QString::fromLatin1(e, nameLen);  // its own name for now: the parents' come below
        f.dir = (quint8(e[0x28]) & 0x80) != 0;
        f.consecutive = (quint8(e[0x28]) & 0x40) != 0;
        f.blocks = le24(e + 0x29);
        f.start = le24(e + 0x2F);
        f.parent = qint16(be16(e + 0x32));
        f.size = be32(e + 0x34);
        if (!safeName(f.path)) {
            *error = QStringLiteral("The package has a file name that cannot be used: %1").arg(f.path);
            return false;
        }
        if (files_.size() >= kMaxPackageFiles) {
            *error = QStringLiteral("The package lists too many files.");
            return false;
        }
        files_.append(f);
    }
    // each file's path from its parents (an index into the table; -1 the root)
    QStringList names;
    for (const PackageFile& f : files_) names.append(f.path);
    for (PackageFile& f : files_) {
        QStringList parts{f.path};
        int parent = f.parent, guard = 0;
        while (parent != -1 && parent >= 0 && parent < names.size() && guard++ < 64) {
            parts.prepend(names[parent]);
            parent = files_[parent].parent;
        }
        f.path = parts.join(u'/');
    }
    return true;
}

// block number -> where it lies in the file, past the hash tables before it (free60 / Velocity
// "ComputeBackingDataBlockNumber")
qint64 Package::blockOffset(quint32 block) const {
    qint64 r = (qint64((block + 0xAA) / 0xAA) << sex_) + block;
    if (block >= 0xAA) {
        r += qint64((block + 0x70E4) / 0x70E4) << sex_;
        if (block >= 0x70E4) r += qint64(1) << sex_;
    }
    return firstBlock_ + (r << 12);
}

// the block's entry in its level-0 hash table, whose last 3 bytes name the next block of a chained file
qint64 Package::hashEntryOffset(quint32 block) const {
    qint64 hb = 0;
    if (block >= 0xAA) {
        const qint64 step0 = sex_ == 0 ? 0xAB : 0xAC;
        hb = qint64(block / 0xAA) * step0;
        hb += qint64(block / 0x70E4 + 1) << sex_;
        if (block / 0x70E4) hb += qint64(1) << sex_;
    }
    qint64 off = firstBlock_ + (hb << 12);
    if (sex_ == 1 && (blockSeparation_ & 2)) off += kBlock;  // a male package using its second table
    return off + qint64(block % 0xAA) * 0x18;
}

bool Package::nextBlock(quint32 block, quint32* next, QString* error) {
    QByteArray b;
    if (!f_.seek(hashEntryOffset(block) + 0x15) || (b = f_.read(3)).size() != 3) {
        *error = QStringLiteral("The package is cut short (a hash table lies past its end).");
        return false;
    }
    *next = be24(b.constData());
    return true;
}

bool Package::readBlock(quint32 block, QByteArray* out, QString* error) {
    if (!f_.seek(blockOffset(block)) || (*out = f_.read(kBlock)).isEmpty()) {
        *error = QStringLiteral("The package is cut short (block %1 lies past its end).").arg(block);
        return false;
    }
    return true;
}

const PackageFile* Package::find(const QString& path) const {
    for (const PackageFile& f : files_)
        if (!f.dir && f.path.compare(path, Qt::CaseInsensitive) == 0) return &f;
    return nullptr;
}

qint64 Package::totalBytes() const {
    qint64 n = 0;
    for (const PackageFile& f : files_)
        if (!f.dir) n += f.size;
    return n;
}

bool Package::read(const PackageFile& f, QByteArray* out, QString* error) {
    out->clear();
    if (f.dir) return true;
    out->reserve(qsizetype(f.size));
    qint64 left = f.size;
    quint32 block = f.start;
    int guard = 0;
    while (left > 0) {
        QByteArray b;
        if (!readBlock(block, &b, error)) return false;
        const qint64 take = qMin<qint64>(left, kBlock);
        if (b.size() < take) {
            *error = QStringLiteral("The package is cut short (%1 lies past its end).").arg(f.path);
            return false;
        }
        out->append(b.constData(), take);
        left -= take;
        if (left > 0) {
            if (f.consecutive) ++block;
            else if (!nextBlock(block, &block, error)) return false;
        }
        if (++guard > 0x100000) {  // 4 GB of blocks: a chain that loops
            *error = QStringLiteral("The package's block chain for %1 loops.").arg(f.path);
            return false;
        }
    }
    return true;
}

bool Package::extractAll(const QString& outDir, QString* error) {
    const QDir out(outDir);
    for (const PackageFile& f : files_) {
        const QString target = out.filePath(f.path);
        if (f.dir) {
            if (!QDir().mkpath(target)) {
                *error = QStringLiteral("Cannot make the folder %1.").arg(native(target));
                return false;
            }
            continue;
        }
        QByteArray data;
        if (!read(f, &data, error)) return false;
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            *error = QStringLiteral("Cannot make the folder %1.").arg(native(QFileInfo(target).absolutePath()));
            return false;
        }
        QFile dst(target + QStringLiteral(".part"));
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate) || dst.write(data) != data.size()) {
            *error = QStringLiteral("Cannot write %1: %2 (is the disk full?)").arg(native(target), dst.errorString());
            dst.close();
            dst.remove();
            return false;
        }
        dst.close();
        QFile::remove(target);
        if (!QFile::rename(dst.fileName(), target)) {
            *error = QStringLiteral("Cannot put %1 in place.").arg(native(target));
            return false;
        }
    }
    return true;
}

// --- XEX ----------------------------------------------------------------------------------------------------

bool readXex(const QByteArray& d, XexInfo* info, QString* error) {
    *info = XexInfo{};
    if (d.size() < 24 || !d.startsWith("XEX2")) {
        *error = QStringLiteral("not an Xbox 360 executable (no XEX2 header)");
        return false;
    }
    const char* p = d.constData();
    info->moduleFlags = be32(p + 4);
    info->patch = (info->moduleFlags & 0x30) != 0;  // a delta (0x20) or full (0x10) patch
    const quint32 count = be32(p + 20);
    if (count > 4096 || 24 + qint64(count) * 8 > d.size()) {
        *error = QStringLiteral("its XEX2 header is damaged");
        return false;
    }
    const auto in = [&d](qint64 at, qint64 n) { return at >= 0 && at + n <= d.size(); };
    for (quint32 i = 0; i < count; ++i) {
        const quint32 key = be32(p + 24 + i * 8), val = be32(p + 28 + i * 8);
        if (key == 0x40006 && in(val, 24)) {  // execution info
            info->hasExecution = true;
            info->mediaId = be32(p + val);
            info->version = be32(p + val + 4);
            info->baseVersion = be32(p + val + 8);
            info->titleId = be32(p + val + 12);
            info->disc = quint8(p[val + 18]);
            info->discs = quint8(p[val + 19]);
        } else if (key == 0x5FF && in(val, 0x20)) {  // delta patch descriptor
            info->hasDelta = true;
            info->deltaTarget = be32(p + val + 4);
            info->deltaSource = be32(p + val + 8);
            info->deltaSourceDigest = d.mid(val + 0xC, 0x14);
        } else if (key == 0x183FF && in(val, 4)) {  // original PE name
            const quint32 n = be32(p + val);
            if (n > 4 && in(val, n)) info->originalName = QString::fromLatin1(p + val + 4, int(n - 4)).section(QChar(0), 0, 0);
        }
    }
    return true;
}

}  // namespace x360
