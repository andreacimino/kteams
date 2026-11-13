#include "trouter.h"
#include "trouterframes.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QUrlQuery>
#include <QUuid>
#include <QLoggingCategory>
#include <zlib.h>

// Protocol details: QT_LOGGING_RULES="kteams.trouter.debug=true"
Q_LOGGING_CATEGORY(lcTrouter, "kteams.trouter", QtInfoMsg)

namespace {
// Values purple-teams sends (teams_trouter.c, libteams.h).
constexpr auto kTcCv = "2024.23.01.2";
constexpr auto kClientVersion = "49/25113001312";
constexpr auto kDefaultSocketIo = "https://go.trouter.teams.microsoft.com/";
constexpr auto kRegistrar = "https://teams.microsoft.com/registrar/prod/V2/registrations";
constexpr int kTtlSecs = 86400;
constexpr int kPingSecs = 30;

// Correlation vector, as purple-teams' teams_generate_correlation_vector().
QString correlationVector()
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/+";
    static const char suffix[] = "AgQw";
    QString cv;
    for (int i = 0; i < 21; ++i) cv += QLatin1Char(chars[QRandomGenerator::global()->bounded(64)]);
    cv += QLatin1Char(suffix[QRandomGenerator::global()->bounded(4)]);
    return cv;
}
}

Trouter::Trouter(QObject *parent) : QObject(parent)
{
    // One endpoint id per installation, kept across runs (purple-teams keeps it per account).
    QSettings settings;
    m_endpointId = settings.value(QStringLiteral("trouter/endpoint")).toString();
    if (m_endpointId.isEmpty()) {
        m_endpointId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("trouter/endpoint"), m_endpointId);
    }

    m_ping.setInterval(std::chrono::seconds(kPingSecs));
    connect(&m_ping, &QTimer::timeout, this, [this] { sendCommand({{QStringLiteral("name"), QStringLiteral("ping")}}); });
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Trouter::requestSession);
    m_reregister.setInterval(std::chrono::seconds(kTtlSecs - 10));
    connect(&m_reregister, &QTimer::timeout, this, &Trouter::registerEndpoint);

    connect(&m_socket, &QWebSocket::textMessageReceived, this, &Trouter::onText);
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        setConnected(false);
        m_ping.stop();
        if (m_running) scheduleReconnect();
    });
}

void Trouter::start(const QString &skypeToken, TokenProvider tokens)
{
    m_skypeToken = skypeToken;
    m_tokens = std::move(tokens);
    m_running = true;
    m_failures = 0;
    requestSession();
}

void Trouter::stop()
{
    m_running = false;
    m_ping.stop();
    m_reconnect.stop();
    m_reregister.stop();
    m_socket.abort();
    setConnected(false);
}

void Trouter::setConnected(bool connected)
{
    if (m_connected == connected) return;
    m_connected = connected;
    if (connected) m_failures = 0;
    emit connectedChanged(connected);
}

void Trouter::scheduleReconnect()
{
    // 2 s, 4 s, 8 s ... up to 2 minutes.
    const int delay = qMin(120, 2 << qMin(m_failures, 6));
    ++m_failures;
    qCInfo(lcTrouter) << "reconnecting in" << delay << "s";
    m_reconnect.start(std::chrono::seconds(delay));
}

QString Trouter::connectQuery() const
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("v"), QStringLiteral("v4"));
    const QJsonObject params = m_info.value(QLatin1String("connectparams")).toObject();
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        q.addQueryItem(it.key(), QString::fromLatin1(QUrl::toPercentEncoding(it.value().toString())));
    const QString tc = QStringLiteral("{\"cv\":\"%1\",\"ua\":\"TeamsCDL\",\"hr\":\"\",\"v\":\"%2\"}")
                           .arg(QString::fromLatin1(kTcCv), QString::fromLatin1(kClientVersion));
    q.addQueryItem(QStringLiteral("tc"), QString::fromLatin1(QUrl::toPercentEncoding(tc)));
    q.addQueryItem(QStringLiteral("con_num"), QStringLiteral("1234567890123_1"));
    q.addQueryItem(QStringLiteral("epid"), m_endpointId);
    const QString ccid = m_info.value(QLatin1String("ccid")).toString();
    if (!ccid.isEmpty()) q.addQueryItem(QStringLiteral("ccid"), QString::fromLatin1(QUrl::toPercentEncoding(ccid)));
    q.addQueryItem(QStringLiteral("auth"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("timeout"), QStringLiteral("40"));
    return q.toString(QUrl::FullyEncoded);
}

void Trouter::requestSession()
{
    if (!m_running || m_skypeToken.isEmpty()) return;
    // purple-teams: teams_trouter_begin() -> teams_trouter_info_cb() -> teams_trouter_sessionid_cb()
    QNetworkRequest req(QUrl(QStringLiteral("https://go.trouter.teams.microsoft.com/v4/a?epid=") + m_endpointId));
    req.setRawHeader("x-skypetoken", m_skypeToken.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_nam.post(req, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_info = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || m_info.isEmpty()) {
            qCWarning(lcTrouter) << "info failed" << reply->errorString();
            scheduleReconnect();
            return;
        }
        QString socketIo = m_info.value(QLatin1String("socketio")).toString(QString::fromLatin1(kDefaultSocketIo));
        QNetworkRequest sessionReq(QUrl(socketIo + QStringLiteral("socket.io/1/?") + connectQuery()));
        sessionReq.setRawHeader("X-Skypetoken", m_skypeToken.toUtf8());
        QNetworkReply *session = m_nam.get(sessionReq);
        connect(session, &QNetworkReply::finished, this, [this, session] {
            session->deleteLater();
            // "<session id>:180:180:websocket,xhr-polling"
            const QString sessionId = QString::fromLatin1(session->readAll()).section(QLatin1Char(':'), 0, 0);
            if (session->error() != QNetworkReply::NoError || sessionId.isEmpty()) {
                qCWarning(lcTrouter) << "session failed" << session->errorString();
                scheduleReconnect();
                return;
            }
            openSocket(sessionId);
        });
    });
}

void Trouter::openSocket(const QString &sessionId)
{
    const QString socketIo = m_info.value(QLatin1String("socketio")).toString(QString::fromLatin1(kDefaultSocketIo));
    QString url = socketIo + QStringLiteral("socket.io/1/websocket/") + sessionId + QStringLiteral("?") + connectQuery();
    url.replace(QLatin1String("https://"), QLatin1String("wss://"));
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("X-Skypetoken", m_skypeToken.toUtf8());
    m_commandCount = 1;
    m_socket.abort();
    m_socket.open(req);
}

void Trouter::sendFrame(const QString &frame)
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) m_socket.sendTextMessage(frame);
}

void Trouter::sendCommand(const QJsonObject &command)
{
    sendFrame(QStringLiteral("5:%1+::").arg(m_commandCount++)
              + QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact)));
}

void Trouter::sendEphemeral(const QJsonObject &command)
{
    sendFrame(QStringLiteral("5:::") + QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact)));
}

void Trouter::authenticate()
{
    // purple-teams: teams_trouter_send_authentication() + teams_trouter_send_active()
    m_tokens([this](const QString &token) {
        const QJsonObject headers{{QStringLiteral("X-Ms-Test-User"), QStringLiteral("False")},
                                  {QStringLiteral("Authorization"), QStringLiteral("Bearer ") + token},
                                  {QStringLiteral("X-MS-Migration"), QStringLiteral("True")}};
        const QJsonObject arg{{QStringLiteral("headers"), headers},
                              {QStringLiteral("connectparams"), m_info.value(QLatin1String("connectparams"))}};
        sendEphemeral({{QStringLiteral("name"), QStringLiteral("user.authenticate")}, {QStringLiteral("args"), QJsonArray{arg}}});
        const QJsonObject activity{{QStringLiteral("state"), QStringLiteral("active")},
                                   {QStringLiteral("cv"), correlationVector() + QStringLiteral(".0.1")}};
        sendCommand({{QStringLiteral("name"), QStringLiteral("user.activity")}, {QStringLiteral("args"), QJsonArray{activity}}});
        registerEndpoint();
    });
}

void Trouter::sendActivity(bool active)
{
    // purple-teams: teams_trouter_send_active()
    const QJsonObject activity{{QStringLiteral("state"), active ? QStringLiteral("active") : QStringLiteral("inactive")},
                               {QStringLiteral("cv"), correlationVector() + QStringLiteral(".0.1")}};
    sendCommand({{QStringLiteral("name"), QStringLiteral("user.activity")}, {QStringLiteral("args"), QJsonArray{activity}}});
}

void Trouter::registerEndpoint()
{
    // purple-teams: teams_trouter_register(): NextGenCalling for call notifications, then
    // TeamsCDLWebWorker for events and messaging (registered last, with the endpoint id).
    const QString surl = m_info.value(QLatin1String("surl")).toString();
    if (surl.isEmpty()) return;
    m_tokens([this, surl](const QString &token) {
        registerApp(QStringLiteral("NextGenCalling"), QStringLiteral("DesktopNgc_2.3:SkypeNgc"), surl + QStringLiteral("NGCallManagerWin"),
                    QUuid::createUuid().toString(QUuid::WithoutBraces), token);
        registerApp(QStringLiteral("TeamsCDLWebWorker"), QStringLiteral("TeamsCDLWebWorker_2.1"), surl, m_endpointId, token);
    });
}

void Trouter::registerApp(const QString &appId, const QString &templateKey, const QString &path, const QString &registrationId,
                          const QString &token)
{
    const QJsonObject description{{QStringLiteral("appId"), appId},
                                  {QStringLiteral("aesKey"), QString()},
                                  {QStringLiteral("languageId"), QStringLiteral("en-US")},
                                  {QStringLiteral("platform"), QStringLiteral("edge")},
                                  {QStringLiteral("templateKey"), templateKey},
                                  {QStringLiteral("platformUIVersion"), QString::fromLatin1(kClientVersion)}};
    const QJsonObject transport{{QStringLiteral("context"), QString()}, {QStringLiteral("path"), path}, {QStringLiteral("ttl"), kTtlSecs}};
    const QJsonObject body{{QStringLiteral("clientDescription"), description},
                           {QStringLiteral("registrationId"), registrationId},
                           {QStringLiteral("nodeId"), QString()},
                           {QStringLiteral("transports"), QJsonObject{{QStringLiteral("TROUTER"), QJsonArray{transport}}}}};
    QNetworkRequest req{QUrl(QString::fromLatin1(kRegistrar))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-Skypetoken", m_skypeToken.toUtf8());
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, appId] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcTrouter) << "registration failed" << appId << reply->errorString();
            return;
        }
        qCInfo(lcTrouter) << "registered" << appId << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (appId == QLatin1String("TeamsCDLWebWorker")) {
            setConnected(true);
            m_reregister.start();
        }
    });
}

void Trouter::onText(const QString &frame)
{
    // Frames (socket.io v1): "1::" connected, "3:::{...}" request from the server,
    // "5:..." event, "6:..." ack.
    const TrouterFrames::Frame parsed = TrouterFrames::parseFrame(frame);
    if (parsed.kind == TrouterFrames::Frame::Kind::Connected) {
        qCInfo(lcTrouter) << "socket connected";
        m_ping.start();
        authenticate();
        return;
    }
    if (parsed.kind != TrouterFrames::Frame::Kind::Request) qCDebug(lcTrouter).noquote() << "frame" << frame.left(160);
    if (parsed.kind == TrouterFrames::Frame::Kind::Request) {
        // Acknowledge first, as purple-teams does.
        sendFrame(TrouterFrames::ackFrame(parsed.payload));
        handleRequest(parsed.payload);
    } else if (parsed.kind == TrouterFrames::Frame::Kind::Event) {
        // purple-teams registers again on every report, but each registration makes the server
        // repeat the same report: only a new loss (different etags) is worth it.
        const QByteArray indicators = TrouterFrames::lossIndicators(parsed.payload);
        if (!indicators.isNull() && indicators != m_lastLoss) {
            m_lastLoss = indicators;
            qCDebug(lcTrouter) << "message loss, registering again";
            registerEndpoint();
        }
    }
}

void Trouter::handleRequest(const QJsonObject &request)
{
    const TrouterFrames::Decoded decoded = TrouterFrames::decodeRequest(request);
    const QJsonObject &content = decoded.content;
    const QString url = request.value(QLatin1String("url")).toString();
    qCDebug(lcTrouter).noquote() << "push" << url.section(QLatin1Char('/'), -1)
                                 << content.value(QLatin1String("resourceType")).toString();
    switch (TrouterFrames::routeOf(url)) {
    case TrouterFrames::Route::CallAgent:
        if (content.isEmpty()) qCInfo(lcTrouter).noquote() << "callAgent raw body (" << decoded.body.size() << "bytes):" << QString::fromUtf8(decoded.body.left(300));
        emit callAgentRequest(url, content);
        break;
    case TrouterFrames::Route::Messaging:
        if (content.value(QLatin1String("type")).toString() == QLatin1String("EventMessage")) emit eventMessage(content);
        break;
    case TrouterFrames::Route::CallManager:
        // purple-teams (teams_trouter.c): evt 107 = incoming call; "cp" holds callNotification.
        if (decoded.evt == 107) {
            const TrouterFrames::Caller caller = TrouterFrames::callerOf(content);
            emit incomingCall(caller.mri, caller.name, content);
        }
        break;
    case TrouterFrames::Route::Presence:
        emit presence(content.value(QLatin1String("presence")).toArray());
        break;
    case TrouterFrames::Route::Other:
        break;
    }
}
