#include "desktopintegration.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QStandardPaths>
#include <KStatusNotifierItem>

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
    m_tray->setStatus(count > 0 ? KStatusNotifierItem::NeedsAttention : KStatusNotifierItem::Active);
    m_tray->setAttentionIconByPixmap(QIcon(QStringLiteral(":/KTeams/icon.svg")));
    m_tray->setOverlayIconByName(count > 0 ? QStringLiteral("mail-unread-new") : QString());
    m_tray->setToolTipSubTitle(count > 0 ? (count == 1 ? tr("1 unread chat") : tr("%1 unread chats").arg(count))
                                         : QString());

    // Task manager badge: the Unity LauncherEntry signal, which Plasma's task manager reads.
    // It matches the installed desktop file by id.
    QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/ac/teams/native"),
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
