#include "screencast.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QLoggingCategory>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcScreenCast, "kteams.screencast", QtInfoMsg)

namespace {
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequestInterface = "org.freedesktop.portal.Request";
constexpr auto kSessionInterface = "org.freedesktop.portal.Session";
// SelectSources: types (bits: 1 monitor, 2 window, 4 virtual), cursor_mode 2 = embedded in the frames.
constexpr uint kSourceTypes = 3;
constexpr uint kCursorEmbedded = 2;

// Start's "streams": a(ua{sv}).
struct PortalStream {
    uint node = 0;
    QVariantMap properties;
};
QDBusArgument &operator<<(QDBusArgument &a, const PortalStream &s)
{
    a.beginStructure();
    a << s.node << s.properties;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PortalStream &s)
{
    a.beginStructure();
    a >> s.node >> s.properties;
    a.endStructure();
    return a;
}
}
Q_DECLARE_METATYPE(PortalStream)

ScreenCastPortal::ScreenCastPortal(QObject *parent) : QObject(parent)
{
    static const bool registered = [] { qDBusRegisterMetaType<PortalStream>(); qDBusRegisterMetaType<QList<PortalStream>>(); return true; }();
    Q_UNUSED(registered);
}

ScreenCastPortal::~ScreenCastPortal() { stop(); }

void ScreenCastPortal::request(const QString &method, QVariantList args, QVariantMap options, std::function<void(const QVariantMap &)> onSuccess)
{
    // The portal answers on /org/freedesktop/portal/desktop/request/<sender>/<token>:
    // subscribe before calling, so the Response cannot be missed.
    const QString token = QStringLiteral("kteams%1").arg(++m_counter);
    QString sender = QDBusConnection::sessionBus().baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    if (!m_requestPath.isEmpty())
        QDBusConnection::sessionBus().disconnect(QString::fromLatin1(kService), m_requestPath, QString::fromLatin1(kRequestInterface),
                                                 QStringLiteral("Response"), this, SLOT(handleResponse(uint,QVariantMap)));
    m_requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    m_onSuccess = std::move(onSuccess);
    QDBusConnection::sessionBus().connect(QString::fromLatin1(kService), m_requestPath, QString::fromLatin1(kRequestInterface),
                                          QStringLiteral("Response"), this, SLOT(handleResponse(uint,QVariantMap)));
    options.insert(QStringLiteral("handle_token"), token);
    args << options;
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kInterface), method);
    call.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, method] {
        watcher->deleteLater();
        if (watcher->isError()) {
            qCWarning(lcScreenCast) << method << "failed:" << watcher->error().message();
            emit failed(tr("Screen portal: %1").arg(watcher->error().message()));
            m_session.clear();
        }
    });
}

void ScreenCastPortal::handleResponse(uint code, const QVariantMap &results)
{
    // 0 success, 1 the user cancelled, 2 other error.
    if (code != 0) {
        qCInfo(lcScreenCast) << "request ended with code" << code;
        m_session.clear();
        emit failed(code == 1 ? tr("Sharing cancelled") : tr("Screen portal: error"));
        return;
    }
    if (m_onSuccess) m_onSuccess(results);
}

void ScreenCastPortal::start()
{
    if (active()) return;
    const QString sessionToken = QStringLiteral("kteamssession%1").arg(++m_counter);
    request(QStringLiteral("CreateSession"), {}, {{QStringLiteral("session_handle_token"), sessionToken}}, [this](const QVariantMap &results) {
        m_session = results.value(QStringLiteral("session_handle")).toString();
        if (m_session.isEmpty()) { emit failed(tr("Screen portal: no session")); return; }
        QDBusConnection::sessionBus().connect(QString::fromLatin1(kService), m_session, QString::fromLatin1(kSessionInterface),
                                              QStringLiteral("Closed"), this, SLOT(handleClosed()));
        request(QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(m_session))},
                {{QStringLiteral("types"), kSourceTypes}, {QStringLiteral("multiple"), false}, {QStringLiteral("cursor_mode"), kCursorEmbedded}},
                [this](const QVariantMap &) {
            request(QStringLiteral("Start"), {QVariant::fromValue(QDBusObjectPath(m_session)), QString()}, {}, [this](const QVariantMap &results) {
                const auto streams = qdbus_cast<QList<PortalStream>>(results.value(QStringLiteral("streams")));
                if (streams.isEmpty()) { emit failed(tr("Screen portal: no stream")); stop(); return; }
                QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kInterface),
                                                                   QStringLiteral("OpenPipeWireRemote"));
                call.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap()});
                const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(call);
                if (!reply.isValid()) { emit failed(tr("Screen portal: %1").arg(reply.error().message())); stop(); return; }
                const int fd = dup(reply.value().fileDescriptor());
                qCInfo(lcScreenCast) << "sharing node" << streams.first().node << streams.first().properties.value(QStringLiteral("size"));
                emit ready(fd, streams.first().node);
            });
        });
    });
}

void ScreenCastPortal::stop()
{
    if (m_session.isEmpty()) return;
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kService), m_session, QString::fromLatin1(kSessionInterface), QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(call);
    QDBusConnection::sessionBus().disconnect(QString::fromLatin1(kService), m_session, QString::fromLatin1(kSessionInterface),
                                             QStringLiteral("Closed"), this, SLOT(handleClosed()));
    m_session.clear();
}

void ScreenCastPortal::handleClosed()
{
    qCInfo(lcScreenCast) << "session closed by the portal";
    m_session.clear();
    emit closed();
}
