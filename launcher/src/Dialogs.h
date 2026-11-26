#pragma once
// Account sign-in and registration, and the update offer. (The game draws MATCH FOUND, the rematch choice and the
// lobbies itself since SCV 0.1.8: docs/PROTOCOL.md 3.8.)

#include <QDialog>
#include <QJsonObject>
#include <QTimer>

#include "GameData.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class ServerClient;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(ServerClient* server, const QString& lastName, QWidget* parent = nullptr);
    bool remember() const;
    QString name() const;
    void askForNewAccount();  // opened by itself on a PC with no account yet: a new one, and why it is asked

private:
    void setRegistering(bool on);
    void submit();
    void onMessage(const QJsonObject& msg);

    ServerClient* server_;
    QLabel* title_;
    QLabel* intro_;
    QLabel* error_;
    QLineEdit* name_;
    QLineEdit* password_;
    QLineEdit* confirm_;
    QLabel* confirmLabel_;
    QCheckBox* remember_;
    QPushButton* submit_;
    QPushButton* switch_;
    bool registering_ = false;
    int pendingId_ = -1;
};

// A newer release the server names (Updater): what it is and what is new, UPDATE NOW or LATER; while it downloads and
// installs, how far it is (CANCEL until the files go in). `required`: this launcher is too old for the server;
// `blocker`: why it cannot update itself (UPDATE NOW opens the release's page instead).
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    UpdateDialog(const QJsonObject& release, bool required, const QString& blocker, QWidget* parent = nullptr);
    void setProgress(const QString& text, int percent);  // percent -1: no measure
    void setWaiting(const QString& text);                // downloaded; installs once the match or the game is over
    void setFailed(const QString& text);                 // nothing changed: TRY AGAIN or LATER

signals:
    void updateNow();
    void cancelRequested();

private:
    QLabel* status_;
    QProgressBar* bar_;
    QPushButton* now_;
    QPushButton* later_;
    bool running_ = false;
};
