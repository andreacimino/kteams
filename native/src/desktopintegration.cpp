#include "desktopintegration.h"
#include "presenceinfo.h"

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
    QFile bundled(QStringLiteral(":/Kollega/kollega.notifyrc"));
    QFile installed(dir + QStringLiteral("/kollega.notifyrc"));
    if (bundled.open(QIODevice::ReadOnly)) {
        const QByteArray wanted = bundled.readAll();
        QByteArray current;
        if (installed.open(QIODevice::ReadOnly)) { current = installed.readAll(); installed.close(); }
        if (current != wanted && installed.open(QIODevice::WriteOnly | QIODevice::Truncate)) { installed.write(wanted); installed.close(); }
    }

    // Leftovers of the app's previous name (ac-teams-native): its autostart entry would
    // point at a binary that no longer exists.
    QFile::remove(dir + QStringLiteral("/ac-teams-native.notifyrc"));
    // Autostart entries written under the app's earlier id (ac-teams-native): replaced by
    // one under the current id, so the setting keeps working and switching it off removes
    // the right file.
    for (const char *legacyId : kLegacyDesktopIds) {
        const QString legacy = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/")
            + QString::fromLatin1(legacyId) + QStringLiteral(".desktop");
        if (QFile::exists(legacy)) { QFile::remove(legacy); setStartAtLogin(true); }
    }

    m_tray = new KStatusNotifierItem(QString::fromLatin1(kDesktopId), this);
    m_tray->setCategory(KStatusNotifierItem::Communications);
    m_tray->setTitle(QStringLiteral("Kollega"));
    m_tray->setIconByPixmap(QIcon(QStringLiteral(":/Kollega/icon.svg")));
    m_tray->setToolTip(QIcon(QStringLiteral(":/Kollega/icon.svg")), QStringLiteral("Kollega"), QString());
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
        QAction *a = status->addAction(QIcon::fromTheme(PresenceInfo::presenceIcon(value)), label);
        a->setCheckable(true);
        a->setData(value);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, value] { emit availabilityRequested(value); });
        m_statusActions << a;
    }
    m_tray->contextMenu()->insertMenu(m_tray->contextMenu()->actions().value(0), status);
    m_tray->contextMenu()->insertSeparator(m_tray->contextMenu()->actions().value(1));
}

void DesktopIntegration::setPresence(const QString &availability)
{
    if (availability == m_presence) return;
    m_presence = availability;
    const QIcon icon = PresenceInfo::iconWithPresence(QIcon(QStringLiteral(":/Kollega/icon.svg")), availability);
    m_tray->setIconByPixmap(icon);
    m_tray->setAttentionIconByPixmap(icon);
    m_tray->setToolTipIconByPixmap(icon);
    for (QAction *a : std::as_const(m_statusActions)) a->setChecked(a->data().toString() == availability);
    updateTooltip();
}

void DesktopIntegration::updateTooltip()
{
    QStringList parts;
    if (!m_presence.isEmpty()) parts << PresenceInfo::presenceText(m_presence);
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
    QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/io/github/andreacimino/kollega"),
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
            f.write(QStringLiteral("[Desktop Entry]\nName=Kollega\nExec=\"%1\"\nIcon=%2\nType=Application\n"
                                   "X-GNOME-Autostart-enabled=true\n")
                        .arg(QCoreApplication::applicationFilePath(), QString::fromLatin1(kDesktopId))
                        .toUtf8());
        }
    } else {
        QFile::remove(autostartPath());
    }
    emit startAtLoginChanged();
}
