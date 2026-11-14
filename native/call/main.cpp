// kollega-capture: a window with the Teams web app, only for calls and meetings.
//
// One instance at a time: a second start hands its address to the running one and exits,
// so a call reuses an already loaded Teams. Closing the window ends the call and hides it;
// the process stays warm for a while (or always, with --keep-warm) so the next call starts
// at once, then exits to give the memory back.

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDesktopServices>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QLoggingCategory>
#include <QQuickWebEngineProfile>
#include <QWebEnginePermission>
#include <QWebEngineUrlRequestInterceptor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

namespace {
constexpr auto kServerName = "kollega-capture";
constexpr int kWarmMinutes = 15;  // stay loaded this long after a call, unless --keep-warm
// The Teams web app's current home: going there directly skips a redirect.
constexpr auto kHome = "https://teams.cloud.microsoft/";

bool isTeamsUrl(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https")) return false;
    const QString host = url.host();
    return host == QLatin1String("teams.microsoft.com") || host == QLatin1String("teams.cloud.microsoft")
        || host.endsWith(QLatin1String(".teams.microsoft.com"));
}
}

Q_LOGGING_CATEGORY(lcCall, "kollega.call", QtInfoMsg)

// This window is only for calls: what Teams web would load for chats, files, calendar,
// pictures and telemetry is never requested. Login, configuration, the app's own files,
// Trouter, the calling services and threads (group calls) still go through.
// KOLLEGA_CALL_NO_BLOCK=1 turns this off, to tell a blocking problem from another one.
class CallOnlyFilter : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString host = url.host();
        const QString path = url.path();
        const bool block =
            // telemetry
            host.endsWith(QLatin1String("events.data.microsoft.com")) || host.endsWith(QLatin1String(".aria.microsoft.com"))
            || host == QLatin1String("js.monitor.azure.com") || host.endsWith(QLatin1String("dc.services.visualstudio.com"))
            // chats, messages, activity feed, teams/chats summary
            || path.contains(QLatin1String("/users/ME/conversations")) || path.startsWith(QLatin1String("/api/csa/"))
            || host.startsWith(QLatin1String("chatsvcagg."))
            // pictures and chat images
            || path.contains(QLatin1String("/profilepicturev2")) || host.endsWith(QLatin1String("asm.skype.com"))
            // calendar, files, search, Office app launcher
            || path.contains(QLatin1String("/me/calendars")) || host.endsWith(QLatin1String(".sharepoint.com"))
            || host == QLatin1String("substrate.office.com") || host == QLatin1String("webshell.suite.office.com");
        if (block) {
            ++m_blocked;
            qCDebug(lcCall) << "blocked" << host << path.left(60);
            info.block(true);
        }
    }

    int blocked() const { return m_blocked; }

private:
    int m_blocked = 0;
};

// --capture: records the requests the Teams web app makes (also from its workers, which a
// page script cannot see), with their bodies, next to what capture.js reports from the page.
class CaptureLog : public QObject
{
    Q_OBJECT
public:
    explicit CaptureLog(QObject *parent = nullptr) : QObject(parent)
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kollega/call-captures");
        QDir().mkpath(dir);
        m_file.setFileName(dir + QStringLiteral("/capture-%1.jsonl").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            qCInfo(lcCall) << "capturing to" << m_file.fileName();
        else
            qCWarning(lcCall) << "cannot write the capture to" << m_file.fileName();
    }
    // From the page (console line "ACCAPTURE <json>").
    Q_INVOKABLE void page(const QString &json) { write(json.toUtf8()); }
    void write(const QByteArray &line)
    {
        m_file.write(line);
        m_file.write("\n");
        m_file.flush();
    }

private:
    QFile m_file;
};

class CaptureInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    CaptureInterceptor(CaptureLog *log, QObject *parent = nullptr) : QWebEngineUrlRequestInterceptor(parent), m_log(log) {}

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString host = url.host();
        static const QRegularExpression staticFile(QStringLiteral("\\.(js|css|png|jpg|svg|woff2?|ico|json|map)$"));
        const bool interesting = (host.contains(QLatin1String("skype.com")) || host.contains(QLatin1String("teams.microsoft.com"))
                                  || host.contains(QLatin1String("cloud.microsoft")) || host.contains(QLatin1String("trouter"))
                                  || host.contains(QLatin1String("flightproxy")) || host.contains(QLatin1String("office.com")))
                                 && !staticFile.match(url.path()).hasMatch();
        if (!interesting) return;
        QByteArray body;
        if (QIODevice *dev = info.requestBody(); dev && dev->isOpen()) {
            const qint64 pos = dev->pos();
            body = dev->read(20000);
            dev->seek(pos);
        }
        // Secrets never reach the file: tokens, auth codes and signatures are masked.
        static const QRegularExpression jwt(QStringLiteral("eyJ[A-Za-z0-9_.-]{20,}"));
        static const QRegularExpression jsonSecret(
            QStringLiteral("(\"[A-Za-z_]*(?:token|credential|password|secret|code)[A-Za-z_]*\"\\s*:\\s*\")[^\"]*\""),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression formSecret(
            QStringLiteral("((?:^|&)(?:code|client_info|state|sig|refresh_token|client_secret|assertion|[A-Za-z_]*token[A-Za-z_]*)=)[^&]+"),
            QRegularExpression::CaseInsensitiveOption);
        QString text = QString::fromUtf8(body);
        text.replace(jwt, QStringLiteral("<jwt>"));
        text.replace(jsonSecret, QStringLiteral("\\1<masked>\""));
        text.replace(formSecret, QStringLiteral("\\1<masked>"));
        QUrl shown = url;
        shown.setFragment(QString());
        QUrlQuery q(shown);
        for (const auto &item : q.queryItems()) {
            const QString k = item.first.toLower();
            if (k.contains(QLatin1String("token")) || k == QLatin1String("sig") || k == QLatin1String("code")
                || k == QLatin1String("client_info") || k == QLatin1String("state"))
                q.removeAllQueryItems(item.first), q.addQueryItem(item.first, QStringLiteral("masked"));
        }
        shown.setQuery(q);
        const QJsonObject line{{QStringLiteral("t"), QDateTime::currentMSecsSinceEpoch()},
                               {QStringLiteral("kind"), QStringLiteral("net")},
                               {QStringLiteral("data"), QJsonObject{{QStringLiteral("method"), QString::fromLatin1(info.requestMethod())},
                                                                    {QStringLiteral("url"), shown.toString()},
                                                                    {QStringLiteral("body"), text}}}};
        m_log->write(QJsonDocument(line).toJson(QJsonDocument::Compact));
    }

private:
    CaptureLog *m_log;
};

// Decisions the page asks for, made in C++ where the permission types are well defined.
class CallPolicy : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    // Microphone, camera and screen sharing: what a call needs. Everything else is refused.
    Q_INVOKABLE void decide(const QWebEnginePermission &permission) const
    {
        switch (permission.permissionType()) {
        case QWebEnginePermission::PermissionType::MediaAudioCapture:
        case QWebEnginePermission::PermissionType::MediaVideoCapture:
        case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
        case QWebEnginePermission::PermissionType::DesktopVideoCapture:
        case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
            if (isTeamsUrl(permission.origin())) { permission.grant(); return; }
            break;
        default:
            break;
        }
        permission.deny();
    }

    // Links to other sites open in the system browser, not in the call window.
    Q_INVOKABLE bool keepInWindow(const QUrl &url) const
    {
        if (isTeamsUrl(url) || url.host().endsWith(QLatin1String("login.microsoftonline.com"))
            || url.host().endsWith(QLatin1String("login.live.com")) || url.host().endsWith(QLatin1String(".microsoft.com"))
            || url.host().endsWith(QLatin1String(".office.com")) || url.host().endsWith(QLatin1String(".skype.com"))
            || url.host().endsWith(QLatin1String(".cloud.microsoft"))
            || url.scheme() == QLatin1String("about") || url.scheme() == QLatin1String("blob") || url.scheme() == QLatin1String("data"))
            return true;
        QDesktopServices::openUrl(url);
        return false;
    }
};

// Owns the window's lifetime: shows it for a call, hides it afterwards, quits when idle.
class CallController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl homeUrl READ homeUrl CONSTANT)
public:
    CallController(const QUrl &home, bool keepWarm, QObject *parent = nullptr) : QObject(parent), m_home(home), m_keepWarm(keepWarm)
    {
        m_idle.setSingleShot(true);
        m_idle.setInterval(std::chrono::minutes(kWarmMinutes));
        connect(&m_idle, &QTimer::timeout, qApp, &QCoreApplication::quit);
    }

    QUrl homeUrl() const { return m_home; }
    void setWindow(QQuickWindow *window) { m_window = window; }

    // Started by the main app inside a systemd scope (KOLLEGA_CALL_SCOPE): while hidden the
    // scope has a memory ceiling, so the kernel compresses idle pages into zram; during a
    // call the ceiling is lifted so nothing slows down.
    static void setMemoryCeiling(bool limited)
    {
        const QString scope = qEnvironmentVariable("KOLLEGA_CALL_SCOPE");
        if (scope.isEmpty()) return;
        QProcess::startDetached(QStringLiteral("systemctl"),
                                {QStringLiteral("--user"), QStringLiteral("set-property"), QStringLiteral("--runtime"), scope,
                                 limited ? QStringLiteral("MemoryHigh=250M") : QStringLiteral("MemoryHigh=infinity")});
    }

    // A call or meeting to show (from the command line or another instance).
    void open(const QUrl &url)
    {
        if (!m_window) return;
        m_idle.stop();
        setMemoryCeiling(false);
        if (isTeamsUrl(url)) m_window->setProperty("target", url);
        m_window->showNormal();
        m_window->raise();
        m_window->requestActivate();
    }

    // "Ready for calls" was turned off: leave now unless a call is on screen.
    void quitIfIdle()
    {
        m_keepWarm = false;
        if (!m_window || !m_window->isVisible()) QCoreApplication::quit();
    }

    // The window was closed: the call has ended (the page went home); stay warm for a while.
    Q_INVOKABLE void windowHidden()
    {
        setMemoryCeiling(true);
        if (!m_keepWarm) m_idle.start();
    }

private:
    QQuickWindow *m_window = nullptr;
    QTimer m_idle;
    QUrl m_home;
    bool m_keepWarm = false;
};

int main(int argc, char *argv[])
{
    // Lighter Chromium for a single-purpose window (all are standard Chromium switches):
    // low-end mode trims caches and memory, no extensions, no background networking.
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--enable-low-end-device-mode --disable-extensions --disable-background-networking "
                "--disable-component-update --disable-sync --no-pings");
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kollega"));
    QCoreApplication::setApplicationName(QStringLiteral("kollega-capture"));
    app.setQuitOnLastWindowClosed(false);

    // Arguments: [url] [--login-hint <upn>] [--keep-warm] [--background] [--capture]
    QUrl start;
    QString loginHint;
    bool keepWarm = false, background = false, capture = false;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--quit")) continue;  // handled below, for a running instance
        if (args[i] == QLatin1String("--capture")) capture = true;
        else if (args[i] == QLatin1String("--keep-warm")) keepWarm = true;
        else if (args[i] == QLatin1String("--background")) background = true;
        else if (args[i] == QLatin1String("--login-hint") && i + 1 < args.size()) loginHint = args[++i];
        else start = QUrl(args[i]);
    }
    if (!start.isEmpty() && !isTeamsUrl(start)) return 2;
    if (args.contains(QLatin1String("--quit"))) {
        // Only meaningful for a running instance.
        QLocalSocket other;
        other.connectToServer(QString::fromLatin1(kServerName));
        if (other.waitForConnected(300)) { other.write("--quit\n"); other.waitForBytesWritten(1000); }
        return 0;
    }

    // Already running: hand over the address (or just "show yourself") and stop here.
    {
        QLocalSocket other;
        other.connectToServer(QString::fromLatin1(kServerName));
        if (other.waitForConnected(300)) {
            if (args.contains(QLatin1String("--quit"))) other.write("--quit\n");
            else if (!background) other.write(start.toString(QUrl::FullyEncoded).toUtf8() + '\n');
            other.waitForBytesWritten(1000);
            return 0;
        }
    }

    QUrl home(QString::fromLatin1(kHome));
    if (!loginHint.isEmpty()) {
        // A needed sign-in then starts with the account filled in (Teams web's own parameter).
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("loginHint"), loginHint);
        home.setQuery(q);
    }

    // Reuse the web profile the first app versions signed in with: usually no new sign-in.
    // (The native app's tokens cannot be shared: Microsoft binds them to the requesting client.)
    const QString profilePath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/kollega/kollega/QtWebEngine/kollega-auth");

    CallPolicy policy;
    CallController controller(home, keepWarm);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("startUrl"), start.isEmpty() ? home : start);
    engine.rootContext()->setContextProperty(QStringLiteral("startVisible"), !background);
    engine.rootContext()->setContextProperty(QStringLiteral("profilePath"), profilePath);
    engine.rootContext()->setContextProperty(QStringLiteral("policy"), &policy);
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    CaptureLog *captureLog = capture ? new CaptureLog(&app) : nullptr;
    engine.rootContext()->setContextProperty(QStringLiteral("captureLog"), captureLog);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/call/CallWindow.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    controller.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()));

    CallOnlyFilter filter;
    auto *view = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("web"));
    auto *profile = view ? qobject_cast<QQuickWebEngineProfile *>(view->property("profile").value<QObject *>()) : nullptr;
    if (!profile) qCWarning(lcCall) << "profile not found: no request filter";
    // While capturing, nothing is blocked: the point is to see what Teams really does.
    CaptureInterceptor *captureInterceptor = captureLog ? new CaptureInterceptor(captureLog, &app) : nullptr;
    if (profile && captureInterceptor) profile->setUrlRequestInterceptor(captureInterceptor);
    else if (profile && qEnvironmentVariableIsEmpty("KOLLEGA_CALL_NO_BLOCK")) profile->setUrlRequestInterceptor(&filter);
    QTimer stats;  // how much was spared, for the log
    QObject::connect(&stats, &QTimer::timeout, [&filter] { qCInfo(lcCall) << "requests blocked so far:" << filter.blocked(); });
    stats.start(std::chrono::seconds(30));
    if (background) controller.windowHidden();

    // Later starts send their address here (only this user can connect).
    QLocalServer::removeServer(QString::fromLatin1(kServerName));
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(QString::fromLatin1(kServerName));
    QObject::connect(&server, &QLocalServer::newConnection, &controller, [&server, &controller] {
        QLocalSocket *client = server.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::readyRead, &controller, [&controller, client] {
            while (client->canReadLine()) {
                const QString line = QString::fromUtf8(client->readLine().trimmed());
                if (line == QLatin1String("--quit")) controller.quitIfIdle();
                else controller.open(QUrl(line));
            }
        });
        QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    });

    return app.exec();
}

#include "main.moc"
