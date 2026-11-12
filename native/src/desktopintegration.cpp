#include "desktopintegration.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QPen>
#include <QStandardPaths>
#include <KStatusNotifierItem>
#include <QAction>
#include <QActionGroup>
#include <QMenu>

DesktopIntegration::DesktopIntegration(QObject *parent) : QObject(parent)
{
    // Notification events (popup + sound per event, configurable in System Settings): the
    // .notifyrc must be where KNotifications looks; keep the user's copy up to date.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/knotifications6");
    QDir().mkpath(dir);
    QFile bundled(QStringLiteral(":/KTeams/kteams.notifyrc"));
    QFile installed(dir + QStringLiteral("/kteams.notifyrc"));
    if (bundled.open(QIODevice::ReadOnly)) {
        const QByteArray wanted = bundled.readAll();
        QByteArray current;
        if (installed.open(QIODevice::ReadOnly)) { current = installed.readAll(); installed.close(); }
        if (current != wanted && installed.open(QIODevice::WriteOnly | QIODevice::Truncate)) { installed.write(wanted); installed.close(); }
    }

    // Leftovers of the app's previous name (ac-teams-native): its autostart entry would
    // point at a binary that no longer exists.
    QFile::remove(dir + QStringLiteral("/ac-teams-native.notifyrc"));
    const QString oldAutostart = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/org.ac.teams.native.desktop");
    if (QFile::exists(oldAutostart)) { QFile::remove(oldAutostart); setStartAtLogin(true); }

    m_tray = new KStatusNotifierItem(QString::fromLatin1(kDesktopId), this);
    m_tray->setCategory(KStatusNotifierItem::Communications);
    m_tray->setTitle(QStringLiteral("KTeams"));
    m_tray->setIconByPixmap(QIcon(QStringLiteral(":/KTeams/icon.svg")));
    m_tray->setToolTip(QIcon(QStringLiteral(":/KTeams/icon.svg")), QStringLiteral("KTeams"), QString());
    m_tray->setStatus(KStatusNotifierItem::Active);
    // The standard menu brings "Quit"; clicking the icon shows/hides the associated window.
    m_tray->setStandardActionsEnabled(true);
    // Status submenu, as the official client's tray has.
    QMenu *status = new QMenu(tr("Status"), m_tray->contextMenu());
    status->setIcon(QIcon::fromTheme(QStringLiteral("user-online")));
    auto *group = new QActionGroup(status);
    const QList<QPair<QString, QString>> choices{{QStringLiteral("Available"), tr("Available")}, {QStringLiteral("Busy"), tr("Busy")},
                                                 {QStringLiteral("DoNotDisturb"), tr("Do not disturb")}, {QStringLiteral("BeRightBack"), tr("Be right back")},
                                                 {QStringLiteral("Away"), tr("Away")}, {QStringLiteral("Offline"), tr("Appear offline")}};
    for (const auto &[value, label] : choices) {
        QAction *a = status->addAction(QIcon::fromTheme(presenceIcon(value)), label);
        a->setCheckable(true);
        a->setData(value);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, value] { emit availabilityRequested(value); });
        m_statusActions << a;
    }
    m_tray->contextMenu()->insertMenu(m_tray->contextMenu()->actions().value(0), status);
    m_tray->contextMenu()->insertSeparator(m_tray->contextMenu()->actions().value(1));
}

QString DesktopIntegration::presenceIcon(const QString &availability)
{
    if (availability == QLatin1String("Available")) return QStringLiteral("user-online");
    if (availability == QLatin1String("Away") || availability == QLatin1String("BeRightBack")) return QStringLiteral("user-away");
    if (availability == QLatin1String("Offline") || availability.isEmpty()) return QStringLiteral("user-offline");
    return QStringLiteral("user-busy"); // Busy, DoNotDisturb, InACall, InAMeeting, Presenting
}

QString DesktopIntegration::presenceText(const QString &availability)
{
    static const QHash<QString, QString> names{{QStringLiteral("Available"), tr("Available")}, {QStringLiteral("Busy"), tr("Busy")},
                                               {QStringLiteral("DoNotDisturb"), tr("Do not disturb")}, {QStringLiteral("BeRightBack"), tr("Be right back")},
                                               {QStringLiteral("Away"), tr("Away")}, {QStringLiteral("Offline"), tr("Offline")},
                                               {QStringLiteral("InACall"), tr("In a call")}, {QStringLiteral("InAMeeting"), tr("In a meeting")},
                                               {QStringLiteral("Presenting"), tr("Presenting")}};
    return names.value(availability, availability);
}

QIcon DesktopIntegration::iconWithPresence(const QString &availability)
{
    // The tray's overlay icons are too small to read: the dot is painted into the icon
    // itself, large, at every size the tray may ask for.
    QColor color;
    if (availability == QLatin1String("Available")) color = QColor(0x2e, 0xb8, 0x5c);
    else if (availability == QLatin1String("Away") || availability == QLatin1String("BeRightBack")) color = QColor(0xf5, 0xb8, 0x00);
    else if (availability == QLatin1String("Offline")) color = QColor(0x9a, 0x9a, 0x9a);
    else if (!availability.isEmpty()) color = QColor(0xd9, 0x2d, 0x2d); // busy, do not disturb, in a call...
    const QIcon base(QStringLiteral(":/KTeams/icon.svg"));
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

void DesktopIntegration::setPresence(const QString &availability)
{
    if (availability == m_presence) return;
    m_presence = availability;
    const QIcon icon = iconWithPresence(availability);
    m_tray->setIconByPixmap(icon);
    m_tray->setAttentionIconByPixmap(icon);
    m_tray->setToolTipIconByPixmap(icon);
    for (QAction *a : std::as_const(m_statusActions)) a->setChecked(a->data().toString() == availability);
    updateTooltip();
}

void DesktopIntegration::updateTooltip()
{
    QStringList parts;
    if (!m_presence.isEmpty()) parts << presenceText(m_presence);
    if (m_unread > 0) parts << (m_unread == 1 ? tr("1 unread chat") : tr("%1 unread chats").arg(m_unread));
    m_tray->setToolTipSubTitle(parts.join(QStringLiteral(" · ")));
}

void DesktopIntegration::setWindow(QWindow *window)
{
    m_window = window;
    m_tray->setAssociatedWindow(window);
}

void DesktopIntegration::setUnreadCount(int count)
{
    if (count == m_unread) return;
    m_unread = count;
    // Unread: the "needs attention" state (highlighted icon); the overlay dot is the presence.
    m_tray->setStatus(count > 0 ? KStatusNotifierItem::NeedsAttention : KStatusNotifierItem::Active);
    updateTooltip();

    // Task manager badge: the Unity LauncherEntry signal, which Plasma's task manager reads.
    // It matches the installed desktop file by id.
    QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/kteams/kteams"),
                                                     QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
    signal << QStringLiteral("application://%1.desktop").arg(QString::fromLatin1(kDesktopId))
           << QVariantMap{{QStringLiteral("count"), qint64(count)}, {QStringLiteral("count-visible"), count > 0}};
    QDBusConnection::sessionBus().send(signal);
}

QString DesktopIntegration::autostartPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/")
        + QString::fromLatin1(kDesktopId) + QStringLiteral(".desktop");
}

bool DesktopIntegration::startAtLogin() const
{
    return QFile::exists(autostartPath());
}

void DesktopIntegration::setStartAtLogin(bool enabled)
{
    if (enabled == startAtLogin()) return;
    if (enabled) {
        QDir().mkpath(QFileInfo(autostartPath()).path());
        QFile f(autostartPath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            // Absolute path: works also when the app is run from its build folder.
            f.write(QStringLiteral("[Desktop Entry]\nName=KTeams\nExec=\"%1\"\nIcon=%2\nType=Application\n"
                                   "X-GNOME-Autostart-enabled=true\n")
                        .arg(QCoreApplication::applicationFilePath(), QString::fromLatin1(kDesktopId))
                        .toUtf8());
        }
    } else {
        QFile::remove(autostartPath());
    }
    emit startAtLoginChanged();
}
