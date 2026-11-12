#pragma once

#include <QObject>
#include <QVariantMap>
#include <functional>

// Screen capture through the desktop portal (org.freedesktop.portal.ScreenCast, served by
// xdg-desktop-portal-kde on Plasma): the system asks the user which screen or window to
// share, and hands back a PipeWire node that pipewiresrc reads. No libportal: a few D-Bus
// calls, each answered by a Response signal on a request object.
class ScreenCastPortal : public QObject
{
    Q_OBJECT
public:
    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    // Opens the system picker; ready() follows with the PipeWire connection, or failed().
    void start();
    // Ends the session (the picker, if still open, and the capture).
    void stop();
    bool active() const { return !m_session.isEmpty(); }

signals:
    // fd: our PipeWire remote (owned by the receiver), nodeId: the stream's node.
    void ready(int fd, unsigned nodeId);
    void failed(const QString &message);
    // The session ended from the outside (the user stopped sharing in the system UI).
    void closed();

private slots:
    void handleResponse(uint code, const QVariantMap &results);
    void handleClosed();

private:
    // A portal method whose outcome arrives as a Response signal on a request path.
    void request(const QString &method, QVariantList args, QVariantMap options, std::function<void(const QVariantMap &)> onSuccess);
    QString m_session;
    QString m_requestPath;
    std::function<void(const QVariantMap &)> m_onSuccess;
    int m_counter = 0;
};
