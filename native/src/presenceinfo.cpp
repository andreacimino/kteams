#include "presenceinfo.h"

#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QPen>
#include <QPixmap>

namespace PresenceInfo {

QString presenceIcon(const QString &availability)
{
    if (availability == QLatin1String("Available")) return QStringLiteral("user-online");
    if (availability == QLatin1String("Away") || availability == QLatin1String("BeRightBack")) return QStringLiteral("user-away");
    if (availability == QLatin1String("Offline") || availability.isEmpty()) return QStringLiteral("user-offline");
    return QStringLiteral("user-busy"); // Busy, DoNotDisturb, InACall, InAMeeting, Presenting
}

QColor presenceColor(const QString &availability)
{
    if (availability == QLatin1String("Available")) return QColor(0x2e, 0xb8, 0x5c);
    if (availability == QLatin1String("Away") || availability == QLatin1String("BeRightBack")) return QColor(0xf5, 0xb8, 0x00);
    if (availability == QLatin1String("Offline")) return QColor(0x9a, 0x9a, 0x9a);
    if (!availability.isEmpty()) return QColor(0xd9, 0x2d, 0x2d); // busy, do not disturb, in a call...
    return {};
}

QString presenceText(const QString &availability)
{
    static const QHash<QString, QString> names{{QStringLiteral("Available"), QCoreApplication::translate("DesktopIntegration", "Available")}, {QStringLiteral("Busy"), QCoreApplication::translate("DesktopIntegration", "Busy")},
                                               {QStringLiteral("DoNotDisturb"), QCoreApplication::translate("DesktopIntegration", "Do not disturb")}, {QStringLiteral("BeRightBack"), QCoreApplication::translate("DesktopIntegration", "Be right back")},
                                               {QStringLiteral("Away"), QCoreApplication::translate("DesktopIntegration", "Away")}, {QStringLiteral("Offline"), QCoreApplication::translate("DesktopIntegration", "Offline")},
                                               {QStringLiteral("InACall"), QCoreApplication::translate("DesktopIntegration", "In a call")}, {QStringLiteral("InAMeeting"), QCoreApplication::translate("DesktopIntegration", "In a meeting")},
                                               {QStringLiteral("Presenting"), QCoreApplication::translate("DesktopIntegration", "Presenting")}};
    return names.value(availability, availability);
}

QIcon iconWithPresence(const QIcon &base, const QString &availability)
{
    // The tray's overlay icons are too small to read: the dot is painted into the icon
    // itself, large, at every size the tray may ask for.
    const QColor color = presenceColor(availability);
    if (!color.isValid()) return base;
    QIcon icon;
    for (int size : {22, 24, 32, 48, 64, 128}) {
        QPixmap pm = base.pixmap(size, size);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal d = size * 0.30;
        const QRectF dot(size - d, size - d, d, d);
        p.setPen(QPen(QColor(255, 255, 255), qMax(1.0, size / 16.0)));
        p.setBrush(color);
        p.drawEllipse(dot);
        if (availability == QLatin1String("DoNotDisturb")) { // the bar of the official icon
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawRect(QRectF(dot.left() + d * 0.22, dot.center().y() - d * 0.09, d * 0.56, d * 0.18));
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

}
