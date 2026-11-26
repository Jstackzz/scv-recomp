#include "Dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "AppPaths.h"
#include "ServerClient.h"
#include "Style.h"
#include "Updater.h"
#include "Widgets.h"
#include "WinUtil.h"

// --- LoginDialog ----------------------------------------------------------------------------------

LoginDialog::LoginDialog(ServerClient* server, const QString& lastName, QWidget* parent)
    : QDialog(parent), server_(server) {
    setWindowTitle(tr("SCV Online account"));
    setModal(true);
    setMinimumWidth(420);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(12);
    title_ = ui::label(QString(), "PageTitle");
    ui::letterSpacing(title_, 3);
    v->addWidget(title_);
    intro_ = new QLabel;
    intro_->setWordWrap(true);
    intro_->hide();
    v->addWidget(intro_);
    auto* hint = ui::label(tr("Names are 3-16 letters, digits, '_' or '-'. Passwords have at least 6 characters."), "Muted");
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    name_ = new QLineEdit(lastName);
    name_->setMaxLength(16);
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    confirm_ = new QLineEdit;
    confirm_->setEchoMode(QLineEdit::Password);
    confirmLabel_ = new QLabel(tr("Repeat password"));
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Password"), password_);
    form->addRow(confirmLabel_, confirm_);
    v->addLayout(form);
    remember_ = new QCheckBox(tr("Remember me on this PC"));
    remember_->setChecked(true);
    v->addWidget(remember_);
    error_ = ui::label(QString(), "Warning");
    error_->setWordWrap(true);
    v->addWidget(error_);

    auto* buttons = new QHBoxLayout;
    switch_ = new QPushButton;
    switch_->setFlat(true);
    submit_ = new QPushButton;
    submit_->setDefault(true);
    buttons->addWidget(switch_);
    buttons->addStretch();
    buttons->addWidget(submit_);
    v->addLayout(buttons);

    connect(switch_, &QPushButton::clicked, this, [this] { setRegistering(!registering_); });
    connect(submit_, &QPushButton::clicked, this, &LoginDialog::submit);
    connect(server_, &ServerClient::message, this, &LoginDialog::onMessage);
    setRegistering(false);
    if (!lastName.isEmpty()) password_->setFocus();
}

bool LoginDialog::remember() const { return remember_->isChecked(); }
QString LoginDialog::name() const { return name_->text().trimmed(); }

void LoginDialog::askForNewAccount() {
    setRegistering(true);
    intro_->setText(tr("Welcome! To play online you need an account on this server: choose a name and a password. "
                       "Have one already? Choose \"I already have an account\". You can also do this later with "
                       "SIGN IN on the Play page."));
    intro_->show();
    name_->setFocus();
}

void LoginDialog::setRegistering(bool on) {
    registering_ = on;
    title_->setText(on ? tr("CREATE ACCOUNT") : tr("SIGN IN"));
    submit_->setText(on ? tr("Create account") : tr("Sign in"));
    switch_->setText(on ? tr("I already have an account") : tr("Create a new account"));
    confirm_->setVisible(on);
    confirmLabel_->setVisible(on);
    if (!on) intro_->hide();  // the welcome is about making one
    error_->clear();
}

void LoginDialog::submit() {
    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));
    const QString n = name(), p = password_->text();
    if (!nameRe.match(n).hasMatch()) {
        error_->setText(tr("Names are 3-16 letters, digits, '_' or '-'."));
        return;
    }
    if (p.size() < 6 || p.size() > 128) {
        error_->setText(tr("Passwords are 6-128 characters."));
        return;
    }
    if (registering_ && p != confirm_->text()) {
        error_->setText(tr("The two passwords differ."));
        return;
    }
    if (!server_->isOnline()) {
        error_->setText(tr("Not connected to the server. Check the address in Settings."));
        return;
    }
    if (server_->isLoggedIn()) server_->logout();
    pendingId_ = registering_ ? server_->registerAccount(n, p) : server_->login(n, p);
    submit_->setEnabled(false);
    error_->setText(tr("Contacting the server..."));
}

void LoginDialog::onMessage(const QJsonObject& msg) {
    if (pendingId_ < 0 || msg.value(QStringLiteral("id")).toInt(-2) != pendingId_) return;
    pendingId_ = -1;
    submit_->setEnabled(true);
    if (msg.value(QStringLiteral("type")).toString() == QLatin1String("account")) {
        password_->clear();
        confirm_->clear();
        accept();
    } else {
        error_->setText(msg.value(QStringLiteral("message")).toString());
    }
}

// --- UpdateDialog ----------------------------------------------------------------------------------

UpdateDialog::UpdateDialog(const QJsonObject& release, bool required, const QString& blocker, QWidget* parent)
    : QDialog(parent) {
    const QString version = release.value(QStringLiteral("version")).toString();
    const QString page = release.value(QStringLiteral("page")).toString();
    setWindowTitle(tr("SCV Online %1").arg(version));
    setModal(false);
    setMinimumWidth(560);
    // over the game: a game window in front (borderless over the whole screen) hid the offer behind it
    setWindowFlag(Qt::WindowStaysOnTopHint);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 22, 28, 22);
    v->setSpacing(10);
    auto* emblem = new QLabel;
    emblem->setPixmap(style::logo(64));
    emblem->setAlignment(Qt::AlignCenter);
    v->addWidget(emblem);
    auto* title = ui::label(required ? tr("UPDATE NEEDED") : tr("UPDATE AVAILABLE"), "CardTitle");
    title->setAlignment(Qt::AlignCenter);
    v->addWidget(title);
    auto* big = ui::label(tr("SCV Online %1").arg(version), "Big");
    big->setAlignment(Qt::AlignCenter);
    v->addWidget(big);
    // the same number in another build: this launcher is a test build (or one made before the release), whose game
    // differs from everyone else's
    const bool otherBuild = Updater::sameVersionOtherBuild(release);
    auto* info = ui::label(otherBuild ? tr("You have a test build of %1 (packaged %2). Everyone else plays the release "
                                           "of %1 (packaged %3), whose game differs from yours: update to play with "
                                           "them.")
                                            .arg(QStringLiteral(SC5_LAUNCHER_VERSION), paths::packageBuild(),
                                                 release.value(QStringLiteral("build")).toString())
                           : required ? tr("You have %1. The server needs this version to play online.")
                                            .arg(QStringLiteral(SC5_LAUNCHER_VERSION))
                                      : tr("You have %1. Players on another version cannot play you: update to play "
                                           "with everyone.").arg(QStringLiteral(SC5_LAUNCHER_VERSION)),
                           "Rank");
    info->setAlignment(Qt::AlignCenter);
    info->setWordWrap(true);
    v->addWidget(info);
    const QString notes = release.value(QStringLiteral("notes")).toString().trimmed();
    if (!notes.isEmpty()) {
        auto* text = new QLabel(notes.left(700));
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        v->addWidget(text);
    }
    if (page.startsWith(QLatin1String("https://"))) {
        auto* link = new QLabel(QStringLiteral("<a href=\"%1\" style=\"color:#F2C14E\">%2</a>")
                                    .arg(page.toHtmlEscaped(), tr("What's new in %1").arg(version)));
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setAlignment(Qt::AlignCenter);
        v->addWidget(link);
    }
    status_ = ui::label(blocker.isEmpty() ? tr("The launcher downloads it, checks it, puts it in place and starts "
                                               "again. Your settings, saves and built games stay.")
                                          : blocker,
                        "Muted");
    status_->setWordWrap(true);
    v->addWidget(status_);
    bar_ = new QProgressBar;
    bar_->setTextVisible(false);
    bar_->setFixedHeight(8);
    bar_->hide();
    v->addWidget(bar_);
    auto* buttons = new QHBoxLayout;
    now_ = new QPushButton(blocker.isEmpty() ? tr("UPDATE NOW") : tr("OPEN THE DOWNLOAD PAGE"));
    now_->setObjectName(QStringLiteral("Primary"));
    now_->setProperty("compact", true);
    now_->setMinimumHeight(40);
    now_->setDefault(true);
    later_ = new QPushButton(tr("LATER"));
    later_->setMinimumHeight(40);
    buttons->addWidget(now_, 1);
    buttons->addWidget(later_, 1);
    v->addLayout(buttons);
    connect(now_, &QPushButton::clicked, this, [this, blocker, page] {
        if (!blocker.isEmpty()) {
            if (!page.isEmpty()) QDesktopServices::openUrl(QUrl(page));
            return;
        }
        running_ = true;
        now_->setEnabled(false);
        later_->setText(tr("CANCEL"));
        bar_->show();
        emit updateNow();
    });
    connect(later_, &QPushButton::clicked, this, [this] {
        if (running_) {
            emit cancelRequested();
            running_ = false;
            now_->setEnabled(true);
            now_->setText(tr("UPDATE NOW"));
            later_->setText(tr("LATER"));
            bar_->hide();
            status_->setText(tr("Cancelled: nothing was changed."));
            return;
        }
        reject();
    });
    now_->setFocus();  // A on a controller, as Enter on the keyboard, updates
    winutil::useDarkTitleBar(this);
}

void UpdateDialog::setProgress(const QString& text, int percent) {
    status_->setText(text);
    bar_->show();
    bar_->setRange(0, percent < 0 ? 0 : 100);  // 0..0: busy without a measure
    if (percent >= 0) bar_->setValue(percent);
}

void UpdateDialog::setWaiting(const QString& text) {
    status_->setText(text);
    bar_->setRange(0, 100);
    bar_->setValue(100);
}

void UpdateDialog::setFailed(const QString& text) {
    running_ = false;
    status_->setText(text);
    bar_->hide();
    now_->setEnabled(true);
    now_->setText(tr("TRY AGAIN"));
    later_->setText(tr("LATER"));
}
