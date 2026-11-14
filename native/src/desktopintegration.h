#pragma once

#include <QObject>
#include <QPointer>
#include <QIcon>
#include <QWindow>

class QAction;

class KStatusNotifierItem;

// Plasma integration: tray icon (show/hide the window, quit), unread badge on the
// task manager entry, and start at login.
class DesktopIntegration : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY startAtLoginChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)

public:
    static constexpr auto kDesktopId = "io.github.andreacimino.kollega";
    // Ids the app had before; their autostart entries are moved to kDesktopId.
    static constexpr const char *kLegacyDesktopIds[] = {"org.ac.teams.native"};

    explicit DesktopIntegration(QObject *parent = nullptr);

    void setWindow(QWindow *window);
    // Chats with unread messages: tray state/tooltip and task manager badge.
    void setUnreadCount(int count);
    // Own presence ("Available", "Busy", "DoNotDisturb", "BeRightBack", "Away", "Offline",
    // "InACall"...): a dot on the tray icon, the tooltip, and the checked entry of the
    // tray's status submenu.
    void setPresence(const QString &availability);

    bool startAtLogin() const;
    void setStartAtLogin(bool enabled);
    bool trayAvailable() const { return m_tray != nullptr; }

signals:
    void startAtLoginChanged();
    // The user picked a status in the tray menu.
    void availabilityRequested(const QString &availability);

private:
    QString autostartPath() const;

    KStatusNotifierItem *m_tray = nullptr;
    QPointer<QWindow> m_window;
    int m_unread = -1;
    QString m_presence;
    QList<QAction *> m_statusActions; // data(): the availability each one sets
    void updateTooltip();
};
