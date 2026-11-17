#pragma once
// "Build game" (the player's Xbox 360 disc, title update and DLC -> the game's folders -> its code compiled) and
// Settings.

#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

#include "Widgets.h"
#include "X360.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;
class GameProcess;
class ServerClient;
class UpnpMapper;

class BuildPage : public Page {
    Q_OBJECT
public:
    explicit BuildPage(QWidget* parent = nullptr);
    ~BuildPage() override;
    void setGame(const QString& gameId) override;
    void extractImage(const QString& path);  // fill in the image and start, as if CHECK AND BUILD was clicked
    bool busy() const { return step_ != Step::Idle; }  // checking, extracting or compiling

signals:
    void busyChanged(bool busy);

private:
    enum class Step { Idle, Checking, Extracting, Compiling };
    // what the check found (a worker thread fills it in): the files the build needs, read and checked
    struct Check {
        bool ok = false;
        QString error;      // why the build cannot go on (shown, nothing extracted)
        QStringList lines;  // what was found, for the log
        bool extractDisc = false;  // a disc image was given (else the disc folder already holds the right one)
        qint64 discBytes = 0, discMissingBytes = 0;
        int discFiles = 0;
        QByteArray xexp, patchCpk;  // the title update's files, when a package was given
        QStringList dlc;            // the DLC packages to install
        qint64 dlcBytes = 0;
    };
    void refreshInfo();
    void startBuild();
    void onChecked(const std::shared_ptr<x360::Disc>& disc, const std::shared_ptr<Check>& c);
    void onExtracted(bool ok, const QString& error);
    // the one step after extraction: compiles the game's code for this PC (cfg::buildScript), or says it is not
    // there yet; also the "Compile the game's code" button
    void compileGame();
    void runWorker(std::function<void()> work, std::function<void()> done);
    void finishStep();
    void updateProgress();
    void setBusy(bool busy);
    void showDlc();

    QLabel* gameTitle_;
    QLabel* folderInfo_;
    QLabel* discInfo_;
    QLabel* updateInfo_;
    QLabel* dlcInfo_;
    QLabel* exeInfo_;
    QLineEdit* image_;
    QLineEdit* update_;
    QLabel* dlcList_;
    QPushButton* browse_;
    QPushButton* browseUpdate_;
    QPushButton* addDlc_;
    QPushButton* clearDlc_;
    QPushButton* build_;
    QPushButton* compile_;
    QPushButton* cancel_;
    QProgressBar* progress_;
    LogView* log_;
    QThread* worker_ = nullptr;
    QProcess* compiler_ = nullptr;
    QByteArray compilerOut_;
    Step step_ = Step::Idle;
    std::atomic<bool> stop_{false};
    std::atomic<qint64> bytesDone_{0};
    std::atomic<int> filesDone_{0};
    qint64 totalBytes_ = 0;
    int totalFiles_ = 0;
    QTimer progressTimer_;
    QStringList dlc_;  // the DLC packages picked (saved: games/<id>/dlc)
};

class SettingsPage : public Page {
    Q_OBJECT
public:
    explicit SettingsPage(ServerClient* server, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void setUpnp(UpnpMapper* upnp);  // the router's state on the page; the game port's changes go to it

signals:
    void reconnectRequested();

private:
    void reloadServer();
    ServerClient* server_;
    QLineEdit* address_;
    QLineEdit* fingerprint_;
    QCheckBox* autoConnect_;
    QLabel* serverState_;
    QLabel* presented_;
    QLabel* routerState_;
    QLabel* hideIpCost_;  // the ping hiding the IP address costs from here (shown while it is on)
    UpnpMapper* upnp_ = nullptr;
};
