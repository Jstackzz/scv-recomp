#include "GameProcess.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <windows.h>

#include "AppPaths.h"
#include "Controls.h"
#include "GameData.h"
#include "WinUtil.h"

namespace {

// the game's files: the extracted disc's default.xex with the title update's default.xexp beside it (the runtime
// applies the update as it loads the executable, docs/PROTOCOL.md 3.1)
bool hasGameFiles(const QString& gameId) {
    const QDir disc(cfg::gameDataDir(gameId));
    return disc.exists(QStringLiteral("default.xex")) && disc.exists(QStringLiteral("default.xexp"));
}

}  // namespace

GameProcess::GameProcess(const QString& gameId, int instance, QObject* parent)
    : QObject(parent), gameId_(gameId), instance_(instance) {
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, &GameProcess::poll);
}

GameProcess::~GameProcess() {
    if (isRunning()) {
        proc_->kill();
        proc_->waitForFinished(2000);
    }
}

bool GameProcess::isRunning() const { return proc_ && proc_->state() != QProcess::NotRunning; }

qint64 GameProcess::pid() const { return isRunning() ? proc_->processId() : 0; }

QString GameProcess::state() const { return status_.section(QLatin1Char(' '), 0, 0); }

QHash<QString, QString> GameProcess::parseKeys(const QString& line) {
    QHash<QString, QString> out;
    const QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 1; i < tokens.size(); ++i) {
        const int eq = tokens[i].indexOf(QLatin1Char('='));
        if (eq > 0) out.insert(tokens[i].left(eq), tokens[i].mid(eq + 1));
    }
    return out;
}

QString GameProcess::smartAppControlHelp(bool blocked) {
    return (blocked ? tr("Windows' Smart App Control blocked the game.")
                    : tr("Windows' Smart App Control is on, and it will block the games.")) +
           QLatin1Char(' ') +
           tr("It only runs programs signed by a publisher it knows, and these games can never be: every PC builds "
              "its own copy of the game's code from its own disc. To play, switch it off in Windows Security > App & "
              "browser control > Smart App Control settings. (On some versions of Windows 11 it can only be "
              "switched back on by resetting Windows.)");
}

QString GameProcess::antivirusHelp(const QString& file) {
    return tr("Windows Security (or another antivirus) may have blocked or removed %1. That is a false alarm: a new "
              "release's files, and the game code each PC builds for itself, are new to it. Open Windows Security > "
              "Virus & threat protection > Protection history, pick the entry for %1 and choose Actions > Restore (or "
              "Allow on device), then try again.")
        .arg(file);
}

bool GameProcess::codeStale(const QString& exe) {
    const QFileInfo fi(exe);
    if (fi.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) != 0 || !fi.exists()) return false;
    const QDir dir(fi.absolutePath());
    const QFileInfo dll(dir.filePath(fi.completeBaseName() + QStringLiteral(".dll")));
    if (!dll.exists()) return false;  // never built: its first start builds it
    QFile stamp(dir.filePath(QStringLiteral("compiler/work/%1/built.txt").arg(fi.completeBaseName())));
    // a DLL with no stamp (build.bat's) is current when newer than the executable
    if (!stamp.open(QIODevice::ReadOnly)) return dll.lastModified() <= fi.lastModified();
    // "<the executable's size> <its last write time, a FILETIME> <the xbe's SHA-1>" (StampFor)
    const QList<QByteArray> fields = stamp.readLine().trimmed().split(' ');
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const std::wstring path = QDir::toNativeSeparators(fi.absoluteFilePath()).toStdWString();
    if (fields.size() < 2 || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return true;
    const quint64 size = (quint64(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const quint64 time = (quint64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return fields[0] != QByteArray::number(size) || fields[1] != QByteArray::number(time);
}

QString GameProcess::xexFile(const QString& gameId) {
    const QString xex = QDir(cfg::gameDataDir(gameId)).filePath(QStringLiteral("default.xex"));
    return QFileInfo::exists(xex) ? xex : QString();
}

bool GameProcess::start(const QString& exe, QString* error, bool netMode) {
    if (isRunning()) return true;
    const QFileInfo fi(exe);
    if (exe.isEmpty() || !fi.exists()) {
        if (error && !exe.isEmpty() && !paths::packagedGameFolder(gameId_).isEmpty())  // a package has it: taken
            *error = tr("%1 is missing.").arg(fi.fileName()) + QLatin1Char(' ') + antivirusHelp(fi.fileName());
        else if (error)
            *error = tr("Game executable not found: %1. Build the game first or set its path in Settings.")
                         .arg(QDir::toNativeSeparators(exe));
        return false;
    }
    if (fi.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0 && !hasGameFiles(gameId_)) {
        if (error)
            *error = tr("The game's files are not there yet: open Build game and extract your disc and its title "
                        "update first (%1).").arg(QDir::toNativeSeparators(cfg::gameDataDir(gameId_)));
        return false;
    }
    // the newest sessions are kept (an online match's netplay log is about a megabyte)
    constexpr int kKeepSessions = 30;
    const QFileInfoList old = QDir(paths::sessionsDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (int i = kKeepSessions; i < old.size(); i++) QDir(old[i].absoluteFilePath()).removeRecursively();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    // a folder of its own: two games started within the same second (a match's game and practice after a set) shared
    // one, its log and the files the launcher and the game talk through
    const QString name = QStringLiteral("%1-%2-%3").arg(gameId_, stamp).arg(instance_);
    sessionDir_ = QDir(paths::sessionsDir()).filePath(name);
    for (int n = 2; QFileInfo::exists(sessionDir_); n++)
        sessionDir_ = QDir(paths::sessionsDir()).filePath(QStringLiteral("%1-%2").arg(name).arg(n));
    QDir().mkpath(sessionDir_);
    cmdPath_ = QDir(sessionDir_).filePath(QStringLiteral("cmd.txt"));
    statusPath_ = QDir(sessionDir_).filePath(QStringLiteral("status.txt"));
    QFile::remove(cmdPath_);
    QFile::remove(statusPath_);

    netMode_ = netMode;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (netMode) {
        env.insert(QStringLiteral("SC5_NET"), QStringLiteral("1"));
        env.insert(QStringLiteral("SC5_CMD"), QDir::toNativeSeparators(cmdPath_));
        env.insert(QStringLiteral("SC5_STATUS"), QDir::toNativeSeparators(statusPath_));
        // every online match's frames (both players' inputs, the state checks) beside the session's other files: what
        // to look at when a match went wrong; a test that names its own log keeps it
        const QString netLog = gameId_.toUpper() + QStringLiteral("_NET_LOG");
        if (!env.contains(netLog))
            env.insert(netLog, QDir::toNativeSeparators(QDir(sessionDir_).filePath(QStringLiteral("net-{match}.log"))));
    }
    env.insert(QStringLiteral("SC5_GAME"), gameId_);
    env.insert(QStringLiteral("SC5_SESSION_DIR"), QDir::toNativeSeparators(sessionDir_));
    env.insert(QStringLiteral("SC5_INSTANCE"), QString::number(instance_));
    env.insert(QStringLiteral("SC5_LOG"), QDir::toNativeSeparators(QDir(sessionDir_).filePath(QStringLiteral("game.log"))));
    // the game's files (Build game lays them out): the disc with the title update's default.xexp beside default.xex,
    // the update's other files, the player's own DLC, the saves and the shader cache. An online game (netMode) saves
    // into a fresh folder of its session, so both players' games start from the same save; the cache is kept.
    env.insert(QStringLiteral("SC5_GAME_DIR"), QDir::toNativeSeparators(cfg::gameDataDir(gameId_)));
    env.insert(QStringLiteral("SC5_UPDATE_DIR"), QDir::toNativeSeparators(cfg::gameUpdateDir(gameId_)));
    env.insert(QStringLiteral("SC5_DLC_DIR"), QDir::toNativeSeparators(cfg::gameDlcDir(gameId_)));
    const QString userDir = netMode ? QDir(sessionDir_).filePath(QStringLiteral("user")) : cfg::gameUserDir(gameId_);
    QDir().mkpath(userDir);
    env.insert(QStringLiteral("SC5_USER_DIR"), QDir::toNativeSeparators(userDir));
    env.insert(QStringLiteral("SC5_CACHE_DIR"), QDir::toNativeSeparators(cfg::gameCacheDir(gameId_)));
    // F3 in the game saves what it drew (the frame's draws, textures and picture, then the next frames' pictures)
    // under the session: what a player who sees something drawn wrong (flicker, a broken texture) sends
    env.insert(QStringLiteral("SC5_CAPTURE_DIR"), QDir::toNativeSeparators(QDir(sessionDir_).filePath(QStringLiteral("captures"))));
    // the display settings: only how a frame looks, never what the game computes (docs/PROTOCOL.md 3.1)
    env.insert(QStringLiteral("SC5_DISPLAY"), cfg::displayMode());
    env.insert(QStringLiteral("SC5_RESOLUTION"), cfg::resolution());
    env.insert(QStringLiteral("SC5_RENDER_SCALE"), QString::number(cfg::renderScale()));
    env.insert(QStringLiteral("SC5_ANTI_ALIASING"), cfg::antiAliasing());
    env.insert(QStringLiteral("SC5_ANISOTROPIC"), QString::number(cfg::anisotropic()));
    env.insert(QStringLiteral("SC5_VSYNC"), cfg::vsync() ? QStringLiteral("1") : QStringLiteral("0"));
    env.insert(QStringLiteral("SC5_ADAPTIVE_SYNC"), cfg::adaptiveSync() ? QStringLiteral("1") : QStringLiteral("0"));
    env.insert(QStringLiteral("SC5_BRIGHTNESS"), QString::number(cfg::brightness()));
    env.insert(QStringLiteral("SC5_FRAMEDATA"), cfg::frameData() ? QStringLiteral("1") : QStringLiteral("0"));
    for (const QString& level : cfg::volumeNames())  // the sound levels (Settings, SOUND)
        env.insert(QStringLiteral("SC5_VOLUME_") + level.toUpper(), QString::number(cfg::volume(level)));
    // the same settings again whenever the player changes them, for the game that runs on (it keeps watching the file)
    cfg::writeDisplayFile();
    env.insert(QStringLiteral("SC5_DISPLAY_FILE"), QDir::toNativeSeparators(paths::displayFile()));
    env.insert(QStringLiteral("SC5_CONTROLS"), QDir::toNativeSeparators(controls::file()));  // read again when it changes
    for (auto it = extraEnv_.cbegin(); it != extraEnv_.cend(); ++it) env.insert(it.key(), it.value());

    proc_ = new QProcess(this);
    proc_->setProcessEnvironment(env);
    proc_->setWorkingDirectory(fi.absolutePath());
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    QString program = fi.absoluteFilePath();
    QStringList args = extraArgs_;
    if (fi.suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) == 0) {
        program = cfg::python();
        args = {QStringLiteral("-u"), fi.absoluteFilePath()};
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;  // the development stand-in has no window of its own
        });
    } else if (cfg::devBackground()) {
        // tests on a PC someone uses: the game's window comes up minimized, without taking the foreground
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            a->startupInfo->wShowWindow = SW_SHOWMINNOACTIVE;
        });
    }
    connect(proc_, &QProcess::readyRead, this, [this] {
        outBuf_.append(proc_->readAll());
        qsizetype nl;
        while ((nl = outBuf_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(outBuf_.left(nl)).trimmed();
            outBuf_.remove(0, nl + 1);
            if (!line.isEmpty()) emit outputLine(line);
        }
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        poll();
        poll_.stop();
        applog::write(QStringLiteral("game %1/%2: exited with code %3%4")
                          .arg(gameId_).arg(instance_).arg(code).arg(st == QProcess::CrashExit ? " (crash)" : ""));
        const QString last = quitting_ ? QStringLiteral("exited") : QStringLiteral("error exited");
        exitCode_ = code;
        status_ = last;
        emit statusChanged(last);
        emit exited(code, st == QProcess::CrashExit);
    });
    proc_->start(program, args);
    if (!proc_->waitForStarted(10000)) {
        if (error) {
            *error = tr("Could not start %1: %2").arg(QDir::toNativeSeparators(program), proc_->errorString());
            // "An Application Control policy has blocked this file" (English Windows) or the policy reported on
            if (winutil::smartAppControl() == 1 || proc_->errorString().contains(QLatin1String("Application Control")))
                *error += QLatin1Char('\n') + smartAppControlHelp();
            else if (proc_->errorString().contains(QLatin1String("virus"), Qt::CaseInsensitive))  // ERROR_VIRUS_INFECTED
                *error += QLatin1Char('\n') + antivirusHelp(fi.fileName());
        }
        // one arg() call: Windows' messages can hold a %1 of their own, which a second call would fill
        applog::write(QStringLiteral("game %1/%2: could not start %3: %4 (Smart App Control %5)")
                          .arg(gameId_, QString::number(instance_), QDir::toNativeSeparators(program),
                               proc_->errorString(), QString::number(winutil::smartAppControl())));
        delete proc_;
        proc_ = nullptr;
        return false;
    }
    winutil::gameJob().assign(proc_->processId());
    applog::write(QStringLiteral("game %1/%2: started %3 (pid %4), session %5")
                      .arg(gameId_).arg(instance_)
                      .arg((QStringList{QDir::toNativeSeparators(fi.absoluteFilePath())} + args).join(QLatin1Char(' ')))
                      .arg(proc_->processId()).arg(QDir::toNativeSeparators(sessionDir_)));
    status_ = QStringLiteral("starting");
    emit statusChanged(status_);
    if (netMode) poll_.start();
    return true;
}

void GameProcess::poll() {
    bool exists = false;
    const QByteArray raw = winutil::readShared(statusPath_, &exists);
    if (exists && !raw.isEmpty()) {
        QString first = QString::fromLatin1(raw).section(QLatin1Char('\n'), 0, 0).trimmed();
        if (!first.isEmpty() && first != status_) {
            status_ = first;
            applog::write(QStringLiteral("game %1/%2: status %3").arg(gameId_).arg(instance_).arg(first));
            emit statusChanged(first);
        }
    }
    if (!pendingCmd_.isEmpty() && (!QFileInfo::exists(cmdPath_) || lastWrite_.elapsed() > 2000)) {
        const QString line = pendingCmd_;
        pendingCmd_.clear();
        writeCommand(line);
    }
}

void GameProcess::command(const QString& line) {
    if (cmdPath_.isEmpty()) return;
    if (QFileInfo::exists(cmdPath_) && lastWrite_.isValid() && lastWrite_.elapsed() < 2000) {
        pendingCmd_ = line;  // previous command not taken yet
        return;
    }
    writeCommand(line);
}

void GameProcess::writeCommand(const QString& line) {
    QString err;
    if (!winutil::writeFileAtomic(cmdPath_, line.toLatin1() + '\n', &err)) {
        applog::write(QStringLiteral("game %1/%2: %3").arg(gameId_).arg(instance_).arg(err));
        return;
    }
    lastWrite_.start();
    // the log leaves the address out (the opponent's or the server's): logs get shared
    static const QRegularExpression remote(QStringLiteral("\\bremote=\\S+"));
    applog::write(QStringLiteral("game %1/%2: cmd %3").arg(gameId_).arg(instance_)
                      .arg(QString(line).replace(remote, QStringLiteral("remote=(hidden)"))));
}

void GameProcess::quit() {
    if (!isRunning() || quitting_) return;
    quitting_ = true;
    pendingCmd_.clear();
    if (netMode_) writeCommand(QStringLiteral("quit"));
    else proc_->terminate();  // WM_CLOSE to its windows: the game closes as when the player closes it
    QTimer::singleShot(5000, this, [this] {
        if (isRunning()) kill();
    });
}

void GameProcess::kill() {
    if (isRunning()) {
        quitting_ = true;
        proc_->kill();
    }
}
