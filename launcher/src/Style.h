#pragma once
// The launcher's look: navy and black, gold and cool blue (Soulcalibur's colours, none of its art), the QSS
// sheet, and SC5 Online's own emblem and icon (launcher/tools/make_art.py).

#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QString>

namespace style {
QPalette palette();
QString sheet();
QPixmap logo(int height);  // the SC5 Online emblem (launcher/data/logo), height pixels tall before display scaling
// a player's connection as an icon: "wifi" the Wi-Fi fan (a dot under three arcs, orange: wireless drops and delays
// packets now and then), "ethernet" (wired) four rising bars in green; a null pixmap for anything else
QPixmap linkIcon(const QString& link, int size);
QString linkName(const QString& link);  // its tooltip: "Wi-Fi", "Wired (Ethernet)", ...
QIcon appIcon();
// a ranked title's badge (launcher/data/ranks, as the server names the title: "Soul Warrior"), height pixels tall before
// display scaling, as wide as the badges' box (2.4 times its height; each badge centred in it, so they line up); a null
// pixmap for a title without one
QPixmap rankBadge(const QString& title, int height);
// the title as the badges' files and the game name it: lower case, dashes for spaces ("soul-warrior"); "" for none
QString rankKey(const QString& title);
}  // namespace style
