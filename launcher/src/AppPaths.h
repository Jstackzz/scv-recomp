#pragma once
// Where the launcher keeps its settings, logs and per-session folders, and typed settings access.

#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace paths {
void init(const QString& configOverride);  // call once, before anything else
QString configFile();                        // sc5-launcher.ini
QString dataDir();                           // folder of the ini: sessions\, sc5-launcher.log
QString sessionsDir();
QString logFile();
QString detectedRepoRoot();                  // walks up from the exe looking for tools\py\x360.py
bool isPackage();                            // a player package: games\ beside the launcher
QString packagedGameFolder(const QString& gameId);  // games\<id> beside the launcher (a player package), or empty
QString toolchainDir();                      // toolchain\ beside the launcher: what compiles the game's code
QString displayFile();                       // display.txt beside the ini: the display settings for running games
// which build of its version this launcher is: a player package's VERSION.txt stamp ("SC5-Online 0.1.4, packaged
// 2026-11-23 22:31" gives "2026-11-23 22:31"); empty in a source build. Two packages of one version (a test build
// and the release) differ there, which the version number alone never shows (docs/PROTOCOL.md 1.3)
QString packageBuild();
}  // namespace paths

namespace cfg {
QSettings& store();
QVariant get(const QString& key, const QVariant& fallback = {});
void set(const QString& key, const QVariant& value);

QString serverAddress();
QString fingerprint();
QString repoRoot();
QString python();
// the game's folder: games\sc5 beside the launcher in a player package, the repository's game\ in a source build;
// below it disc\ (the extracted disc with the title update's default.xexp beside default.xex), update\ (the title
// update's other files), dlc\ (the player's own DLC packages), user\ (single player's saves) and cache\ (shaders)
QString gameFolder(const QString& gameId);
QString gameExe(const QString& gameId);  // sc5.exe: in the game folder (a package) or the sc5\ build output
QString gameDataDir(const QString& gameId);    // <game folder>\disc (SC5_GAME_DIR)
QString gameUpdateDir(const QString& gameId);  // <game folder>\update (SC5_UPDATE_DIR)
QString gameDlcDir(const QString& gameId);     // <game folder>\dlc (SC5_DLC_DIR)
QString gameUserDir(const QString& gameId);    // <game folder>\user (SC5_USER_DIR, single player)
// the offline game's own save in gameUserDir (<profile>\4E4D083D\00000001\SC5SaveData\savedata.bin, the newest if
// several profiles have one), where its created characters are: an online game reads them from it (SC5_CREATION_SAVE,
// read only); empty when there is none
QString gameSaveFile(const QString& gameId);
QString gameCacheDir(const QString& gameId);   // <game folder>\cache (SC5_CACHE_DIR: the shader cache, kept)
// what compiles the game's code for this PC once the disc is extracted (BuildPage::compileGame): the game folder's
// build-game.cmd unless set (games/<id>/build_script); its arguments games/<id>/build_args
QString buildScript(const QString& gameId);
QString buildArgs(const QString& gameId);
QString region();
int maxPing();
bool strict();
// the player picks their character on the game's own select screen in online matches, not in the launcher
bool pickInGame();
// a Ranked or Casual search starts the game's practice to play while it waits (MatchController::maybeStartPractice)
bool practiceWhileWaiting();
// once a Ranked set or a Casual series is over (not left by this player), the same search starts again by itself
// (MatchController::maybeSearchAfterSeries)
bool searchAgain();
bool matchSound();  // a chime when a Ranked or Casual match is found (the MATCH FOUND window comes up)
// The input delay: kAutoDelay (the default since 0.1.4), the match's from its measured ping (the server's choice,
// match_go's delay; autoDelayFor), or the player's own 0-4 frames, which a match plays when it is the larger of the
// two players'. Settings, ONLINE ([online] input_delay: "auto" or the number)
constexpr int kAutoDelay = -1;
int inputDelay();
QString inputDelayText(int delay);  // "Auto (from the ping)" or "2 frames"
// what Auto comes to for a round trip: the one-way trip's frames rounded up, less one, 1 to 6 (the server's
// protocol.auto_delay: 1 under 67 ms, 2 to 100, 3 to 133, 4 to 166, 5 to 200, then 6)
int autoDelayFor(int rttMs);
// what a lobby_ready or queue_join says of it: "delay" a number (Auto: 1, what a server from before Auto takes) and
// "delay_auto" true for Auto
void putDelay(QJsonObject& msg, int delay);
int gamePort();
// The display settings (Settings, DISPLAY): each changes only how a frame looks, never what the game computes, so
// two players may set them differently in one match (docs/PROTOCOL.md 3.1)
QString displayMode();  // "window", "borderless" (default) or "fullscreen"
QString resolution();   // "desktop" (default) or "<w>x<h>": the window's or the full screen's size
int renderScale();      // the internal resolution, times the console's 1280x720: 1 (default) to 4
QString antiAliasing(); // "off" (default), "fxaa" or "fxaa_extreme": smoothing of the finished frame
int anisotropic();      // texture filtering: 0 the game's own (default), else 2, 4, 8 or 16 times anisotropic
bool vsync();
bool adaptiveSync();  // with vsync, a G-SYNC or FreeSync monitor: the game's frames may tear for it to follow them
int brightness();     // percent, 100 as the game draws it
bool frameData();     // practice shows player 1's frame data (SC5_FRAMEDATA)
// the side the player wants to start a fight on, whichever player they are: "off", "left" or "right"
// (SC5_PREFERRED_SIDE: the game shows the fight mirrored when they would start on the other side)
QString preferredSide();
// the game's sound levels, percent of the game's own (100), 0 to 150: "master", "music", "effects", "voices"
// (SC5_VOLUME_<NAME>; the display file's volume_<name>)
QStringList volumeNames();
int volume(const QString& name);
// paths::displayFile() written again from the display settings: a running game follows it (SC5_DISPLAY_FILE). set()
// does it for every display/ key.
void writeDisplayFile();
int localTestPort(int instance);
bool udpLoopback();  // development: matches on loopback only (one-PC tests, no firewall prompt)
bool udpBlocked();   // development: nothing goes to the server's UDP port, as from a PC whose UDP never gets there
bool devBackground();  // development: tests on a PC in use: games start minimized, no window comes to the front
bool upnp();         // open the game UDP port on the router (UPnP)
bool hideIp();       // matches through the server's relay: no opponent learns this player's address
bool hideIpAlways(); // ... even when the ping that way is high (else such a match tries a direct connection)
}  // namespace cfg

namespace applog {
void write(const QString& line);  // timestamped line in sc5-launcher.log
}
