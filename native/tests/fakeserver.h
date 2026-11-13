#pragma once
// A fake HTTP server on loopback for the tests that talk to a service: it records every
// request and answers with the configured status, headers and body (or what `respond` returns).
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

struct Request {
    QByteArray method;
    QByteArray target;       // path and query, as sent
    QByteArray body;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QUrl url() const { return QUrl(QString::fromLatin1(target)); }
    QString path() const { return url().path(QUrl::FullyEncoded); }
    QString query(const QString &name) const { return QUrlQuery(url()).queryItemValue(name, QUrl::FullyDecoded); }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    // The body of a form post (application/x-www-form-urlencoded), decoded.
    QString form(const QString &name) const { return QUrlQuery(QString::fromUtf8(body)).queryItemValue(name, QUrl::FullyDecoded); }
};

class FakeServer : public QObject
{
public:
    FakeServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    void stop() { m_server.close(); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    // Waits until `count` requests have arrived (in total since the last clear).
    bool waitForRequests(qsizetype count, int msecs = 5000) { return QTest::qWaitFor([&] { return requests.size() >= count; }, msecs); }
    void reset()
    {
        status = 200;
        answer = "{}";
        extraHeaders.clear();
        respond = nullptr;
        statusOf = nullptr;
        requests.clear();
    }

    int status = 200;
    QByteArray answer = "{}";
    QByteArray extraHeaders;                         // full header lines, each ending in \r\n
    std::function<QByteArray(const Request &)> respond; // when set, the body of the answer
    std::function<int(const Request &)> statusOf;       // when set, the status of the answer
    QList<Request> requests;

private:
    void read(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        Request r;
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        r.method = first.value(0);
        r.target = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0) r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = r.headers.value("content-length", "0").toInt();
        if (buffer.size() < headerEnd + 4 + length) return;
        r.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        requests.append(r);
        const QByteArray body = respond ? respond(r) : answer;
        socket->write("HTTP/1.1 " + QByteArray::number(statusOf ? statusOf(r) : status) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
