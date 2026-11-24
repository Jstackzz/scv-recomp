#include "Style.h"

#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QPainter>

namespace style {

QPalette palette() {
    QPalette p;
    const QColor bg(0x07, 0x0a, 0x12), base(0x0a, 0x0f, 0x1a), text(0xe8, 0xe6, 0xdf), mid(0x1c, 0x26, 0x3a);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0x0e, 0x15, 0x24));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x12, 0x1a, 0x2b));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, QColor(0x1f, 0x4a, 0x8a));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x10, 0x18, 0x29));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x6b, 0x72, 0x82));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5f, 0x66, 0x74));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5f, 0x66, 0x74));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x5f, 0x66, 0x74));
    return p;
}

QString sheet() {
    // Soulcalibur's look, our own: navy and black, gold for titles and the ranks, a cool blue for what is active or
    // chosen; the focus a controller (or Tab) moves stays a brighter gold, set apart from both by its width
    return QStringLiteral(R"QSS(
* { font-family: "Segoe UI"; font-size: 10pt; color: #E8E6DF; }
QMainWindow, QWidget#Root, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget { background: #070A12; }
QDialog { background: #0B101C; }
QToolTip { background: #101829; color: #E8E6DF; border: 1px solid #B8923A; padding: 4px; }

/* header */
QWidget#Header {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #13254A, stop:0.45 #0B1426, stop:1 #070A12);
    border-bottom: 2px solid #B8923A;
}
QLabel#TitleMain { font-family: "Palatino Linotype"; font-size: 32pt; font-weight: 700; color: #E2B85A; background: transparent; }
QLabel#TitleSub  { font-family: "Palatino Linotype"; font-size: 32pt; font-weight: 400; color: #EDE6D6; background: transparent; }
QLabel#Tagline   { color: #8FA0BC; font-size: 9pt; background: transparent; }
QLabel#GameName  { font-family: "Bahnschrift"; font-size: 10pt; font-weight: 600; color: #C9B27A; background: transparent; }
QLabel#Emblem    { background: transparent; }

QPushButton#AccountButton {
    background: #101829; border: 1px solid #36466A; border-radius: 15px; padding: 6px 16px; font-weight: 600;
}
QPushButton#AccountButton:hover { border-color: #5E95E8; }

/* navigation */
QWidget#NavBar { background: #0A0F1B; border-bottom: 1px solid #1C263A; }
QPushButton#NavButton {
    font-family: "Bahnschrift"; font-size: 11pt; font-weight: 600;
    background: transparent; color: #9AA5B8; border: none; border-bottom: 3px solid transparent;
    padding: 11px 10px 8px 10px;
}
QPushButton#NavButton:hover { color: #FFFFFF; }
QPushButton#NavButton:checked { color: #FFFFFF; border-bottom: 3px solid #4C8DFF; }

/* page furniture */
QLabel#PageTitle { font-family: "Palatino Linotype"; font-size: 22pt; font-weight: 700; color: #EDE6D6; }
QLabel#PageSubtitle { color: #8F99AA; font-size: 10pt; }
QFrame#Card { background: #0E1524; border: 1px solid #1E2A40; border-radius: 6px; }
QLabel#CardTitle { font-family: "Bahnschrift"; font-size: 11pt; font-weight: 700; color: #D9B45A; background: transparent; }
QLabel#Muted { color: #8A93A3; background: transparent; }
QLabel#Big { font-family: "Bahnschrift"; font-size: 16pt; font-weight: 700; color: #FFFFFF; background: transparent; }
QLabel#StatValue { font-family: "Bahnschrift"; font-size: 18pt; font-weight: 700; color: #FFFFFF; background: transparent; }
QLabel#StatLabel { color: #8A93A3; font-size: 8pt; background: transparent; }
QLabel#Warning { color: #F0B35A; background: transparent; }
/* what a player must see and act on (a lobby whose players cannot play each other, a join turned away): large white
   on dark orange, framed in gold */
QLabel#Alert { font-family: "Bahnschrift"; font-size: 13pt; font-weight: 700; color: #FFFFFF; background: #6B2A0C;
               border: 2px solid #F0B35A; border-radius: 4px; padding: 8px 12px; }
QFrame#Card QLabel#Alert { background: #6B2A0C; }
QLabel#Rank { font-family: "Bahnschrift"; font-size: 11pt; font-weight: 700; color: #E2B85A; background: transparent; }
QFrame#Card QLabel, QFrame#Card QCheckBox { background: transparent; }
QFrame#Card QStackedWidget, QFrame#Card QStackedWidget > QWidget { background: transparent; }

/* controls */
QPushButton {
    background: #121A2B; border: 1px solid #2A3650; border-radius: 4px; padding: 7px 14px;
}
QPushButton:hover { border-color: #4C8DFF; background: #172238; }
QPushButton:pressed { background: #0D2347; }
QPushButton:disabled { color: #566070; border-color: #1B2334; background: #0C111C; }
QPushButton#Primary {
    font-family: "Bahnschrift"; font-size: 15pt; font-weight: 700; color: #FFFFFF;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2E63B8, stop:1 #142F63);
    border: 1px solid #6C9EEA; border-radius: 4px; padding: 12px 30px;
}
QPushButton#Primary:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3873CF, stop:1 #193A78); }
QPushButton#Primary:disabled { color: #6F7C94; background: #111A2C; border-color: #232F47; }
QPushButton#Primary[searching="true"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2A3448, stop:1 #151C2A); border-color: #5D6982;
}
QPushButton#Primary[compact="true"] { font-size: 11pt; padding: 6px 14px; }  /* beside plain buttons in a row */
QComboBox, QLineEdit, QSpinBox, QPlainTextEdit, QTextBrowser {
    background: #0A0F1A; border: 1px solid #2A3650; border-radius: 4px; padding: 5px 8px;
    selection-background-color: #1F4A8A; selection-color: #FFFFFF;
}
QComboBox:hover, QLineEdit:focus, QSpinBox:focus { border-color: #4C8DFF; }
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox::up-button, QSpinBox::down-button { width: 22px; border: none; background: #121A2B; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #1F4A8A; }
QComboBox QAbstractItemView { background: #0E1524; border: 1px solid #4C8DFF; selection-background-color: #1F4A8A; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #44516C; background: #0A0F1A; border-radius: 3px; }
QCheckBox::indicator:checked { background: #2F6BD0; border-color: #7FAAF0; }
QProgressBar { background: #0A0F1A; border: 1px solid #2A3650; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #173A78, stop:1 #4C8DFF); border-radius: 3px; }
QSlider::groove:horizontal { height: 6px; background: #1A2438; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #2F6BD0; border-radius: 3px; }
QSlider::handle:horizontal { width: 16px; margin: -6px 0; border-radius: 8px; background: #D9B45A; border: 1px solid #F2D38A; }

QListWidget#CharacterGrid { background: transparent; border: none; outline: none; }
QListWidget#CharacterGrid::item {
    background: #111A2B; border: 1px solid #243049; border-radius: 4px; color: #DAD6CC; padding: 4px;
}
QListWidget#CharacterGrid::item:hover { border-color: #4C8DFF; color: #FFFFFF; }
QListWidget#CharacterGrid::item:selected {
    color: #FFFFFF; border: 1px solid #7FAAF0;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #24539E, stop:1 #102850);
}
QListWidget { background: #0A0F1A; border: 1px solid #1E2A40; border-radius: 4px; }
QTableWidget { background: #0A0F1A; alternate-background-color: #0E1524; gridline-color: #18223A;
               border: 1px solid #1E2A40; border-radius: 4px; selection-background-color: #1F4A8A; }
QHeaderView::section { background: #101829; color: #C9C3B5; border: none; border-bottom: 2px solid #B8923A;
                       padding: 6px 8px; font-weight: 600; }
QTableCornerButton::section { background: #101829; border: none; }
QTabWidget::pane { border: 1px solid #1E2A40; border-radius: 4px; top: -1px; }
QTabBar::tab { background: #0E1524; border: 1px solid #1E2A40; padding: 7px 16px; color: #9AA5B8; }
QTabBar::tab:selected { color: #FFFFFF; border-bottom: 2px solid #4C8DFF; }
QGroupBox { border: 1px solid #1E2A40; border-radius: 6px; margin-top: 18px; padding: 12px 10px 10px 10px; background: #0E1524; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #D9B45A;
                   font-family: "Bahnschrift"; font-weight: 700; }
QScrollBar:vertical { background: #080C15; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #26324A; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #2F6BD0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar:horizontal { background: #080C15; height: 10px; }
QScrollBar::handle:horizontal { background: #26324A; border-radius: 4px; min-width: 30px; }

/* footer */
QWidget#Footer { background: #05080E; border-top: 1px solid #1C263A; }
QWidget#Footer QLabel { color: #8A93A3; font-size: 9pt; background: transparent; }
QLabel#Dot { font-size: 12pt; }

/* match found */
QLabel#MatchFound { font-family: "Palatino Linotype"; font-size: 28pt; font-weight: 700; color: #E2B85A; }

/* the focus a controller (or Tab) moves: gold, over hover and checked, two pixels wide so it stands apart from the
   gold titles and the blue of what is chosen */
QPushButton:focus, QComboBox:focus, QListWidget:focus, QTableWidget:focus, QSpinBox:focus, QLineEdit:focus {
    border: 2px solid #F2C14E;
}
QPushButton#Primary:focus { border: 2px solid #FFE7A3; color: #FFF4D6; }
QPushButton#NavButton:focus { color: #F2C14E; border: none; border-bottom: 3px solid #F2C14E; }
QPushButton#AccountButton:focus { border: 2px solid #F2C14E; }
QCheckBox:focus { color: #F2C14E; }
QCheckBox::indicator:focus { border: 2px solid #F2C14E; }
QSlider:focus::handle:horizontal { border: 2px solid #FFE7A3; background: #F2C14E; }
QListWidget#CharacterGrid:focus::item:selected { border: 2px solid #F2C14E; }
)QSS");
}

QPixmap linkIcon(const QString& link, int size) {
    const bool wifi = link == QLatin1String("wifi");
    if (!wifi && link != QLatin1String("ethernet")) return {};
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    if (wifi) {
        const QColor c(0xF0, 0xB3, 0x5A);
        const QPointF o(s / 2, s * 0.84);  // the fan's point
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(o, s * 0.085, s * 0.085);
        p.setPen(QPen(c, s * 0.1, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        for (qreal r : {s * 0.3, s * 0.52, s * 0.74})
            p.drawArc(QRectF(o.x() - r, o.y() - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
    } else {
        const QColor c(0x7C, 0xC6, 0x7A);
        const qreal w = s * 0.16, gap = s * 0.08, left = (s - 4 * w - 3 * gap) / 2, bottom = s * 0.92;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int i = 0; i < 4; i++) {
            const qreal h = s * (0.3 + 0.2 * i);
            p.drawRoundedRect(QRectF(left + i * (w + gap), bottom - h, w, h), w * 0.25, w * 0.25);
        }
    }
    return pm;
}

QString linkName(const QString& link) {
    if (link == QLatin1String("wifi")) return QObject::tr("Wi-Fi");
    if (link == QLatin1String("ethernet")) return QObject::tr("Wired (Ethernet)");
    if (link == QLatin1String("other")) return QObject::tr("Other connection");
    return QObject::tr("Connection type unknown");
}

QPixmap logo(int height) {
    // drawn at the screen's scale (150 % makes a 76 px logo 114 pixels), from the 320 px tall resource
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    const QImage img(QStringLiteral(":/logo/sc5-logo.png"));
    QPixmap pm = QPixmap::fromImage(img.scaledToHeight(qRound(height * dpr), Qt::SmoothTransformation));
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/logo/icon-%1.png").arg(size), QSize(size, size));
    return icon;
}

QString rankKey(const QString& title) {
    QString key;
    for (const QChar c : title.trimmed().toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            key += c;
        else if ((c == QLatin1Char(' ') || c == QLatin1Char('-')) && !key.isEmpty() && !key.endsWith(QLatin1Char('-')))
            key += QLatin1Char('-');
    }
    while (key.endsWith(QLatin1Char('-'))) key.chop(1);
    return key;
}

QPixmap rankBadge(const QString& title, int height) {
    const QString key = rankKey(title);
    if (key.isEmpty() || height <= 0) return {};
    // drawn at the screen's scale, scaled once per title and size (lists ask for every row)
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    static QHash<QString, QPixmap> made;
    const QString id = QStringLiteral("%1/%2/%3").arg(key).arg(height).arg(dpr);
    if (auto it = made.constFind(id); it != made.constEnd()) return *it;
    const QImage img(QStringLiteral(":/ranks/%1.png").arg(key));
    QPixmap pm;
    if (!img.isNull()) {
        pm = QPixmap::fromImage(img.scaledToHeight(qRound(height * dpr), Qt::SmoothTransformation));
        pm.setDevicePixelRatio(dpr);
    }
    made.insert(id, pm);
    return pm;
}

}  // namespace style
