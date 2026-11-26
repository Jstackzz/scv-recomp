#include "ToolPages.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStorageInfo>
#include <QThread>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "GameData.h"
#include "GameProcess.h"
#include "KeyboardBox.h"
#include "ServerClient.h"
#include "Upnp.h"
#include "WinUtil.h"

#include <windows.h>

namespace {
QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QString humanBytes(qint64 b) {
    if (b >= (qint64(1) << 30)) return QStringLiteral("%1 GB").arg(double(b) / double(qint64(1) << 30), 0, 'f', 2);
    if (b >= (1 << 20)) return QStringLiteral("%1 MB").arg(double(b) / double(1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(b / 1024);
}

QWidget* pathRow(QLineEdit* edit, QPushButton* browse) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    h->addWidget(browse);
    return w;
}
}  // namespace

// --- BuildPage -------------------------------------------------------------------------------------

namespace {

QString sha1Of(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

bool sameHash(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

QString hex8(quint32 v) { return QStringLiteral("%1").arg(v, 8, 16, QLatin1Char('0')).toUpper(); }

// the disc's dashboard update: never needed (the console's own system software), so never extracted
bool systemUpdate(const QString& path) {
    return path.section(u'/', 0, 0).compare(QLatin1String("$SystemUpdate"), Qt::CaseInsensitive) == 0;
}

QByteArray readFile(const QString& path, qint64 limit = qint64(64) << 20) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit) return {};
    return f.readAll();
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path + QStringLiteral(".part"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        *error = QCoreApplication::translate("BuildPage", "Cannot write %1: %2").arg(native(path), f.errorString());
        f.close();
        f.remove();
        return false;
    }
    f.close();
    QFile::remove(path);
    if (!QFile::rename(f.fileName(), path)) {
        *error = QCoreApplication::translate("BuildPage", "Cannot put %1 in place.").arg(native(path));
        return false;
    }
    return true;
}

// a DLC package's folder name under dlc\: its file name, made safe
QString dlcName(const QString& package) {
    QString name = QFileInfo(package).fileName();
    for (QChar& c : name)
        if (!(c.isLetterOrNumber() || c == u'.' || c == u'-' || c == u'_')) c = u'_';
    return name.isEmpty() ? QStringLiteral("package") : name;
}

}  // namespace

BuildPage::BuildPage(QWidget* parent) : Page(parent) {
    // in a scroll area: with the title update notice over it, a small window scrolls rather than crushing the steps
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Build game"),
                                   tr("The game is built on your PC from your own Xbox 360 disc and title update 3: "
                                      "the launcher checks both, extracts the disc, puts the update beside it (and "
                                      "your own downloadable content, if you add it), then the game's code is "
                                      "compiled for this PC. Nothing from the game is downloaded.")));
    updateAlert_ = ui::label(QString(), "Alert");
    updateAlert_->setWordWrap(true);
    updateAlert_->setTextFormat(Qt::PlainText);
    updateAlert_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    updateAlert_->hide();
    root->addWidget(updateAlert_);
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    QVBoxLayout* info = nullptr;
    columns->addWidget(ui::card(tr("Game"), &info), 1);
    gameTitle_ = ui::label(QString(), "Big");
    folderInfo_ = ui::label(QString(), "Muted");
    discInfo_ = ui::label(QString(), "Muted");
    updateInfo_ = ui::label(QString(), "Muted");
    dlcInfo_ = ui::label(QString(), "Muted");
    exeInfo_ = ui::label(QString(), "Muted");
    info->addWidget(gameTitle_);
    for (QLabel* l : {folderInfo_, discInfo_, updateInfo_, dlcInfo_, exeInfo_}) {
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info->addWidget(l);
    }
    info->addStretch();

    QVBoxLayout* steps = nullptr;
    columns->addWidget(ui::card(tr("Steps"), &steps), 1);
    steps->addWidget(ui::label(tr("1. Your disc image (.iso)"), "Muted"));
    image_ = new QLineEdit;
    image_->setPlaceholderText(tr("path to your disc image (empty: the disc already extracted)"));
    browse_ = new QPushButton(tr("Browse..."));
    steps->addWidget(pathRow(image_, browse_));
    if (const Game* g = GameData::instance().game(gameId_)) {
        QString file = g->update.package;
        if (!g->update.packageNote.isEmpty()) file += QStringLiteral(" (%1)").arg(g->update.packageNote);
        // in full contrast, not muted as the other steps: which file to pick here changed (title update 1 until 0.1.5)
        auto* step2 = ui::label(tr("2. Title update %1: the file %2").arg(g->update.number).arg(file));
        step2->setWordWrap(true);
        step2->setTextInteractionFlags(Qt::TextSelectableByMouse);
        steps->addWidget(step2);
    }
    update_ = new QLineEdit;
    update_->setPlaceholderText(tr("path to the title update package (empty: already in place)"));
    browseUpdate_ = new QPushButton(tr("Browse..."));
    steps->addWidget(pathRow(update_, browseUpdate_));
    steps->addWidget(ui::label(tr("3. Your downloadable content packages (optional)"), "Muted"));
    auto* dlcRow = new QHBoxLayout;
    dlcList_ = ui::label(QString());
    dlcList_->setWordWrap(true);
    addDlc_ = new QPushButton(tr("Add..."));
    clearDlc_ = new QPushButton(tr("Clear"));
    dlcRow->addWidget(dlcList_, 1);
    dlcRow->addWidget(addDlc_);
    dlcRow->addWidget(clearDlc_);
    steps->addLayout(dlcRow);
    build_ = new QPushButton(tr("CHECK AND BUILD"));
    build_->setObjectName(QStringLiteral("Primary"));
    steps->addWidget(build_);
    auto* compileRow = new QHBoxLayout;
    compile_ = new QPushButton(tr("Compile the game's code"));
    compile_->setToolTip(tr("The last step again by hand: compiles the game's code for this PC from the extracted "
                            "disc and update."));
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->setEnabled(false);
    compileRow->addWidget(compile_);
    compileRow->addStretch();
    compileRow->addWidget(cancel_);
    steps->addLayout(compileRow);
    steps->addStretch();

    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    root->addWidget(progress_);
    log_ = new LogView;
    root->addWidget(log_, 1);

    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Disc image"), image_->text(),
                                                       tr("Xbox 360 disc images (*.iso);;All files (*)"));
        if (!f.isEmpty()) {
            image_->setText(native(f));
            cfg::set(QStringLiteral("games/%1/image").arg(gameId_), f);
        }
    });
    connect(browseUpdate_, &QPushButton::clicked, this, [this] { chooseUpdate(false); });
    connect(addDlc_, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Downloadable content packages"),
                                                                dlc_.isEmpty() ? QString() : QFileInfo(dlc_.last()).absolutePath(),
                                                                tr("All files (*)"));
        for (const QString& f : files)
            if (!dlc_.contains(f)) dlc_ << f;
        cfg::set(QStringLiteral("games/%1/dlc").arg(gameId_), dlc_);
        showDlc();
    });
    connect(clearDlc_, &QPushButton::clicked, this, [this] {
        dlc_.clear();
        cfg::set(QStringLiteral("games/%1/dlc").arg(gameId_), dlc_);
        showDlc();
    });
    connect(build_, &QPushButton::clicked, this, &BuildPage::startBuild);
    connect(compile_, &QPushButton::clicked, this, &BuildPage::compileGame);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        stop_ = true;
        if (compiler_) compiler_->kill();
    });
    progressTimer_.setInterval(500);
    connect(&progressTimer_, &QTimer::timeout, this, &BuildPage::updateProgress);
}

BuildPage::~BuildPage() {
    stop_ = true;
    if (worker_) worker_->wait();
    if (compiler_) {
        compiler_->kill();
        compiler_->waitForFinished(3000);
    }
}

void BuildPage::extractImage(const QString& path) {
    image_->setText(native(path));
    startBuild();
}

void BuildPage::setGame(const QString& gameId) {
    if (step_ != Step::Idle) return;  // keep the running build's game
    Page::setGame(gameId);
    image_->setText(native(cfg::get(QStringLiteral("games/%1/image").arg(gameId)).toString()));
    update_->setText(native(cfg::get(QStringLiteral("games/%1/update_package").arg(gameId)).toString()));
    dlc_ = cfg::get(QStringLiteral("games/%1/dlc").arg(gameId)).toStringList();
    dlc_.removeAll(QString());  // "dlc=" in the settings file: none
    showDlc();
    refreshInfo();
}

void BuildPage::showDlc() {
    if (dlc_.isEmpty()) {
        dlcList_->setText(tr("none"));
        dlcList_->setToolTip(QString());
        return;
    }
    QStringList names;
    for (const QString& f : std::as_const(dlc_)) names << QFileInfo(f).fileName();
    dlcList_->setText(tr("%n package(s)", nullptr, int(dlc_.size())));
    dlcList_->setToolTip(names.join(QLatin1Char('\n')));
}

void BuildPage::refreshInfo() {
    const Game* g = GameData::instance().game(gameId_);
    if (!g) return;
    gameTitle_->setText(g->title);
    folderInfo_->setText(tr("%1 (%2), title ID %3\nGame folder: %4")
                             .arg(g->title, g->edition, g->titleId, native(cfg::gameFolder(gameId_))));
    const QDir disc(cfg::gameDataDir(gameId_));
    discInfo_->setText(disc.exists(QStringLiteral("default.xex"))
                           ? tr("Disc: extracted in %1").arg(native(disc.path()))
                           : tr("Disc: not extracted yet (%1)").arg(native(disc.path())));
    const bool xexp = disc.exists(QStringLiteral("default.xexp"));
    const bool patch = QDir(cfg::gameUpdateDir(gameId_)).exists(QStringLiteral("patch.cpk"));
    // the update in place checked by its hashes: after an update of SCV Online that moved to another title update,
    // the old one is still there until the player chooses the new one
    const QString problem = GameProcess::updateProblem(gameId_);
    updateInfo_->setText(!problem.isEmpty() ? tr("Title update: not %1 (choose it in step 2)").arg(g->update.name())
                         : xexp && patch    ? tr("Title update: %1 in place").arg(g->update.name())
                                            : tr("Title update: not in place yet (%1)").arg(g->update.package));
    // short, so the steps stay in view: what is there, and which file to choose (the window CHECK AND BUILD or the
    // compile step opens says the rest)
    QString file = g->update.package;
    if (!g->update.packageNote.isEmpty()) file += QStringLiteral(" (%1)").arg(g->update.packageNote);
    updateAlert_->setText(problem.section(QStringLiteral("\n\n"), 0, 0) + QLatin1Char(' ') +
                          tr("SCV Online now needs %1: in step 2, choose the file %2, then press CHECK AND BUILD.")
                              .arg(g->update.name(), file));
    updateAlert_->setVisible(!problem.isEmpty());
    const QStringList installed = QDir(cfg::gameDlcDir(gameId_)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    dlcInfo_->setText(installed.isEmpty() ? tr("Downloadable content: none")
                                          : tr("Downloadable content: %n package(s)", nullptr, int(installed.size())));
    dlcInfo_->setToolTip(installed.join(QLatin1Char('\n')));
    const QFileInfo exe(cfg::gameExe(gameId_));
    exeInfo_->setText(exe.exists() ? tr("Game: %1").arg(native(exe.filePath()))
                                   : tr("Game: %1 (not built yet)").arg(native(exe.filePath())));
}

void BuildPage::setBusy(bool busy) {
    for (QWidget* w : std::initializer_list<QWidget*>{build_, compile_, browse_, browseUpdate_, image_, update_,
                                                      addDlc_, clearDlc_})
        w->setEnabled(!busy);
    cancel_->setEnabled(busy);
    emit busyChanged(busy);
}

void BuildPage::runWorker(std::function<void()> work, std::function<void()> done) {
    worker_ = QThread::create(std::move(work));
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, [this, done] {
        worker_->deleteLater();
        worker_ = nullptr;
        done();
    });
    worker_->start();
}

void BuildPage::finishStep() {
    step_ = Step::Idle;
    progressTimer_.stop();
    setBusy(false);
}

void BuildPage::startBuild() {
    if (step_ != Step::Idle) return;
    const Game* g = GameData::instance().game(gameId_);
    if (!g) return;
    const QString image = QDir::fromNativeSeparators(image_->text().trimmed());
    const QString update = QDir::fromNativeSeparators(update_->text().trimmed());
    if (!image.isEmpty() && !QFileInfo(image).isFile()) {
        log_->line(tr("There is no disc image at %1.").arg(native(image)));
        return;
    }
    if (!update.isEmpty() && !QFileInfo(update).isFile()) {
        log_->line(tr("There is no title update package at %1.").arg(native(update)));
        return;
    }
    cfg::set(QStringLiteral("games/%1/image").arg(gameId_), image);
    cfg::set(QStringLiteral("games/%1/update_package").arg(gameId_), update);
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    log_->line(tr("Checking your files ..."));
    applog::write(QStringLiteral("build %1: checking disc %2, update %3, %4 DLC package(s)")
                      .arg(gameId_, image.isEmpty() ? QStringLiteral("(extracted)") : native(image),
                           update.isEmpty() ? QStringLiteral("(in place)") : native(update))
                      .arg(dlc_.size()));
    step_ = Step::Checking;
    stop_ = false;
    setBusy(true);
    auto disc = std::make_shared<x360::Disc>();
    auto c = std::make_shared<Check>();
    const Game game = *g;
    const QString dataDir = cfg::gameDataDir(gameId_), updateDir = cfg::gameUpdateDir(gameId_);
    runWorker(
        [disc, c, game, image, update, dataDir, updateDir, dlc = dlc_] {
            const auto tr = [](const char* text) { return QCoreApplication::translate("BuildPage", text); };
            // --- the disc: its default.xex must be the one the build is made from
            QByteArray xex;
            if (!image.isEmpty()) {
                if (!disc->open(image, &c->error)) return;
                const x360::Entry* e = disc->find(QStringLiteral("default.xex"));
                if (!e) {
                    c->error = tr("The disc image has no default.xex: it is not an Xbox 360 game disc.");
                    return;
                }
                if (!disc->read(*e, &xex, &c->error)) return;
                c->extractDisc = true;
                c->discBytes = disc->totalBytes(systemUpdate);
                c->discFiles = disc->fileCount(systemUpdate);
                for (const x360::Entry& f : disc->entries()) {
                    if (f.dir || systemUpdate(f.path)) continue;
                    if (QFileInfo(QDir(dataDir).filePath(f.path)).size() != qint64(f.size)) c->discMissingBytes += f.size;
                }
                c->lines << tr("Disc: %1, %2 files, %3 (the dashboard update, $SystemUpdate, left out).")
                                .arg(disc->layout())
                                .arg(c->discFiles)
                                .arg(humanBytes(c->discBytes));
            } else {
                xex = readFile(QDir(dataDir).filePath(QStringLiteral("default.xex")));
                if (xex.isEmpty()) {
                    c->error = tr("Choose your disc image (step 1): the disc is not extracted yet.");
                    return;
                }
                c->lines << tr("Disc: already extracted in %1.").arg(native(dataDir));
            }
            const QString xexSha1 = sha1Of(xex);
            if (const QString other = game.otherRelease(xex); !other.isEmpty()) {
                c->error = other;
                return;
            }
            c->lines << tr("default.xex: %1 %2, title ID %3, SHA-1 %4: the right one.")
                            .arg(game.title, game.xexVersion, game.titleId, xexSha1);
            // --- the title update: a LIVE package of this title whose default.xexp patches that version into the
            // one the build is made from (title update 3; an older one is named and refused)
            const TitleUpdate& tu = game.update;
            if (!update.isEmpty()) {
                const QString fileName = QFileInfo(update).fileName();
                const QString chosen = tr("The file you chose, %1, is").arg(fileName);
                c->wrongUpdate = true;  // until it is the right one: every refusal below asks for the right file
                x360::Package pkg;
                QString why;
                if (!pkg.open(update, &why)) {
                    c->error = tr("The file you chose, %1, is not a title update: %2").arg(fileName, why) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    return;
                }
                if (pkg.magic() != QLatin1String("LIVE") || pkg.contentType() != x360::kContentTitleUpdate) {
                    c->error = tr("%1 not a title update: it is a %2 package of %3.")
                                   .arg(chosen, pkg.magic(), x360::contentTypeName(pkg.contentType())) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    return;
                }
                if (pkg.titleId() != game.titleIdValue()) {
                    c->error = tr("%1 a title update of another game (%2, title ID %3), not of %4.")
                                   .arg(chosen, pkg.titleName(), hex8(pkg.titleId()), game.title) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    return;
                }
                const x360::PackageFile* xexpFile = pkg.find(QStringLiteral("default.xexp"));
                if (!xexpFile || !pkg.read(*xexpFile, &c->xexp, &why)) {
                    c->error = tr("%1 a title update without the game's patch in it (no default.xexp%2).")
                                   .arg(chosen, why.isEmpty() ? QString() : QStringLiteral(": ") + why) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    return;
                }
                x360::XexInfo info;
                if (!x360::readXex(c->xexp, &info, &why) || !info.hasDelta) {
                    c->error = tr("%1 a title update whose default.xexp is not a patch of the game (%2).")
                                   .arg(chosen, why.isEmpty() ? tr("no delta patch in it") : why) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    return;
                }
                const QString from = x360::versionText(info.deltaSource), to = x360::versionText(info.deltaTarget);
                c->lines << tr("Title update: \"%1\" (%2), %3 package, title ID %4; default.xexp patches %5 into %6, "
                               "SHA-1 %7.")
                                .arg(pkg.displayName(), fileName, pkg.magic(), hex8(pkg.titleId()), from, to,
                                     sha1Of(c->xexp));
                if (!sameHash(sha1Of(c->xexp), tu.xexpSha1) || from != tu.fromVersion || to != tu.toVersion) {
                    c->error = game.otherUpdate(chosen, sha1Of(c->xexp), to);
                    return;
                }
                for (auto it = tu.files.cbegin(); it != tu.files.cend(); ++it) {
                    if (it.key() == QLatin1String("default.xexp")) continue;
                    const x360::PackageFile* f = pkg.find(it.key());
                    QByteArray data;
                    if (!f || !pkg.read(*f, &data, &why) ||
                        (!it.value().isEmpty() && !sameHash(sha1Of(data), it.value()))) {
                        c->error = tr("%1 %2, but its file %3 is missing or changed: a damaged copy. Copy it from "
                                      "your console again.").arg(chosen, tu.name(), it.key());
                        return;
                    }
                    if (it.key() == QLatin1String("patch.cpk")) c->patchCpk = data;
                }
                c->wrongUpdate = false;
                c->lines << tr("Title update: %1, the right one.").arg(tu.name());
            } else {
                const QByteArray xexp = readFile(QDir(dataDir).filePath(QStringLiteral("default.xexp")));
                const QByteArray patch = readFile(QDir(updateDir).filePath(QStringLiteral("patch.cpk")));
                if (xexp.isEmpty() || patch.isEmpty()) {
                    c->error = tr("Choose %1 in step 2: it is not in place yet.").arg(tu.name()) +
                               QStringLiteral("\n\n") + game.updateNeeded();
                    c->wrongUpdate = true;
                    return;
                }
                if (!sameHash(sha1Of(xexp), tu.xexpSha1)) {
                    x360::XexInfo info;
                    QString why;
                    const QString to = x360::readXex(xexp, &info, &why) && info.hasDelta
                                           ? x360::versionText(info.deltaTarget) : QString();
                    c->error = game.otherUpdate(tr("The title update on this PC is"), sha1Of(xexp), to);
                    c->wrongUpdate = true;
                    return;
                }
                if (const QString want = tu.files.value(QStringLiteral("patch.cpk"));
                    !want.isEmpty() && !sameHash(sha1Of(patch), want)) {
                    c->error = tr("The title update on this PC is not complete: its file patch.cpk is not %1's.")
                                   .arg(tu.name()) + QStringLiteral("\n\n") + game.updateNeeded();
                    c->wrongUpdate = true;
                    return;
                }
                c->lines << tr("Title update: %1 already in place.").arg(tu.name());
            }
            // --- the player's own DLC: packages of this title's downloadable content, nothing else
            for (const QString& path : dlc) {
                x360::Package pkg;
                QString why;
                if (!pkg.open(path, &why)) {
                    c->error = why;
                    return;
                }
                if (pkg.contentType() != x360::kContentMarketplace || pkg.titleId() != game.titleIdValue()) {
                    c->error = tr("%1 is not %2 downloadable content: it is a %3 package of title ID %4.")
                                   .arg(QFileInfo(path).fileName(), game.title, x360::contentTypeName(pkg.contentType()),
                                        hex8(pkg.titleId()));
                    return;
                }
                c->dlc << path;
                c->dlcBytes += pkg.totalBytes();
                c->lines << QCoreApplication::translate("BuildPage", "Downloadable content: \"%1\" (%2, %n file(s)).",
                                                        nullptr, int(pkg.files().size()))
                                .arg(pkg.displayName(), QFileInfo(path).fileName());
            }
            c->ok = true;
        },
        [this, disc, c] { onChecked(disc, c); });
}

void BuildPage::onChecked(const std::shared_ptr<x360::Disc>& disc, const std::shared_ptr<Check>& c) {
    for (const QString& line : std::as_const(c->lines)) log_->line(line);
    if (stop_) {
        log_->line(tr("Cancelled."));
        finishStep();
        return;
    }
    if (!c->ok) {
        log_->line(c->error);
        applog::write(QStringLiteral("build %1: refused: %2").arg(gameId_, QString(c->error).replace(u'\n', u' ')));
        finishStep();
        refreshInfo();
        if (c->wrongUpdate) askForUpdate(c->error);
        else QMessageBox::warning(this, tr("Cannot build the game"), c->error);
        return;
    }
    // the space it takes: what is not extracted yet, the DLC, and room for the game's code
    const QString folder = cfg::gameFolder(gameId_);
    QDir().mkpath(folder);
    const qint64 needed = c->discMissingBytes + c->dlcBytes + (qint64(2) << 30);
    const qint64 freeBytes = QStorageInfo(folder).bytesAvailable();
    if (freeBytes >= 0 && freeBytes < needed) {
        const QString text = tr("Not enough free space on that drive: %1 needed, %2 free.")
                                 .arg(humanBytes(needed), humanBytes(freeBytes));
        log_->line(text);
        finishStep();
        QMessageBox::warning(this, tr("Cannot build the game"), text);
        return;
    }
    step_ = Step::Extracting;
    bytesDone_ = 0;
    filesDone_ = 0;
    totalBytes_ = c->extractDisc ? c->discBytes : 0;
    totalFiles_ = c->extractDisc ? c->discFiles : 0;
    if (c->extractDisc) log_->line(tr("Extracting the disc into %1 ...").arg(native(cfg::gameDataDir(gameId_))));
    progressTimer_.start();
    auto error = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    const QString dataDir = cfg::gameDataDir(gameId_), updateDir = cfg::gameUpdateDir(gameId_);
    const QString dlcDir = cfg::gameDlcDir(gameId_);
    runWorker(
        [this, disc, c, error, ok, dataDir, updateDir, dlcDir] {
            if (c->extractDisc && !disc->extractAll(dataDir, systemUpdate, stop_, bytesDone_, filesDone_, error.get()))
                return;
            // the update: default.xexp beside default.xex (the runtime applies it as it loads the game), the rest in
            // the update folder
            if (!c->xexp.isEmpty() && !writeFile(QDir(dataDir).filePath(QStringLiteral("default.xexp")), c->xexp, error.get()))
                return;
            if (!c->patchCpk.isEmpty() &&
                !writeFile(QDir(updateDir).filePath(QStringLiteral("patch.cpk")), c->patchCpk, error.get()))
                return;
            // each DLC package: its files in dlc\<package>\, its header (what the console lists it by) beside them
            for (const QString& path : std::as_const(c->dlc)) {
                if (stop_) {
                    *error = QCoreApplication::translate("BuildPage", "Cancelled.");
                    return;
                }
                x360::Package pkg;
                const QString name = dlcName(path);
                const QString into = QDir(dlcDir).filePath(name);
                QDir(into).removeRecursively();
                if (!pkg.open(path, error.get()) || !pkg.extractAll(into, error.get()) ||
                    !writeFile(QDir(dlcDir).filePath(name + QStringLiteral(".header")), pkg.header(), error.get()))
                    return;
            }
            *ok = true;
        },
        [this, ok, error] { onExtracted(*ok, *error); });
}

void BuildPage::onExtracted(bool ok, const QString& error) {
    finishStep();
    refreshInfo();
    if (!ok) {
        log_->line(tr("Stopped: %1").arg(error));
        applog::write(QStringLiteral("build %1: stopped: %2").arg(gameId_, error));
        return;
    }
    progress_->setValue(1000);
    progress_->setFormat(totalFiles_ ? tr("Extracted  -  %1 files, %2").arg(totalFiles_).arg(humanBytes(totalBytes_))
                                     : tr("In place"));
    log_->line(tr("The disc, its title update and your downloadable content are in place in %1.")
                   .arg(native(cfg::gameFolder(gameId_))));
    applog::write(QStringLiteral("build %1: files in place (%2 disc files extracted)").arg(gameId_).arg(totalFiles_));
    compileGame();
}

void BuildPage::updateProgress() {
    if (step_ != Step::Extracting || totalBytes_ <= 0) return;
    const qint64 done = bytesDone_;
    progress_->setValue(static_cast<int>(qMin<qint64>(999, done * 1000 / totalBytes_)));
    progress_->setFormat(tr("%p%  -  %1 of %2 files, %3 of %4").arg(filesDone_.load()).arg(totalFiles_)
                             .arg(humanBytes(done), humanBytes(totalBytes_)));
}

void BuildPage::rebuild() { compileGame(); }

void BuildPage::chooseUpdate(bool buildAfter) {
    const QString f = QFileDialog::getOpenFileName(this, tr("Title update package"), update_->text(), tr("All files (*)"));
    if (f.isEmpty()) return;
    update_->setText(native(f));
    cfg::set(QStringLiteral("games/%1/update_package").arg(gameId_), f);
    if (buildAfter) startBuild();
}

void BuildPage::askForUpdate(const QString& text) {
    const Game* g = GameData::instance().game(gameId_);
    const QString name = g ? g->update.name() : tr("the title update");
    // large, plain and high contrast: the one thing a player must do before playing again
    QMessageBox box(QMessageBox::Warning, tr("SCV Online needs %1").arg(name), text, QMessageBox::NoButton, this);
    box.setTextFormat(Qt::PlainText);
    box.setStyleSheet(QStringLiteral("QLabel { font-size: 14pt; color: #FFFFFF; }"
                                     "QPushButton { font-family: \"Bahnschrift\"; font-size: 13pt; font-weight: 700; "
                                     "padding: 10px 22px; }"));
    QPushButton* choose = box.addButton(tr("Choose %1 now").arg(name), QMessageBox::AcceptRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(choose);
    box.exec();
    if (box.clickedButton() == choose) chooseUpdate(true);
}

void BuildPage::compileGame() {
    if (compiler_ || step_ != Step::Idle) return;
    // never a build from another title update than the one this release takes (after an update of SCV Online that
    // moved to another one, the old one is still in place until the player chooses the new one)
    if (const QString problem = GameProcess::updateProblem(gameId_); !problem.isEmpty()) {
        log_->line(problem);
        log_->line(GameProcess::updateFix(gameId_));
        applog::write(QStringLiteral("build %1: not compiled: %2").arg(gameId_, QString(problem).replace(u'\n', u' ')));
        refreshInfo();
        askForUpdate(problem);
        return;
    }
    const QString script = cfg::buildScript(gameId_);
    if (!QFileInfo::exists(script)) {
        // the game's side of the build (recompiling the patched executable for this PC) is not part of the launcher
        // yet: the page goes as far as the files the game reads
        log_->line(tr("The step that compiles the game's code for this PC is not ready yet (no %1): the disc and the "
                      "update are in place for it.").arg(native(script)));
        applog::write(QStringLiteral("build %1: no build script %2").arg(gameId_, native(script)));
        refreshInfo();
        return;
    }
    if (GameProcess::xexFile(gameId_).isEmpty())
        log_->line(tr("Warning: the disc is not extracted yet (no default.xex in %1).").arg(native(cfg::gameDataDir(gameId_))));
    compiler_ = new QProcess(this);
    compilerOut_.clear();
    compiler_->setWorkingDirectory(QFileInfo(script).absolutePath());
    compiler_->setProcessChannelMode(QProcess::MergedChannels);
    compiler_->setProgram(QStringLiteral("cmd.exe"));
    // cmd /s /c ""<script>" <args>": the outer quotes go, the script's stay (a path with spaces)
    compiler_->setNativeArguments(QStringLiteral("/d /s /c \"\"%1\" %2\"").arg(native(script), cfg::buildArgs(gameId_)));
    compiler_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SC5_GAME_DIR"), native(cfg::gameDataDir(gameId_)));
    env.insert(QStringLiteral("SC5_UPDATE_DIR"), native(cfg::gameUpdateDir(gameId_)));
    env.insert(QStringLiteral("SC5_GAME_EXE"), native(cfg::gameExe(gameId_)));
    compiler_->setProcessEnvironment(env);
    connect(compiler_, &QProcess::readyRead, this, [this] {
        compilerOut_.append(compiler_->readAll());
        qsizetype nl;
        while ((nl = compilerOut_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(compilerOut_.left(nl)).trimmed();
            compilerOut_.remove(0, nl + 1);
            if (!line.isEmpty()) log_->line(line);
        }
    });
    connect(compiler_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        const bool ok = st == QProcess::NormalExit && code == 0 && QFileInfo::exists(cfg::gameExe(gameId_));
        log_->line(stop_ ? tr("Cancelled.")
                   : ok  ? tr("The game is built: Single player, or search for a match.")
                         : tr("Compiling stopped (exit code %1): the lines above say why.").arg(code));
        applog::write(QStringLiteral("build %1: build script exited with code %2").arg(gameId_).arg(code));
        compiler_->deleteLater();
        compiler_ = nullptr;
        progress_->setRange(0, 1000);
        progress_->setValue(ok ? 1000 : 0);
        finishStep();
        refreshInfo();
    });
    stop_ = false;
    step_ = Step::Compiling;
    setBusy(true);
    progress_->setRange(0, 0);  // busy, no measure
    log_->line(tr("Compiling the game's code: %1").arg(native(script)));
    applog::write(QStringLiteral("build %1: running %2").arg(gameId_, native(script)));
    compiler_->start();
}

// --- SettingsPage ----------------------------------------------------------------------------------

SettingsPage::SettingsPage(ServerClient* server, QWidget* parent) : Page(parent), server_(server) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(10);
    root->addWidget(ui::pageHeader(tr("Settings"), tr("Saved to %1").arg(native(paths::configFile()))));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(4);
    root->addLayout(grid);

    // server
    auto* serverBox = new QGroupBox(tr("SERVER"));
    auto* sf = new QFormLayout(serverBox);
    address_ = new QLineEdit;
    address_->setPlaceholderText(QStringLiteral("host:7777"));
    fingerprint_ = new QLineEdit;
    fingerprint_->setPlaceholderText(tr("SHA-256 of the server certificate (the server's host gives it out)"));
    autoConnect_ = new QCheckBox(tr("Connect when the launcher starts"));
    autoConnect_->setChecked(cfg::get(QStringLiteral("server/auto_connect"), true).toBool());
    auto* connectBtn = new QPushButton(tr("Connect"));
    auto* disconnectBtn = new QPushButton(tr("Disconnect"));
    auto* pinBtn = new QPushButton(tr("Pin presented certificate"));
    auto* btnRow = new QWidget;
    auto* br = new QHBoxLayout(btnRow);
    br->setContentsMargins(0, 0, 0, 0);
    br->addWidget(connectBtn);
    br->addWidget(disconnectBtn);
    br->addWidget(pinBtn);
    br->addStretch();
    serverState_ = ui::label(QString(), "Muted");
    presented_ = ui::label(QString(), "Muted");
    presented_->setWordWrap(true);
    presented_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sf->addRow(tr("Address"), address_);
    sf->addRow(tr("Fingerprint"), fingerprint_);
    sf->addRow(QString(), autoConnect_);
    sf->addRow(QString(), btnRow);
    sf->addRow(tr("State"), serverState_);
    sf->addRow(tr("Presented"), presented_);
    grid->addWidget(serverBox, 0, 0, 1, 2);

    // online
    auto* onlineBox = new QGroupBox(tr("ONLINE"));
    auto* of = new QFormLayout(onlineBox);
    auto* region = new QComboBox;
    ui::fillRegions(region, cfg::region());
    auto* maxPing = new QComboBox;
    ui::fillMaxPing(maxPing, cfg::maxPing());
    // Auto (the default): the match's delay from its measured ping, the same in both games; or the player's own
    // number of frames, which the match plays when it is the larger of the two players'
    auto* delay = new QComboBox;
    delay->addItem(cfg::inputDelayText(cfg::kAutoDelay), cfg::kAutoDelay);
    for (int d = 0; d <= 4; ++d) delay->addItem(cfg::inputDelayText(d), d);
    delay->setCurrentIndex(std::max(0, delay->findData(cfg::inputDelay())));
    delay->setToolTip(tr("Auto picks the delay from the ping measured when the match starts: 1 frame under 67 ms, a "
                         "frame more for every 33 ms above (2 to 100 ms, 3 to 133 ms, 4 to 166 ms, 5 to 200 ms, 6 "
                         "above), so the game rolls back less and characters jump less. A number keeps that many "
                         "frames, more when the other player's is larger. Both games always play the same delay."));
    auto* delayAuto = ui::label(tr("Auto: the delay comes from the ping between you and your opponent, the same in "
                                   "both games."), "Muted");
    delayAuto->setWordWrap(true);
    auto* port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(cfg::gamePort());
    auto* strict = new QCheckBox(tr("Strict ranked matchmaking (your region only, a close rank only)"));
    strict->setChecked(cfg::strict());
    auto* upnp = new QCheckBox(tr("Open the game port on the router (UPnP)"));
    upnp->setChecked(cfg::upnp());
    auto* hideIp = new QCheckBox(tr("Hide my IP address from opponents"));
    hideIp->setChecked(cfg::hideIp());
    hideIp->setToolTip(tr("Your matches go through the server, so nobody you play can see your address. They get "
                          "more ping the farther you and your opponent are from the server (in Virginia, USA): two "
                          "players in Europe get about 180 ms this way instead of about 25 ms directly. A match whose "
                          "ping that way is too high tries a direct connection instead, unless the box below is "
                          "ticked."));
    auto* hideAlways = new QCheckBox(tr("Even when the ping through the server is high"));
    hideAlways->setChecked(cfg::hideIpAlways());
    hideAlways->setEnabled(hideIp->isChecked());
    hideAlways->setToolTip(tr("Every match goes through the server however much ping that costs: no opponent ever "
                              "sees your address."));
    // what hiding costs from here: a match goes to the server and back, at least twice the ping to it; past the
    // server's limit (welcome: direct_above_ms) it tries a direct connection unless hidden always
    hideIpCost_ = ui::label(QString(), "Muted");
    hideIpCost_->setWordWrap(true);
    auto updateHideIpCost = [this, hideIp, hideAlways] {
        const int rtt = server_->rttMs();
        const int directAbove = server_->welcome().value(QStringLiteral("direct_above_ms")).toInt(0);
        hideIpCost_->setVisible(hideIp->isChecked() && rtt > 0);
        if (hideAlways->isChecked() || directAbove <= 0)
            hideIpCost_->setText(tr("With this on, every match goes through the server in Virginia: at least %1 ms of "
                                    "ping from here (your ping to it is %2 ms), even against a player next door.")
                                     .arg(2 * rtt).arg(rtt));
        else
            hideIpCost_->setText(tr("With this on, a match goes through the server in Virginia: at least %1 ms of "
                                    "ping from here (your ping to it is %2 ms). When that is over %3 ms, the match "
                                    "tries your opponent directly and plays that way if it is faster: they can then "
                                    "see your address.")
                                     .arg(2 * rtt).arg(rtt).arg(directAbove));
    };
    updateHideIpCost();
    connect(hideIp, &QCheckBox::toggled, this, updateHideIpCost);
    connect(hideIp, &QCheckBox::toggled, hideAlways, &QWidget::setEnabled);
    connect(hideAlways, &QCheckBox::toggled, this, updateHideIpCost);
    connect(server_, &ServerClient::loggedIn, this, updateHideIpCost);
    auto* matchSound = new QCheckBox(tr("Play a sound when a match is found"));
    matchSound->setChecked(cfg::matchSound());
    matchSound->setToolTip(tr("A short chime when the game shows MATCH FOUND, heard over the game."));
    routerState_ = ui::label(tr("Router: not asked"), "Muted");
    routerState_->setWordWrap(true);
    of->addRow(tr("Region"), region);
    of->addRow(tr("Max ping"), maxPing);
    of->addRow(tr("Input delay"), delay);
    of->addRow(QString(), delayAuto);
    of->addRow(tr("Game UDP port"), port);
    of->addRow(QString(), strict);
    of->addRow(QString(), matchSound);
    of->addRow(QString(), upnp);
    of->addRow(QString(), hideIp);
    of->addRow(QString(), hideAlways);
    of->addRow(QString(), hideIpCost_);
    of->addRow(tr("Router"), routerState_);
    auto* delayNote = ui::label(tr("Ranked uses at least 1 frame. Players who cannot reach each other directly, or "
                                   "who hide their IP address, play through the server: more ping the farther both "
                                   "are from it."), "Muted");
    delayNote->setWordWrap(true);
    of->addRow(QString(), delayNote);
    grid->addWidget(onlineBox, 1, 0);

    // display
    auto* displayBox = new QGroupBox(tr("DISPLAY"));
    auto* df = new QFormLayout(displayBox);
    auto* mode = new QComboBox;
    mode->addItem(tr("Windowed"), QStringLiteral("windowed"));
    mode->addItem(tr("Borderless"), QStringLiteral("borderless"));
    mode->addItem(tr("Fullscreen"), QStringLiteral("fullscreen"));
    mode->setCurrentIndex(qMax(0, mode->findData(cfg::displayMode())));
    // the main monitor's own resolution first, the default ("desktop": it follows the monitor), then every other size
    // its modes offer, largest first; a size typed in with an older launcher stays on the list
    auto* res = new QComboBox;
    const QSize desk = winutil::desktopSize();
    const QString deskText = QStringLiteral("%1x%2").arg(desk.width()).arg(desk.height());
    res->addItem(desk.isValid() ? tr("%1 (your monitor)").arg(deskText) : tr("Your monitor's"), QStringLiteral("desktop"));
    for (const QSize& size : winutil::displaySizes()) {
        const QString text = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
        if (size != desk) res->addItem(text, text);
    }
    const QString savedRes = cfg::resolution();
    int resAt = savedRes == deskText ? 0 : res->findData(savedRes);
    if (resAt < 0) {
        res->addItem(savedRes, savedRes);
        resAt = res->count() - 1;
    }
    res->setCurrentIndex(resAt);
    res->setMaxVisibleItems(16);
    auto* vsync = new QComboBox;
    vsync->addItem(tr("On"), QStringLiteral("on"));
    vsync->addItem(tr("On, G-SYNC or FreeSync monitor"), QStringLiteral("adaptive"));
    vsync->addItem(tr("Off"), QStringLiteral("off"));
    vsync->setCurrentIndex(!cfg::vsync() ? 2 : cfg::adaptiveSync() ? 1 : 0);
    vsync->setToolTip(tr("On: every frame waits for your monitor, so the picture never tears, even when a graphics "
                         "driver is set to force v-sync off.\nOn, G-SYNC or FreeSync monitor: the monitor follows the "
                         "game's 60 frames a second instead, as smooth as it gets (turn G-SYNC or FreeSync on in your "
                         "graphics driver).\nOff: each frame is shown at once and may tear."));
    // how the game draws: its GPU's work at a higher resolution, smoothed edges, sharper textures at an angle
    auto* scale = new QComboBox;
    scale->addItem(tr("1280x720 (as on the console)"), 1);
    scale->addItem(tr("2560x1440 (2x)"), 2);
    scale->addItem(tr("3840x2160 (3x)"), 3);
    scale->addItem(tr("5120x2880 (4x)"), 4);
    scale->setCurrentIndex(qMax(0, scale->findData(cfg::renderScale())));
    scale->setToolTip(tr("The resolution the game draws at, before it is fitted to the window or screen. Higher is "
                         "sharper and asks more of your graphics card; 1280x720 is the console's own."));
    auto* aa = new QComboBox;
    aa->addItem(tr("Off"), QStringLiteral("off"));
    aa->addItem(tr("FXAA"), QStringLiteral("fxaa"));
    aa->addItem(tr("FXAA, stronger"), QStringLiteral("fxaa_extreme"));
    aa->setCurrentIndex(qMax(0, aa->findData(cfg::antiAliasing())));
    aa->setToolTip(tr("Smooths the jagged edges of the finished picture. Costs little."));
    auto* aniso = new QComboBox;
    aniso->addItem(tr("As the game sets it"), 0);
    for (int n : {2, 4, 8, 16}) aniso->addItem(tr("Anisotropic %1x").arg(n), n);
    aniso->setCurrentIndex(qMax(0, aniso->findData(cfg::anisotropic())));
    aniso->setToolTip(tr("Keeps textures sharp on surfaces seen at an angle, such as the stage's floor."));
    df->addRow(tr("Window"), mode);
    df->addRow(tr("Resolution"), res);
    df->addRow(tr("Internal resolution"), scale);
    df->addRow(tr("Anti-aliasing"), aa);
    df->addRow(tr("Texture filtering"), aniso);
    df->addRow(tr("V-sync"), vsync);
    auto* brightness = new QSpinBox;
    brightness->setRange(70, 160);
    brightness->setSingleStep(5);
    brightness->setSuffix(QStringLiteral(" %"));
    brightness->setValue(cfg::brightness());
    brightness->setToolTip(tr("Lifts (or lowers) the game's mid-tones; blacks and whites stay as they are. 100 % is "
                              "the picture as the game draws it."));
    df->addRow(tr("Brightness"), brightness);
    auto* frameData = new QCheckBox(tr("Frame data in practice"));
    frameData->setChecked(cfg::frameData());
    frameData->setToolTip(tr("In Training and Online Practice, a frame meter as Street Fighter 6's: both players' "
                             "frames (startup, impact, recovery, stun), each one's startup, impact, recovery, total "
                             "and advantage, and the last hit's damage. F7 in the game hides or shows it."));
    df->addRow(QString(), frameData);
    auto* displayNote = ui::label(tr("These change only how the game looks, never what it computes: you and your "
                                     "opponent may set them differently. A game that is running takes them too, its "
                                     "window the next time you switch to it. Windowed at your monitor's resolution, "
                                     "the window is as large as fits on your screen."), "Muted");
    displayNote->setWordWrap(true);
    df->addRow(QString(), displayNote);
    grid->addWidget(displayBox, 1, 1);

    // the keyboard's keys, for a player without a controller (the Controls page's before 0.1.8); the sound is the
    // game's own Options' business since then (no levels here)
    grid->addWidget(new KeyboardBox, 2, 0, 1, 2);
    auto* soundNote = ui::label(tr("Sound and music: set them in the game's own Options."), "Muted");
    grid->addWidget(soundNote, 3, 0, 1, 2);
    // the game's folder and executable, and what compiles its code
    auto* gamesBox = new QGroupBox(tr("GAME FOLDER"));
    auto* gf = new QFormLayout(gamesBox);
    for (const Game& g : GameData::instance().games()) {
        const QString id = g.id;
        auto* folder = new QLineEdit(native(cfg::gameFolder(id)));
        auto* folderBrowse = new QPushButton(tr("..."));
        auto* exe = new QLineEdit(native(cfg::gameExe(id)));
        auto* exeBrowse = new QPushButton(tr("..."));
        gf->addRow(ui::label(g.title, "CardTitle"));
        auto* script = new QLineEdit(native(cfg::buildScript(id)));
        script->setToolTip(tr("What compiles the game's code once Build game has put the disc and update in place."));
        gf->addRow(tr("Folder"), pathRow(folder, folderBrowse));
        gf->addRow(tr("Executable"), pathRow(exe, exeBrowse));
        gf->addRow(tr("Build script"), script);
        connect(script, &QLineEdit::editingFinished, this, [id, script] {
            cfg::set(QStringLiteral("games/%1/build_script").arg(id), QDir::fromNativeSeparators(script->text().trimmed()));
        });
        connect(folder, &QLineEdit::editingFinished, this, [id, folder] {
            cfg::set(QStringLiteral("games/%1/folder").arg(id), QDir::fromNativeSeparators(folder->text().trimmed()));
        });
        connect(exe, &QLineEdit::editingFinished, this, [id, exe] {
            cfg::set(QStringLiteral("games/%1/exe").arg(id), QDir::fromNativeSeparators(exe->text().trimmed()));
        });
        connect(folderBrowse, &QPushButton::clicked, this, [this, id, folder] {
            const QString d = QFileDialog::getExistingDirectory(this, tr("Game folder"), folder->text());
            if (d.isEmpty()) return;
            folder->setText(native(d));
            cfg::set(QStringLiteral("games/%1/folder").arg(id), d);
        });
        connect(exeBrowse, &QPushButton::clicked, this, [this, id, exe] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Game executable"), exe->text(),
                                                           tr("Programs (*.exe *.py);;All files (*)"));
            if (f.isEmpty()) return;
            exe->setText(native(f));
            cfg::set(QStringLiteral("games/%1/exe").arg(id), f);
        });
    }
    grid->addWidget(gamesBox, 4, 0, 1, 2);

    // tools and development
    auto* toolsBox = new QGroupBox(tr("TOOLS AND DEVELOPMENT"));
    auto* tf = new QFormLayout(toolsBox);
    auto* python = new QLineEdit(cfg::python());
    auto* repo = new QLineEdit(native(cfg::repoRoot()));
    auto* lp1 = new QSpinBox;
    lp1->setRange(1024, 65535);
    lp1->setValue(cfg::localTestPort(1));
    auto* lp2 = new QSpinBox;
    lp2->setRange(1024, 65535);
    lp2->setValue(cfg::localTestPort(2));
    tf->addRow(tr("Python"), python);
    tf->addRow(tr("Repository"), repo);
    tf->addRow(tr("Local test ports"), [&] {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(lp1);
        h->addWidget(lp2);
        h->addStretch();
        return w;
    }());
    grid->addWidget(toolsBox, 5, 0, 1, 2);
    toolsBox->setVisible(!paths::isPackage());  // a player package has no repository or Python to point at
    root->addStretch();

    connect(region, &QComboBox::currentIndexChanged, this, [region] { cfg::set(QStringLiteral("online/region"), region->currentData()); });
    connect(maxPing, &QComboBox::currentIndexChanged, this, [maxPing] { cfg::set(QStringLiteral("online/max_ping"), maxPing->currentData()); });
    connect(delay, &QComboBox::currentIndexChanged, this, [delay, delayAuto] {
        const int d = delay->currentData().toInt();
        cfg::set(QStringLiteral("online/input_delay"), d < 0 ? QStringLiteral("auto") : QString::number(d));
        delayAuto->setText(d < 0 ? tr("Auto: the delay comes from the ping between you and your opponent, the same in "
                                      "both games.")
                                 : tr("%1 of delay, or the other player's when it is larger (Auto counts as the "
                                      "ping's).").arg(cfg::inputDelayText(d)));
    });
    connect(port, &QSpinBox::valueChanged, this, [this](int v) {
        cfg::set(QStringLiteral("online/game_port"), v);
        if (upnp_ && cfg::upnp()) upnp_->map(static_cast<quint16>(v));
    });
    connect(upnp, &QCheckBox::toggled, this, [this](bool on) {
        cfg::set(QStringLiteral("online/upnp"), on);
        if (on && upnp_) upnp_->map(static_cast<quint16>(cfg::gamePort()));
    });
    connect(strict, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/strict"), on); });
    connect(matchSound, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/match_sound"), on); });
    connect(hideIp, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/hide_ip"), on); });
    connect(hideAlways, &QCheckBox::toggled, this,
            [](bool on) { cfg::set(QStringLiteral("online/hide_ip_always"), on); });
    connect(mode, &QComboBox::currentIndexChanged, this, [mode] { cfg::set(QStringLiteral("display/mode"), mode->currentData()); });
    connect(res, &QComboBox::currentIndexChanged, this, [res] {
        cfg::set(QStringLiteral("display/resolution"), res->currentData());
    });
    connect(vsync, &QComboBox::currentIndexChanged, this, [vsync] {
        const QString v = vsync->currentData().toString();
        cfg::set(QStringLiteral("display/adaptive_sync"), v == QLatin1String("adaptive"));
        cfg::set(QStringLiteral("display/vsync"), v != QLatin1String("off"));
    });
    connect(scale, &QComboBox::currentIndexChanged, this, [scale] {
        cfg::set(QStringLiteral("display/render_scale"), scale->currentData());
    });
    connect(aa, &QComboBox::currentIndexChanged, this, [aa] { cfg::set(QStringLiteral("display/anti_aliasing"), aa->currentData()); });
    connect(aniso, &QComboBox::currentIndexChanged, this, [aniso] {
        cfg::set(QStringLiteral("display/anisotropic"), aniso->currentData());
    });
    connect(brightness, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("display/brightness"), v); });
    connect(frameData, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("display/frame_data"), on); });
    connect(python, &QLineEdit::editingFinished, this, [python] { cfg::set(QStringLiteral("tools/python"), python->text().trimmed()); });
    connect(repo, &QLineEdit::editingFinished, this, [repo] {
        cfg::set(QStringLiteral("tools/repo_root"), QDir::fromNativeSeparators(repo->text().trimmed()));
    });
    connect(lp1, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port1"), v); });
    connect(lp2, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port2"), v); });
    connect(autoConnect_, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("server/auto_connect"), on); });
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        cfg::set(QStringLiteral("server/address"), address_->text().trimmed());
        cfg::set(QStringLiteral("server/fingerprint"), ServerClient::normalizeFingerprint(fingerprint_->text()));
        emit reconnectRequested();
    });
    connect(disconnectBtn, &QPushButton::clicked, server_, &ServerClient::disconnectFromServer);
    connect(pinBtn, &QPushButton::clicked, this, [this] {
        if (server_->peerFingerprint().isEmpty()) return;
        fingerprint_->setText(ServerClient::prettyFingerprint(server_->peerFingerprint()));
    });
    connect(server_, &ServerClient::stateChanged, this, [this] { reloadServer(); });
    reloadServer();
    // the mouse wheel scrolls the page: a list or number box under the pointer changes only once it was clicked
    // (before, scrolling down the page changed the region or the resolution it passed over)
    auto* guard = new WheelGuard(scroll->viewport(), this);
    for (QWidget* w : content->findChildren<QWidget*>())
        if (qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(guard);
        }
}

bool WheelGuard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::Wheel) return false;
    const auto* w = qobject_cast<QWidget*>(watched);
    if (!w || w->hasFocus()) return false;
    QCoreApplication::sendEvent(page_, event);  // to the page's scroll area instead
    return true;
}

void SettingsPage::setGame(const QString& gameId) { Page::setGame(gameId); }

void SettingsPage::setUpnp(UpnpMapper* upnp) {
    upnp_ = upnp;
    const auto show = [this](const QString& text) {
        routerState_->setText(text.isEmpty() ? tr("not asked (UPnP is off)") : text);
    };
    connect(upnp_, &UpnpMapper::statusChanged, this, show);
    show(upnp_->status());
}

void SettingsPage::reloadServer() {
    if (!address_->hasFocus()) address_->setText(cfg::serverAddress());
    if (!fingerprint_->hasFocus()) fingerprint_->setText(ServerClient::prettyFingerprint(cfg::fingerprint()));
    static const char* names[] = {"offline", "connecting", "connected", "signed in"};
    serverState_->setText(QString::fromLatin1(names[static_cast<int>(server_->state())]));
    presented_->setText(server_->peerFingerprint().isEmpty() ? tr("(none yet)")
                                                             : ServerClient::prettyFingerprint(server_->peerFingerprint()));
}
