#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

class KStatusNotifierItem;

// Plasma integration: tray icon (show/hide the window, quit), unread badge on the
// task manager entry, and start at login.
class DesktopIntegration : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool startAtLogin READ startAtLogin WRITE setStartAtLogin NOTIFY startAtLoginChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)

public:
    static constexpr auto kDesktopId = "org.kteams.kteams";

    explicit DesktopIntegration(QObject *parent = nullptr);

    void setWindow(QWindow *window);
    // Chats with unread messages: tray state/tooltip and task manager badge.
    void setUnreadCount(int count);

    bool startAtLogin() const;
    void setStartAtLogin(bool enabled);
    bool trayAvailable() const { return m_tray != nullptr; }

signals:
    void startAtLoginChanged();

private:
    QString autostartPath() const;

    KStatusNotifierItem *m_tray = nullptr;
    QPointer<QWindow> m_window;
    int m_unread = -1;
};
