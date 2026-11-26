#include "MainWindow.h"

#include "Upnp.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "Dialogs.h"
#include "GameData.h"
#include "GamepadNav.h"
#include "GameProcess.h"
#include "MatchController.h"
#include "MenuBridge.h"
#include "PlayPage.h"
#include "ReportSender.h"
#include "ServerClient.h"
#include "Style.h"
#include "SupportPage.h"
#include "ToolPages.h"
#include "Updater.h"
#include "Widgets.h"
#include "WinUtil.h"

namespace {
enum PageIndex { kPlay, kBuild, kSettings, kSupport };
const char* kPageNames[] = {"play", "build", "settings", "support"};
// the one game: Soulcalibur V (launcher/data/sc5.json)
const QString kGame = QStringLiteral("sc5");
}  // namespace

MainWindow::MainWindow(const LaunchOptions& opts, QWidget* parent)
    : QMainWindow(parent), opts_(opts), server_(new ServerClient(this)), controller_(new MatchController(server_, this)),
      bridge_(new MenuBridge(server_, controller_, this)) {
    setWindowTitle(tr("SCV Online"));
    setWindowIcon(style::appIcon());
    setMinimumSize(1000, 700);
    resize(1180, 820);

    if (!opts_.server.isEmpty()) cfg::set(QStringLiteral("server/address"), opts_.server);
    if (!opts_.fingerprint.isEmpty())
        cfg::set(QStringLiteral("server/fingerprint"), ServerClient::normalizeFingerprint(opts_.fingerprint));

    auto* root = new QWidget;
    root->setObjectName(QStringLiteral("Root"));
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());
    v->addWidget(buildNav());
    notice_ = new QLabel;
    notice_->setWordWrap(true);
    notice_->hide();
    v->addWidget(notice_);
    // F3 in the game: what it drew goes to the server for the developers
    auto* reports = new ReportSender(server_, [this] { return controller_->runningGames(); }, this);
    connect(reports, &ReportSender::logLine, this, [this](const QString& text) {
        applog::write(QStringLiteral("report: ") + text);
        showNotice(text, false);
        bridge_->showMessage(text);
    });

    stack_ = new QStackedWidget;
    play_ = new PlayPage(server_, controller_, bridge_);
    build_ = new BuildPage;
    settings_ = new SettingsPage(server_);
    pages_ = {play_, build_, settings_, new SupportPage};
    for (Page* p : pages_) {
        p->setGame(kGame);
        stack_->addWidget(p);
    }
    v->addWidget(stack_, 1);
    v->addWidget(buildFooter());
    setCentralWidget(root);
    setWindowTitle(tr("SCV Online - Soulcalibur V"));

    // the game port on the router, as long as the launcher runs (one-PC tests stay on loopback and skip it)
    upnp_ = new UpnpMapper(this);
    const QString standInRouter = cfg::get(QStringLiteral("dev/upnp_location")).toString();
    if (!standInRouter.isEmpty()) upnp_->setLocation(QUrl(standInRouter));
    if (opts_.upnpProbe)
        upnp_->map(static_cast<quint16>(cfg::gamePort()), true);
    else if (cfg::upnp() && !cfg::udpLoopback())
        upnp_->map(static_cast<quint16>(cfg::gamePort()));
    settings_->setUpnp(upnp_);

    connect(settings_, &SettingsPage::reconnectRequested, this, &MainWindow::connectServer);
    connect(play_, &PlayPage::playClicked, this, &MainWindow::play);
    connect(play_, &PlayPage::signInClicked, this, &MainWindow::openAccountDialog);
    connect(play_, &PlayPage::signOutClicked, this, [this] {
        server_->logout();
        cfg::set(QStringLiteral("account/token"), QString());
    });
    connect(play_, &PlayPage::connectClicked, this, &MainWindow::connectServer);
    connect(play_, &PlayPage::updateClicked, this, [this] { showUpdateDialog(false); });
    connect(play_, &PlayPage::buildClicked, this, [this] { selectPage(kBuild); });
    connect(controller_, &MatchController::gameShown, this, &MainWindow::focusGame);
    // what the launcher did by itself that the player must hear of: said in the banner too
    connect(controller_, &MatchController::notice, this, [this](const QString& text) {
        applog::write(QStringLiteral("notice: ") + text);
        showNotice(text, true);
    });
    connect(controller_, &MatchController::playerGameChanged, this, [this](bool running) {
        if (running) playError_.clear();
        checkGameBuilt();
    });
    // a match found: heard over the game (the game shows MATCH FOUND itself), and the game brought to the front
    connect(controller_, &MatchController::matchFound, this, [this](const QJsonObject& found) {
        if (!found.value(QStringLiteral("accept_required")).toBool()) return;
        if (cfg::matchSound())
            applog::write(winutil::playWav(QStringLiteral(":/sounds/match-found.wav"))
                              ? QStringLiteral("sound: match found chime")
                              : QStringLiteral("sound: the match found chime could not play"));
        focusGame();
    });
    // the launcher with a controller: LB / RB the pages, START presses PLAY
    pad_ = new GamepadNav(this);
    pad_->onPage = [this](int step) {
        const int n = int(pages_.size());
        selectPage((stack_->currentIndex() + step + n) % n);
    };
    pad_->onStart = [this] {
        if (stack_->currentIndex() == kPlay) play_->pressPlay();
    };
    pad_->paused = [this] { return controller_->inMatch(); };
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        updateFooter();
        if (st == ServerClient::State::Online) QTimer::singleShot(0, this, &MainWindow::askForAccount);
    });
    // in-app updates (Updater): offered from the server's messages, installed once the game is closed
    updater_ = new Updater(this);
    connect(updater_, &Updater::progress, this, [this](const QString& text, int percent) {
        if (updateDialog_) updateDialog_->setProgress(text, percent);
    });
    connect(updater_, &Updater::failed, this, [this](const QString& text) {
        installPending_ = false;
        if (updateDialog_) updateDialog_->setFailed(text);
        else showNotice(text, true);
    });
    connect(updater_, &Updater::readyToInstall, this, &MainWindow::installUpdateWhenFree);
    connect(updater_, &Updater::installed, this, [this](const QString& version, bool started) {
        if (!started)
            QMessageBox::information(this, tr("SCV Online %1").arg(version),
                                     tr("SCV Online is now %1: start it again.").arg(version));
        close();
    });
    auto whenFree = [this] {
        QTimer::singleShot(0, this, [this] {  // out of the signal
            if (installPending_) {
                installUpdateWhenFree();
            } else if (offerPending_ && gamesBusy().isEmpty() && !updater_->busy()) {
                offerPending_ = false;
                showUpdateDialog(offerRequired_);
            }
        });
    };
    connect(controller_, &MatchController::phaseChanged, this, whenFree);
    connect(controller_, &MatchController::gameStatusChanged, this, whenFree);
    connect(controller_, &MatchController::playerGameChanged, this, whenFree);
    connect(build_, &BuildPage::busyChanged, this, whenFree);
    connect(build_, &BuildPage::busyChanged, this, [this](bool busy) {
        if (!busy) checkGameBuilt();
    });
    Updater::cleanUp();  // what an update set aside, once free; the old launcher may still be closing:
    if (!opts_.updatedFrom.isEmpty())
        for (int s : {2, 5, 10, 20}) QTimer::singleShot(s * 1000, this, [] { Updater::cleanUp(); });
    // An update that changed the game (sc5.exe, or the game's code) builds it again on Build game before anything
    // starts it (as MK9 Online's launcher). A game built on another title update than this release's is not built
    // again from it: Build game asks for the new one (BuildPage::compileGame), and nothing starts the old game
    // meanwhile (GameProcess::start).
    QTimer::singleShot(1500, this, [this] {
        if (!paths::isPackage() || !QFileInfo::exists(cfg::gameExe(kGame))) return;
        const bool rebuild = GameProcess::needsRebuild(kGame);
        const QString problem = GameProcess::updateProblem(kGame);
        if (!rebuild && problem.isEmpty()) return;
        selectPage(kBuild);
        if (!problem.isEmpty()) {
            applog::write(QStringLiteral("build %1: the title update in place is not this release's: asking for it").arg(kGame));
            const Game* g = GameData::instance().game(kGame);
            showNotice(tr("SCV Online now needs %1. Choose it on Build game, step 2: the game is then built again.")
                           .arg(g ? g->update.name() : tr("another title update")),
                       true);
        } else {
            applog::write(QStringLiteral("build %1: built from another release than this one's: building again").arg(kGame));
            showNotice(tr("This update changed the game itself: it is being put together again on Build game (once, a "
                          "few minutes at most)."),
                       false);
        }
        build_->rebuild();  // with the wrong title update in place: the window that asks for the right one
    });
    connect(server_, &ServerClient::message, this, &MainWindow::onServerMessage);
    connect(server_, &ServerClient::connectionError, this, [this](const QString& text) {
        lastError_ = text;
        updateFooter();
        if (text.contains(QLatin1String("fingerprint"))) showNotice(text, true);
    });
    connect(server_, &ServerClient::untrustedCertificate, this, [this](const QString& fp) {
        const auto answer = QMessageBox::question(
            this, tr("Trust this server?"),
            tr("The server at %1 presented a certificate with SHA-256 fingerprint\n\n%2\n\n"
               "Trust it only if this is the fingerprint the server's operator published.")
                .arg(cfg::serverAddress(), ServerClient::prettyFingerprint(fp)));
        if (answer == QMessageBox::Yes) {
            cfg::set(QStringLiteral("server/fingerprint"), fp);
            connectServer();
        }
    });
    connect(server_, &ServerClient::loggedIn, this, [this](const QJsonObject& acc) {
        updateFooter();
        if (cfg::get(QStringLiteral("account/remember"), true).toBool() && opts_.user.isEmpty()) {
            cfg::set(QStringLiteral("account/token"), acc.value(QStringLiteral("token")).toString());
            cfg::set(QStringLiteral("account/name"), acc.value(QStringLiteral("name")).toString());
        }
    });
    connect(server_, &ServerClient::resumeFailed, this, [this] {
        cfg::set(QStringLiteral("account/token"), QString());
        showNotice(tr("Your saved sign-in expired: sign in again on the Play page."), false);
    });
    connect(server_, &ServerClient::kicked, this, [this](const QString& text) {
        showNotice(text + tr(" Press CONNECT AGAIN on the Play page to sign in here again."), true);
    });

    noticeTimer_.setSingleShot(true);
    connect(&noticeTimer_, &QTimer::timeout, notice_, &QWidget::hide);

    if (!opts_.game.isEmpty() && opts_.game != kGame)
        applog::write(QStringLiteral("cli: --game %1 is not a game here; %2 is the only one").arg(opts_.game, kGame));
    // the start page: the one asked for, the last one shown, else Play (the pages of before 0.1.8 are all Play now)
    int startPage = kPlay;
    const QString pageName = opts_.page.isEmpty() ? cfg::get(QStringLiteral("ui/page"), QStringLiteral("play")).toString()
                                                  : opts_.page;
    for (int i = 0; i <= kSupport; ++i)
        if (pageName == QLatin1String(kPageNames[i])) startPage = i;
    if (!opts_.extractImage.isEmpty()) startPage = kBuild;
    selectPage(startPage);
    // development: --snap-dir with --page, a picture of that page once the window is up
    if (!opts_.snapDir.isEmpty() && !opts_.page.isEmpty())
        QTimer::singleShot(2500, this, [this] { snap(QStringLiteral("page-") + QLatin1String(kPageNames[stack_->currentIndex()])); });
    if (opts_.localTest)
        QTimer::singleShot(500, this, [this] {
            QString err;
            if (!controller_->startLocalTest(kGame, QStringLiteral("versus"), QStringLiteral("-"), QStringLiteral("-"), &err))
                applog::write(QStringLiteral("cli: local test failed: %1").arg(err));
        });
    if (!opts_.extractImage.isEmpty())
        QTimer::singleShot(500, this, [this] { build_->extractImage(opts_.extractImage); });
    driver_ = new TestDriver(opts_, server_, controller_, bridge_, this);

    server_->setResumeToken(opts_.user.isEmpty() ? cfg::get(QStringLiteral("account/token")).toString() : QString());
    updateFooter();
    checkGameBuilt();
    if (cfg::get(QStringLiteral("server/auto_connect"), true).toBool() || !opts_.server.isEmpty())
        QTimer::singleShot(0, this, &MainWindow::connectServer);
    // Smart App Control blocks the game this launcher starts: say so before the player builds or plays it
    applog::write(QStringLiteral("Smart App Control: %1").arg(winutil::smartAppControl()));
    if (winutil::smartAppControl() == 1) QTimer::singleShot(400, this, &MainWindow::warnSmartAppControl);
    if (!opts_.updatedFrom.isEmpty()) {
        applog::write(QStringLiteral("update: now %1, from %2").arg(QStringLiteral(SC5_LAUNCHER_VERSION), opts_.updatedFrom));
        showNotice(tr("SCV Online is now %1 (it was %2). The game prepares its code for it next: a minute or two.")
                       .arg(QStringLiteral(SC5_LAUNCHER_VERSION), opts_.updatedFrom),
                   false);
    }
    // a game whose code an update left behind (or an update unpacked by hand) builds it now, not on its next start
    connect(controller_, &MatchController::codePrepChanged, this, [this](const QString& text, bool) {
        showNotice(text, false);
    });
    QTimer::singleShot(opts_.updatedFrom.isEmpty() ? 1500 : 6000, this, [this] { controller_->prepareStaleCode(); });
}

void MainWindow::warnSmartAppControl() {
    QMessageBox box(QMessageBox::Warning, tr("Smart App Control is on"), GameProcess::smartAppControlHelp(false),
                    QMessageBox::Ok, this);
    QPushButton* open = box.addButton(tr("Open Windows Security"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:windowsdefender")));
}

QWidget* MainWindow::buildHeader() {
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("Header"));
    header->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(24, 12, 24, 12);
    h->setSpacing(16);
    auto* emblem = new QLabel;
    emblem->setObjectName(QStringLiteral("Emblem"));
    emblem->setPixmap(style::logo(72));
    h->addWidget(emblem);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(12);
    auto* main = ui::label(QStringLiteral("SCV"), "TitleMain");
    auto* sub = ui::label(QStringLiteral("ONLINE"), "TitleSub");
    ui::letterSpacing(main, 3);
    ui::letterSpacing(sub, 8);
    titleRow->addWidget(main);
    titleRow->addWidget(sub);
    titleRow->addStretch();
    titles->addLayout(titleRow);
    auto* tag = ui::label(tr("SOULCALIBUR V ON PC  ·  ROLLBACK NETPLAY  ·  BUILT FROM YOUR OWN DISC"), "Tagline");
    ui::letterSpacing(tag, 2);
    titles->addWidget(tag);
    h->addLayout(titles);
    h->addStretch();
    return header;
}

QWidget* MainWindow::buildNav() {
    auto* nav = new QWidget;
    nav->setObjectName(QStringLiteral("NavBar"));
    nav->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(nav);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(4);
    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    const QStringList names = {tr("PLAY"), tr("BUILD GAME"), tr("SETTINGS"), tr("SUPPORT")};
    for (int i = 0; i < names.size(); ++i) {
        auto* b = new QPushButton(names[i]);
        b->setObjectName(QStringLiteral("NavButton"));
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);  // never squeezed into a cut-off name
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        ui::letterSpacing(b, 1.5);
        navGroup_->addButton(b, i);
        h->addWidget(b);
    }
    h->addStretch();
    connect(navGroup_, &QButtonGroup::idClicked, this, &MainWindow::selectPage);
    return nav;
}

QWidget* MainWindow::buildFooter() {
    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("Footer"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(footer);
    h->setContentsMargins(16, 6, 16, 6);
    h->setSpacing(14);
    connDot_ = new QLabel(QStringLiteral("●"));
    connDot_->setObjectName(QStringLiteral("Dot"));
    connText_ = new QLabel;
    auto* version = new QLabel(QStringLiteral("SCV Online " SC5_LAUNCHER_VERSION));
    h->addWidget(connDot_);
    h->addWidget(connText_);
    h->addStretch();
    h->addWidget(version);
    return footer;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!darkApplied_) {
        winutil::useDarkTitleBar(this);
        darkApplied_ = true;
        if (stack_->currentIndex() == kPlay) play_->focusPlay();
    }
}

void MainWindow::selectPage(int index) {
    if (index < 0 || index >= pages_.size()) return;
    stack_->setCurrentIndex(index);
    if (QAbstractButton* b = navGroup_->button(index)) b->setChecked(true);
    cfg::set(QStringLiteral("ui/page"), QLatin1String(kPageNames[index]));
}

void MainWindow::connectServer() {
    lastError_.clear();
    server_->connectTo(cfg::serverAddress(), cfg::fingerprint());
}

void MainWindow::updateFooter() {
    QString color = QStringLiteral("#5F6674"), text;
    switch (server_->state()) {
    case ServerClient::State::Offline:
        color = QStringLiteral("#FF6A5A");
        text = lastError_.isEmpty() ? tr("Offline") : tr("Offline: %1").arg(lastError_);
        break;
    case ServerClient::State::Connecting:
        color = QStringLiteral("#F0B35A");
        text = tr("Connecting to %1...").arg(cfg::serverAddress());
        break;
    case ServerClient::State::Online:
        color = QStringLiteral("#E8C25A");
        text = tr("Connected to %1 (not signed in)").arg(cfg::serverAddress());
        break;
    case ServerClient::State::LoggedIn:
        color = QStringLiteral("#3FC66B");
        text = tr("Connected to %1 as %2").arg(cfg::serverAddress(), server_->accountName());
        break;
    }
    connDot_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    connText_->setText(text);
}

void MainWindow::checkGameBuilt() {
    QString text;
    const QString exe = cfg::gameExe(kGame);
    const bool realGame = QFileInfo(exe).suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0;
    if (!playError_.isEmpty()) {
        text = playError_;
    } else if (realGame && GameProcess::xexFile(kGame).isEmpty()) {
        text = tr("The game is not built yet. Open Build game and give it your Soulcalibur V disc image and its title "
                  "update.");
    } else if (const QString problem = realGame ? GameProcess::updateProblem(kGame) : QString(); !problem.isEmpty()) {
        text = problem + QStringLiteral("\n\n") + GameProcess::updateFix(kGame);
    } else if (realGame && !QFileInfo::exists(exe)) {
        text = tr("The game's code is not built yet. Open Build game and press CHECK AND BUILD.");
    } else if (realGame && paths::isPackage() && GameProcess::needsRebuild(kGame)) {
        text = tr("This version of SCV Online changed the game: open Build game and press CHECK AND BUILD (a few "
                  "minutes at most).");
    }
    play_->setProblem(text, !text.isEmpty() && !controller_->playerGameRunning());
}

void MainWindow::play() {
    if (controller_->playerGameRunning()) {
        focusGame();
        return;
    }
    if (build_->busy()) {
        playError_ = tr("Build game is still working: press PLAY once it is done.");
        checkGameBuilt();
        return;
    }
    QString err;
    if (!controller_->startPlayerGame(&err)) {
        playError_ = err;
        applog::write(QStringLiteral("play: %1").arg(err));
        checkGameBuilt();
        return;
    }
    playError_.clear();
    checkGameBuilt();
}

// a launcher that never signed in on this PC opens the account window by itself the first time it is connected in a
// run, to make one; not for the command line's sign-in or the development runs that open with a task
void MainWindow::askForAccount() {
    if (askedForAccount_ || !opts_.user.isEmpty() || opts_.localTest || !opts_.extractImage.isEmpty() ||
        !opts_.page.isEmpty())
        return;
    if (server_->state() != ServerClient::State::Online) return;
    if (!cfg::get(QStringLiteral("account/token")).toString().isEmpty() ||
        !cfg::get(QStringLiteral("account/name")).toString().isEmpty())
        return;
    askedForAccount_ = true;
    applog::write(QStringLiteral("account: none on this PC yet, asking for one"));
    openAccountDialog(true);
}

void MainWindow::openAccountDialog(bool newAccount) {
    if (!server_->isOnline()) {
        showNotice(tr("Not connected to the server yet: connecting again. Check the address in Settings if it does not "
                      "connect."),
                   true);
        connectServer();
        return;
    }
    LoginDialog dlg(server_, cfg::get(QStringLiteral("account/name")).toString(), this);
    if (newAccount) dlg.askForNewAccount();
    winutil::useDarkTitleBar(&dlg);
    if (dlg.exec() == QDialog::Accepted) {
        cfg::set(QStringLiteral("account/remember"), dlg.remember());
        cfg::set(QStringLiteral("account/name"), dlg.name());
        cfg::set(QStringLiteral("account/token"), dlg.remember() ? server_->account().value(QStringLiteral("token")).toString()
                                                                 : QString());
    }
}

void MainWindow::snap(const QString& name) {
    QDir().mkpath(opts_.snapDir);
    const QString path = QDir(opts_.snapDir).filePath(name + QStringLiteral(".png"));
    applog::write(QStringLiteral("snap: %1 %2 (window %3x%4)")
                      .arg(path, grab().save(path) ? QStringLiteral("saved") : QStringLiteral("failed"))
                      .arg(width()).arg(height()));
}

void MainWindow::onServerMessage(const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("welcome") && msg.contains(QStringLiteral("update")))
        offerUpdate(msg.value(QStringLiteral("update")), false);
    else if (type == QLatin1String("update"))
        offerUpdate(msg, false);
    else if (type == QLatin1String("error") && msg.value(QStringLiteral("code")).toString() == QLatin1String("client_outdated"))
        offerUpdate(msg.value(QStringLiteral("update")), true);
    if (type == QLatin1String("welcome")) {
        const QString motd = msg.value(QStringLiteral("motd")).toString();
        if (!motd.isEmpty()) showNotice(motd, false);
        server_->send({{QStringLiteral("type"), QStringLiteral("online")}});
    } else if (type == QLatin1String("notice")) {
        showNotice(msg.value(QStringLiteral("text")).toString(), msg.value(QStringLiteral("level")).toString() == QLatin1String("warn"));
    }
}

void MainWindow::popUp(QWidget* dialog) {
    if (isMinimized()) showNormal();  // a dialog of a minimized window stays hidden
    const bool front = winutil::bringToFront(dialog);
    applog::write(QStringLiteral("popup: %1 %2").arg(dialog->windowTitle(), front ? QStringLiteral("in front")
                                                                                 : QStringLiteral("not in front")));
}

void MainWindow::focusGame() {
    GameProcess* g = controller_->game();
    if (!g || !g->pid()) return;
    const bool front = winutil::bringProcessToFront(g->pid());
    applog::write(QStringLiteral("popup: the game %1").arg(front ? QStringLiteral("in front")
                                                                 : QStringLiteral("not brought to the front")));
}

// once a run (LATER leaves the Play page's button to come back to it), every time when this launcher is too old for
// the server; --auto-update installs it without asking (tests)
void MainWindow::offerUpdate(const QJsonValue& value, bool required) {
    const QJsonObject release = Updater::usable(value);
    if (release.isEmpty()) return;
    const QString version = release.value(QStringLiteral("version")).toString();
    const bool otherBuild = Updater::sameVersionOtherBuild(release);
    applog::write(QStringLiteral("update: %1 offered%2%3")
                      .arg(version, required ? QStringLiteral(", needed to play online") : QString(),
                           otherBuild ? QStringLiteral(" (the release's build %1; this one is %2)")
                                            .arg(release.value(QStringLiteral("build")).toString(), paths::packageBuild())
                                      : QString()));
    update_ = release;
    const QString text = otherBuild ? tr("You have a test build: everyone else plays the release of %1.").arg(version)
                         : required ? tr("SCV Online %1 is out, and the server needs it to play online.").arg(version)
                                    : tr("SCV Online %1 is out. Players on another version cannot play you.").arg(version);
    play_->setUpdate(otherBuild ? tr("GET THE RELEASE") : tr("UPDATE TO %1").arg(version),
                     text + QLatin1Char(' ') + tr("It is installed once the game is closed."));
    bridge_->setUpdate(version, text + QLatin1Char(' ') + tr("Close the game and update on SCV Online's Play page."));
    if (updater_->busy()) return;
    if (opts_.autoUpdate && Updater::blocker().isEmpty()) {
        startUpdate();
        return;
    }
    if (updateDialog_ || (updateDismissed_ == version && !required)) return;
    if (!gamesBusy().isEmpty()) {  // not over the game: once it is closed
        offerPending_ = true;
        offerRequired_ = offerRequired_ || required;
        return;
    }
    showUpdateDialog(required);
}

QString MainWindow::gamesBusy() const {
    if (controller_->playerGameRunning()) return tr("you close the game");
    if (controller_->phase() != MatchController::Phase::Idle) return tr("this is over: %1").arg(controller_->phaseText());
    if (build_->busy()) return tr("Build game is done");
    if (controller_->preparingCode()) return tr("the game is prepared for this version");
    return {};
}

void MainWindow::showUpdateDialog(bool required) {
    if (update_.isEmpty()) return;
    if (updateDialog_) {
        popUp(updateDialog_);
        return;
    }
    auto* dlg = new UpdateDialog(update_, required, Updater::blocker(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    updateDialog_ = dlg;
    connect(dlg, &UpdateDialog::updateNow, this, &MainWindow::startUpdate);
    connect(dlg, &UpdateDialog::cancelRequested, this, [this] {
        installPending_ = false;
        updater_->cancel();
    });
    connect(dlg, &QDialog::rejected, this, [this] {
        updateDismissed_ = update_.value(QStringLiteral("version")).toString();
        installPending_ = false;
        updater_->cancel();  // closed while it downloaded: nothing installed
    });
    popUp(dlg);
    QApplication::alert(this);
}

void MainWindow::startUpdate() {
    if (updater_->busy() || update_.isEmpty()) return;
    updater_->start(update_);
}

// The downloaded release goes in once nothing uses the game's files: the game closed, no match or build under way.
// Then the new launcher starts and this one closes (Updater::installed).
void MainWindow::installUpdateWhenFree() {
    if (!updater_->busy()) {
        installPending_ = false;
        return;
    }
    if (const QString busy = gamesBusy(); !busy.isEmpty()) {
        installPending_ = true;
        if (updateDialog_) updateDialog_->setWaiting(tr("Downloaded. It goes in once %1.").arg(busy));
        return;
    }
    installPending_ = false;
    controller_->shutdown();
    updater_->install();
}

void MainWindow::showNotice(const QString& text, bool warn) {
    notice_->setText(text);
    // large and plain: what the launcher has to tell the player (a warning bold, white on dark orange)
    notice_->setStyleSheet(warn ? QStringLiteral("background:#6B3A0A; color:#FFFFFF; padding:12px 24px; font-size:15pt; "
                                                 "font-weight:700;")
                                : QStringLiteral("background:#101829; color:#FFFFFF; padding:12px 24px; font-size:15pt; "
                                                 "border-bottom:1px solid #B8923A;"));
    notice_->show();
    noticeTimer_.start(20000);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // the game is part of SCV Online: closing the launcher closes it too (the system ends it with the launcher anyway)
    if (controller_->playerGameRunning() && opts_.user.isEmpty()) {
        const auto answer = QMessageBox::question(this, tr("Close SCV Online?"),
                                                  tr("Your game is running. Closing SCV Online closes the game too. "
                                                     "Close both?"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    controller_->shutdown();
    server_->disconnectFromServer();
    event->accept();
}
