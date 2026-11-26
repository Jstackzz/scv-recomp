#include "Widgets.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QVBoxLayout>

namespace ui {

QFrame* card(const QString& title, QVBoxLayout** body, QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("Card"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);
    if (!title.isEmpty()) {
        auto* t = label(title.toUpper(), "CardTitle");
        letterSpacing(t, 2);
        layout->addWidget(t);
    }
    if (body) *body = layout;
    return frame;
}

QLabel* label(const QString& text, const char* objectName, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    if (objectName) l->setObjectName(QLatin1String(objectName));
    return l;
}

QWidget* pageHeader(const QString& title, const QString& subtitle, QLabel** subtitleOut) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 4);
    v->setSpacing(2);
    auto* t = label(title.toUpper(), "PageTitle");
    letterSpacing(t, 4);
    auto* s = label(subtitle, "PageSubtitle");
    s->setWordWrap(true);
    v->addWidget(t);
    v->addWidget(s);
    if (subtitleOut) *subtitleOut = s;
    return w;
}

void letterSpacing(QWidget* w, qreal pixels) {
    QFont f = w->font();
    f.setLetterSpacing(QFont::AbsoluteSpacing, pixels);
    w->setFont(f);
}

void fillRegions(QComboBox* box, const QString& select) {
    box->clear();
    for (const auto& [id, name] : GameData::regions()) box->addItem(name, id);
    const int i = box->findData(select);
    box->setCurrentIndex(i >= 0 ? i : 0);
}

void fillMaxPing(QComboBox* box, int select) {
    box->clear();
    for (int ms : {60, 100, 150, 200}) box->addItem(QStringLiteral("%1 ms").arg(ms), ms);
    box->addItem(QObject::tr("Any"), 0);
    const int i = box->findData(select);
    box->setCurrentIndex(i >= 0 ? i : 2);
}

QWidget* statTile(const QString& caption, QLabel** valueOut) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* value = label(QStringLiteral("-"), "StatValue");
    auto* cap = label(caption.toUpper(), "StatLabel");
    letterSpacing(cap, 2);
    v->addWidget(value);
    v->addWidget(cap);
    if (valueOut) *valueOut = value;
    return w;
}

}  // namespace ui

LogView::LogView(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(500);
    setMinimumHeight(90);
    QFont f(QStringLiteral("Consolas"));
    f.setPointSize(11);  // the log is read too: large enough
    setFont(f);
}

void LogView::line(const QString& text) {
    appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), text));
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
