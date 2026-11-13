#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

// How the own presence ("Available", "Busy", "DoNotDisturb", "BeRightBack", "Away", "Offline",
// "InACall"...) is shown: theme icon name, colour, text, and the tray icon with the dot
// (see tests/tst_presence.cpp).
namespace PresenceInfo {

// Theme icon for the status submenu: user-online, user-away, user-offline or user-busy.
QString presenceIcon(const QString &availability);
// The colour of the dot; an invalid colour for an empty (unknown) presence.
QColor presenceColor(const QString &availability);
// The words for the tooltip and the menu (translated); an unknown value is shown as it is.
QString presenceText(const QString &availability);
// `base` with a large dot of the presence colour in the bottom-right corner, at every size the
// tray may ask for; `base` itself when the presence is unknown. Do Not Disturb has the bar.
QIcon iconWithPresence(const QIcon &base, const QString &availability);

}
