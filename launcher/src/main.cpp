// SCV Online launcher for the native PC port of Soulcalibur V (Xbox 360): builds the game from the player's own disc,
// starts it (PLAY), and keeps everything online that the game's own menus ask for in the background: the server
// connection, matchmaking, lobbies, the UDP punch, the matches, reports and updates (docs/LAUNCHER.md).

#include <algorithm>
#include <QApplication>
#include <QDir>
#include <QCommandLineParser>
#include <QLockFile>
#include <QMessageBox>
#include <QSslSocket>
#include <QStyleFactory>

#include "AppPaths.h"
#include "GameData.h"
#include "MainWindow.h"
#include "Style.h"
#include "WinUtil.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // the data folder's name (QStandardPaths), kept from before the name SCV Online
    QApplication::setApplicationName(QStringLiteral("SC5 Online"));
    QApplication::setOrganizationName(QStringLiteral("SC5"));
    QApplication::setApplicationVersion(QStringLiteral(SC5_LAUNCHER_VERSION));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("SCV Online launcher"));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption configOpt(QStringLiteral("config"), QStringLiteral("Settings file to use."), QStringLiteral("ini"));
    const QCommandLineOption serverOpt(QStringLiteral("server"), QStringLiteral("Server address host:port."), QStringLiteral("address"));
    const QCommandLineOption fpOpt(QStringLiteral("fingerprint"), QStringLiteral("Pinned SHA-256 certificate fingerprint."), QStringLiteral("hex"));
    const QCommandLineOption userOpt(QStringLiteral("user"), QStringLiteral("Development: sign in as this account."), QStringLiteral("name"));
    const QCommandLineOption pwEnvOpt(QStringLiteral("password-env"),
                                      QStringLiteral("Development: environment variable holding the password (default SC5_DEV_PASSWORD)."),
                                      QStringLiteral("var"));
    const QCommandLineOption regOpt(QStringLiteral("register"), QStringLiteral("Development: create the --user account if needed."));
    const QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Start the game once the window is up, as PLAY does."));
    const QCommandLineOption queueOpt(QStringLiteral("queue"),
                                      QStringLiteral("Development: start the game and search, as its menu asks, once signed in."),
                                      QStringLiteral("casual|ranked"));
    const QCommandLineOption gameOpt(QStringLiteral("game"), QStringLiteral("Game to select (sc5, the only one)."), QStringLiteral("id"));
    const QCommandLineOption modeOpt(QStringLiteral("mode"), QStringLiteral("Mode for --queue."), QStringLiteral("id"));
    const QCommandLineOption charOpt(QStringLiteral("character"), QStringLiteral("Character for --queue."), QStringLiteral("id"));
    const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to open."), QStringLiteral("play|build|settings|support"));
    const QCommandLineOption localTestOpt(QStringLiteral("local-test"),
                                          QStringLiteral("Development: start the two-instance loopback test."));
    const QCommandLineOption extractOpt(QStringLiteral("extract"), QStringLiteral("Open Build game and extract this disc image."),
                                        QStringLiteral("image"));
    const QCommandLineOption acceptOpt(QStringLiteral("auto-accept"), QStringLiteral("Development: accept found matches after N seconds."),
                                       QStringLiteral("seconds"));
    const QCommandLineOption cancelOpt(
        QStringLiteral("cancel-search"),
        QStringLiteral("Development: cancel the --queue search N seconds after it starts, as CANCEL SEARCH does."),
        QStringLiteral("seconds"));
    const QCommandLineOption lobbyOpt(QStringLiteral("lobby"),
                                      QStringLiteral("Development: host a lobby (start once both are ready), join it "
                                                     "(ready at once) or watch it (as a spectator)."),
                                      QStringLiteral("host|join|watch"));
    const QCommandLineOption lobbyNameOpt(QStringLiteral("lobby-name"), QStringLiteral("Development: the lobby's name for --lobby."),
                                          QStringLiteral("name"));
    const QCommandLineOption lobbyCodeFileOpt(
        QStringLiteral("lobby-code-file"),
        QStringLiteral("Development: --lobby host writes its lobby's code to this file, join reads it from there and "
                       "joins by the code (Online Practice lobbies are never listed)."),
        QStringLiteral("path"));
    const QCommandLineOption upnpProbeOpt(QStringLiteral("upnp-probe"),
                                          QStringLiteral("Find the router (UPnP) and its public address; open no port."));
    const QCommandLineOption lobbyMatchesOpt(QStringLiteral("lobby-matches"),
                                             QStringLiteral("Development: matches to play in the --lobby (default 1)."),
                                             QStringLiteral("n"));
    const QCommandLineOption leaveLobbyOpt(
        QStringLiteral("leave-lobby"),
        QStringLiteral("Development: once the --lobby-matches are played, leave the lobby (its game then closes)."));
    const QCommandLineOption leaveLobbyInMatchOpt(
        QStringLiteral("leave-lobby-in-match"),
        QStringLiteral("Development: leave the lobby this many seconds into its first match (its game closes at once)."),
        QStringLiteral("seconds"));
    const QCommandLineOption kothOpt(
        QStringLiteral("koth"),
        QStringLiteral("Development: --lobby host makes it a King of the Hill lobby (the winner stays on, the rest wait "
                       "in line and watch)."));
    const QCommandLineOption rateOpt(
        QStringLiteral("rate"),
        QStringLiteral("Development: in a King of the Hill lobby, rate the winner of each match watched (1 to 10), "
                       "once this game has shown its end."),
        QStringLiteral("n"));
    const QCommandLineOption snapDirOpt(
        QStringLiteral("snap-dir"),
        QStringLiteral("Development: with --page, save a picture of that page to this folder."),
        QStringLiteral("folder"));
    const QCommandLineOption rematchesOpt(
        QStringLiteral("rematches"),
        QStringLiteral("Development: after a --queue casual match, choose a rematch this many times (a ranked set "
                       "plays on by itself)."),
        QStringLiteral("n"));
    const QCommandLineOption rematchChoiceOpt(
        QStringLiteral("rematch-choice"),
        QStringLiteral("Development: what each --rematches choice is, rematch (the same characters) or select (both "
                       "pick again), comma-separated; the last one repeats (default rematch)."),
        QStringLiteral("choices"));
    const QCommandLineOption leaveSeriesOpt(
        QStringLiteral("leave-series"),
        QStringLiteral("Development: once the --rematches are chosen, leave the series at once instead of letting it run out."));
    const QCommandLineOption lobbyOwnFirstOpt(
        QStringLiteral("lobby-own-first"),
        QStringLiteral("Development: with --lobby join, create a lobby of one's own first, then join the named one "
                       "from it (the server moves the player)."));
    const QCommandLineOption updatedOpt(
        QStringLiteral("updated"),
        QStringLiteral("Started by an update of this version: waits for it to close, then deletes what it set aside."),
        QStringLiteral("version"));
    const QCommandLineOption autoUpdateOpt(QStringLiteral("auto-update"),
                                           QStringLiteral("Development: install an update offered at once, without asking."));
    for (const auto* o : {&configOpt, &serverOpt, &fpOpt, &userOpt, &pwEnvOpt, &regOpt, &playOpt, &queueOpt, &gameOpt, &modeOpt,
                          &charOpt, &pageOpt, &acceptOpt, &cancelOpt, &localTestOpt, &extractOpt, &lobbyOpt, &lobbyNameOpt, &lobbyCodeFileOpt,
                          &lobbyMatchesOpt, &leaveLobbyOpt, &leaveLobbyInMatchOpt, &kothOpt, &rateOpt, &snapDirOpt, &upnpProbeOpt, &rematchesOpt, &rematchChoiceOpt,
                          &leaveSeriesOpt, &lobbyOwnFirstOpt, &updatedOpt, &autoUpdateOpt})
        cli.addOption(*o);
    cli.process(app);

    paths::init(cli.value(configOpt));
    winutil::stayBehind(cfg::devBackground());
    applog::write(QStringLiteral("launcher %1 starting, settings %2").arg(QStringLiteral(SC5_LAUNCHER_VERSION), paths::configFile()));
    // what an older version kept that nothing reads any more (the sound levels, the queue pages' boxes)
    if (const QStringList removed = cfg::dropRemovedSettings(); !removed.isEmpty())
        applog::write(QStringLiteral("settings: took out what this version no longer uses: %1").arg(removed.join(QStringLiteral(", "))));

    // not "<ini>.lock": QSettings locks that file itself while it syncs. Started by an update, the old launcher is
    // still closing: wait for it.
    QLockFile lock(paths::configFile() + QStringLiteral(".instance"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(cli.isSet(updatedOpt) ? 30000 : 200)) {
        QMessageBox::information(nullptr, QStringLiteral("SCV Online"),
                                 QStringLiteral("SCV Online is already running with these settings."));
        return 0;
    }

    QString error;
    if (!GameData::instance().load(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("SCV Online"), error);
        return 1;
    }
    if (QSslSocket::availableBackends().contains(QStringLiteral("schannel")))
        QSslSocket::setActiveBackend(QStringLiteral("schannel"));
    applog::write(QStringLiteral("tls backend %1, %2").arg(QSslSocket::activeBackend(), QSslSocket::sslLibraryVersionString()));

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(style::palette());
    app.setStyleSheet(style::sheet());
    app.setWindowIcon(style::appIcon());

    LaunchOptions opts;
    opts.server = cli.value(serverOpt);
    opts.fingerprint = cli.value(fpOpt);
    opts.user = cli.value(userOpt);
    if (cli.isSet(pwEnvOpt)) opts.passwordEnv = cli.value(pwEnvOpt);
    opts.registerAccount = cli.isSet(regOpt);
    opts.play = cli.isSet(playOpt);
    opts.queue = cli.value(queueOpt);
    opts.game = cli.value(gameOpt);
    opts.mode = cli.value(modeOpt);
    opts.character = cli.value(charOpt);
    opts.page = cli.value(pageOpt);
    opts.autoAcceptSeconds = cli.isSet(acceptOpt) ? cli.value(acceptOpt).toInt() : -1;
    opts.cancelSearchSeconds = cli.isSet(cancelOpt) ? std::max(0, cli.value(cancelOpt).toInt()) : -1;
    opts.localTest = cli.isSet(localTestOpt);
    opts.extractImage = cli.value(extractOpt);
    opts.lobby = cli.value(lobbyOpt);
    opts.lobbyName = cli.value(lobbyNameOpt);
    opts.lobbyCodeFile = cli.value(lobbyCodeFileOpt);
    opts.lobbyMatches = cli.isSet(lobbyMatchesOpt) ? std::max(1, cli.value(lobbyMatchesOpt).toInt()) : 1;
    opts.leaveLobby = cli.isSet(leaveLobbyOpt);
    opts.leaveLobbyInMatch = cli.isSet(leaveLobbyInMatchOpt) ? std::max(1, cli.value(leaveLobbyInMatchOpt).toInt()) : 0;
    opts.koth = cli.isSet(kothOpt);
    opts.snapDir = cli.value(snapDirOpt);
    opts.rate = cli.isSet(rateOpt) ? std::clamp(cli.value(rateOpt).toInt(), 1, 10) : 0;
    opts.rematches = cli.isSet(rematchesOpt) ? std::max(0, cli.value(rematchesOpt).toInt()) : 0;
    opts.rematchChoices = cli.value(rematchChoiceOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
    opts.leaveSeries = cli.isSet(leaveSeriesOpt);
    opts.lobbyOwnFirst = cli.isSet(lobbyOwnFirstOpt);
    opts.upnpProbe = cli.isSet(upnpProbeOpt);
    opts.updatedFrom = cli.value(updatedOpt);
    opts.autoUpdate = cli.isSet(autoUpdateOpt);

    MainWindow window(opts);
    window.show();
    const int rc = app.exec();
    applog::write(QStringLiteral("launcher exiting (%1)").arg(rc));
    return rc;
}
