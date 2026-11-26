#include "PlayPage.h"

#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "GameProcess.h"
#include "MatchController.h"
#include "MenuBridge.h"
#include "ServerClient.h"
#include "Style.h"

namespace {
constexpr int kBadge = 72;  // the rank's badge, pixels tall
}

PlayPage::PlayPage(ServerClient* server, MatchController* controller, MenuBridge* bridge, QWidget* parent)
    : Page(parent), server_(server), controller_(controller), bridge_(bridge) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 24, 40, 20);
    root->setSpacing(18);

    // what keeps the game from starting: large, over everything else
    auto* problemRow = new QHBoxLayout;
    problem_ = ui::label(QString(), "Alert");
    problem_->setWordWrap(true);
    problem_->setTextFormat(Qt::PlainText);
    openBuild_ = new QPushButton(tr("OPEN BUILD GAME"));
    openBuild_->setObjectName(QStringLiteral("Primary"));
    openBuild_->setProperty("compact", true);
    problemRow->addWidget(problem_, 1);
    problemRow->addWidget(openBuild_, 0, Qt::AlignVCenter);
    root->addLayout(problemRow);
    problem_->hide();
    openBuild_->hide();

    // the account
    QVBoxLayout* accountBody = nullptr;
    root->addWidget(ui::card(tr("Your account"), &accountBody));
    auto* accountRow = new QHBoxLayout;
    accountRow->setSpacing(18);
    badge_ = new QLabel;
    badge_->setFixedSize(kBadge, kBadge);
    badge_->setAlignment(Qt::AlignCenter);
    accountRow->addWidget(badge_);
    auto* who = new QVBoxLayout;
    who->setSpacing(4);
    name_ = ui::label(QString(), "Huge");
    name_->setTextFormat(Qt::PlainText);
    rank_ = ui::label(QString(), "Rank");
    record_ = ui::label(QString(), "Status");
    record_->setWordWrap(true);
    who->addWidget(name_);
    who->addWidget(rank_);
    who->addWidget(record_);
    accountRow->addLayout(who, 1);
    auto* accountButtons = new QVBoxLayout;
    signIn_ = new QPushButton(tr("SIGN IN"));
    signIn_->setObjectName(QStringLiteral("Primary"));
    signIn_->setProperty("compact", true);
    create_ = new QPushButton(tr("CREATE ACCOUNT"));
    create_->setProperty("big", true);
    signOut_ = new QPushButton(tr("Sign out"));
    signOut_->setProperty("big", true);
    connect_ = new QPushButton(tr("CONNECT AGAIN"));
    connect_->setObjectName(QStringLiteral("Primary"));
    connect_->setProperty("compact", true);
    for (QPushButton* b : {signIn_, create_, signOut_, connect_}) {
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumWidth(220);
        accountButtons->addWidget(b);
    }
    accountButtons->addStretch();
    accountRow->addLayout(accountButtons);
    accountBody->addLayout(accountRow);

    // PLAY: the game, with everything online in its own menus
    play_ = new QPushButton(tr("PLAY"));
    play_->setObjectName(QStringLiteral("Play"));
    play_->setCursor(Qt::PointingHandCursor);
    play_->setMinimumHeight(110);
    play_->setAccessibleName(tr("Play: start the game"));
    root->addWidget(play_);

    // how the game and the connection are doing, in plain words
    gameLine_ = ui::label(QString(), "Status");
    serverLine_ = ui::label(QString(), "Status");
    onlineLine_ = ui::label(QString(), "Status");
    for (QLabel* l : {gameLine_, serverLine_, onlineLine_}) {
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        root->addWidget(l);
    }

    // a newer release
    QVBoxLayout* updateBody = nullptr;
    updateCard_ = ui::card(tr("Update"), &updateBody);
    auto* updateRow = new QHBoxLayout;
    updateText_ = ui::label(QString(), "Rank");
    updateText_->setWordWrap(true);
    update_ = new QPushButton;
    update_->setObjectName(QStringLiteral("Primary"));
    update_->setProperty("compact", true);
    updateRow->addWidget(updateText_, 1);
    updateRow->addWidget(update_);
    updateBody->addLayout(updateRow);
    updateCard_->hide();
    root->addWidget(updateCard_);
    root->addStretch();

    connect(play_, &QPushButton::clicked, this, &PlayPage::playClicked);
    connect(signIn_, &QPushButton::clicked, this, [this] { emit signInClicked(false); });
    connect(create_, &QPushButton::clicked, this, [this] { emit signInClicked(true); });
    connect(signOut_, &QPushButton::clicked, this, &PlayPage::signOutClicked);
    connect(connect_, &QPushButton::clicked, this, &PlayPage::connectClicked);
    connect(update_, &QPushButton::clicked, this, &PlayPage::updateClicked);
    connect(openBuild_, &QPushButton::clicked, this, &PlayPage::buildClicked);
    connect(server_, &ServerClient::stateChanged, this, &PlayPage::refresh);
    connect(bridge_, &MenuBridge::changed, this, &PlayPage::refresh);
    connect(controller_, &MatchController::playerGameChanged, this, &PlayPage::refresh);
    tick_.setInterval(1000);  // a search's time, the counts
    connect(&tick_, &QTimer::timeout, this, &PlayPage::refresh);
    tick_.start();
    refresh();
}

void PlayPage::setUpdate(const QString& button, const QString& text) {
    update_->setText(button);
    updateText_->setText(text);
    updateCard_->setVisible(!button.isEmpty());
}

void PlayPage::setProblem(const QString& text, bool offerBuild) {
    problem_->setText(text);
    problem_->setVisible(!text.isEmpty());
    openBuild_->setVisible(!text.isEmpty() && offerBuild);
}

void PlayPage::pressPlay() {
    if (play_->isEnabled()) play_->animateClick();
}

void PlayPage::focusPlay() { play_->setFocus(Qt::OtherFocusReason); }

void PlayPage::refresh() {
    const QJsonObject ui = bridge_->state();
    const QJsonObject account = ui.value(QStringLiteral("account")).toObject();
    const QString state = account.value(QStringLiteral("state")).toString();
    const bool in = state == QLatin1String("signed_in");
    const QJsonObject stats = bridge_->versusStats();
    const QJsonObject ranked = stats.value(QStringLiteral("ranked")).toObject();
    const QJsonObject casual = stats.value(QStringLiteral("casual")).toObject();
    if (in) {
        name_->setText(server_->accountName());
        const QString title = ranked.value(QStringLiteral("title")).toString();
        rank_->setText(title.isEmpty() ? tr("Ranked: not played yet")
                                       : tr("Rank: %1, %2 Elo").arg(title).arg(ranked.value(QStringLiteral("elo")).toInt()));
        record_->setText(tr("Ranked sets: %1 won, %2 lost.   Casual matches: %3 won, %4 lost.")
                             .arg(ranked.value(QStringLiteral("wins")).toInt())
                             .arg(ranked.value(QStringLiteral("losses")).toInt())
                             .arg(casual.value(QStringLiteral("wins")).toInt())
                             .arg(casual.value(QStringLiteral("losses")).toInt()));
        const QPixmap badge = style::rankBadge(title, kBadge);
        badge_->setPixmap(badge.isNull() ? style::logo(kBadge - 8) : badge);
    } else {
        badge_->setPixmap(style::logo(kBadge - 8));
        const QString error = account.value(QStringLiteral("error")).toString();
        if (state == QLatin1String("signed_out")) {
            name_->setText(tr("Not signed in"));
            rank_->setText(tr("Sign in, or create an account, to play online."));
            record_->setText(tr("Single player works without one."));
        } else if (state == QLatin1String("connecting")) {
            name_->setText(tr("Connecting..."));
            rank_->setText(tr("Connecting to the server."));
            record_->clear();
        } else {
            name_->setText(tr("Offline"));
            rank_->setText(tr("Not connected to the server."));
            record_->setText(error.isEmpty() ? tr("Single player works offline.") : error);
        }
    }
    signIn_->setVisible(state == QLatin1String("signed_out"));
    create_->setVisible(state == QLatin1String("signed_out"));
    signOut_->setVisible(in);
    signOut_->setEnabled(!controller_->inMatch());
    connect_->setVisible(state == QLatin1String("offline"));

    // the game
    GameProcess* g = controller_->game();
    const bool running = controller_->playerGameRunning();
    play_->setText(running ? tr("SHOW THE GAME") : tr("PLAY"));
    play_->setAccessibleName(running ? tr("Show the game: bring its window to the front") : tr("Play: start the game"));
    QString gameText;
    if (!running) {
        gameText = tr("Your game is not running. Press PLAY to start it.");
    } else {
        const QString st = g->state();
        if (st == QLatin1String("starting") || st == QLatin1String("booting"))
            gameText = tr("Your game is starting.");
        else if (controller_->inMatch())
            gameText = tr("Your game is running: you are in an online match.");
        else
            gameText = tr("Your game is running. Everything online is in its menus.");
    }
    gameLine_->setText(gameText);

    // the connection
    const QJsonObject srv = ui.value(QStringLiteral("server")).toObject();
    QString serverText;
    if (in)
        serverText = tr("Connected to the server as %1: %2 players online, %3 in matches.")
                         .arg(server_->accountName())
                         .arg(srv.value(QStringLiteral("online")).toInt())
                         .arg(srv.value(QStringLiteral("in_matches")).toInt());
    else if (state == QLatin1String("signed_out"))
        serverText = tr("Connected to the server, not signed in.");
    else if (state == QLatin1String("connecting"))
        serverText = tr("Connecting to the server...");
    else
        serverText = tr("Not connected to the server.");
    serverLine_->setText(serverText);

    // what is going on online: the search, the match, the lobby
    QString online;
    const QJsonObject queue = ui.value(QStringLiteral("queue")).toObject();
    const QJsonObject series = ui.value(QStringLiteral("series")).toObject();
    const QJsonObject lobby = ui.value(QStringLiteral("lobby")).toObject();
    if (queue.value(QStringLiteral("state")).toString() != QLatin1String("idle"))
        online = queue.value(QStringLiteral("text")).toString();
    else if (controller_->phase() != MatchController::Phase::Idle)
        online = controller_->phaseText();
    else if (!series.value(QStringLiteral("text")).toString().isEmpty())
        online = series.value(QStringLiteral("text")).toString();
    else if (!lobby.isEmpty())
        online = tr("In lobby %1 (%2). %3").arg(lobby.value(QStringLiteral("name")).toString(),
                                                lobby.value(QStringLiteral("code")).toString(),
                                                lobby.value(QStringLiteral("text")).toString());
    onlineLine_->setText(online);
    onlineLine_->setVisible(!online.isEmpty());
}
