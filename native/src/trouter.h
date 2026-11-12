#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <functional>

// Trouter: Teams' push channel (a socket.io v1 style websocket). Delivers new messages,
// typing, edits and presence as they happen, so the client does not need to poll.
// Protocol and endpoints follow purple-teams (teams_trouter.c).
class Trouter : public QObject
{
    Q_OBJECT
public:
    // Calls back with a Bearer token for api.spaces.skype.com.
    using TokenProvider = std::function<void(std::function<void(const QString &)>)>;

    explicit Trouter(QObject *parent = nullptr);

    void start(const QString &skypeToken, TokenProvider tokens);
    void setSkypeToken(const QString &skypeToken) { m_skypeToken = skypeToken; }
    void stop();
    bool isConnected() const { return m_connected; }
    // This installation's endpoint id (also used for presence, as purple-teams does).
    QString endpointId() const { return m_endpointId; }
    // Our Trouter address: links pointing here (call callbacks) reach this client.
    QString surl() const { return m_info.value(QLatin1String("surl")).toString(); }
    void sendActivity(bool active);

signals:
    void connectedChanged(bool connected);
    // Body of a /messaging request: {"type": "EventMessage", "resourceType": ..., "resource": {...}}
    void eventMessage(const QJsonObject &event);
    // Presence pushes: same items as the getpresence response.
    void presence(const QJsonArray &presences);
    // Incoming call (purple-teams: evt 107 on NGCallManagerWin): caller's MRI and name.
    void incomingCall(const QString &callerMri, const QString &callerName, const QJsonObject &notification);
    // A request for a call callback link we handed out (".../callAgent/...").
    void callAgentRequest(const QString &url, const QJsonObject &body);

private:
    void requestSession();
    void openSocket(const QString &sessionId);
    void onText(const QString &frame);
    void handleRequest(const QJsonObject &request);
    void sendFrame(const QString &frame);
    void sendCommand(const QJsonObject &command);   // "5:<n>+::" (expects an ack)
    void sendEphemeral(const QJsonObject &command); // "5:::"
    void authenticate();
    void registerEndpoint();
    void registerApp(const QString &appId, const QString &templateKey, const QString &path, const QString &registrationId,
                     const QString &token);
    void setConnected(bool connected);
    void scheduleReconnect();
    QString connectQuery() const;

    QNetworkAccessManager m_nam;
    QWebSocket m_socket;
    QTimer m_ping;
    QTimer m_reconnect;
    QTimer m_reregister;
    QString m_skypeToken;
    TokenProvider m_tokens;
    QString m_endpointId;
    QJsonObject m_info;   // socketio, surl, connectparams, ccid
    QByteArray m_lastLoss; // last trouter.message_loss report, to ignore repeats
    int m_commandCount = 1;
    int m_failures = 0;
    bool m_connected = false;
    bool m_running = false;
};
