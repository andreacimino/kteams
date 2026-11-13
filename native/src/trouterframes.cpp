#include "trouterframes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <zlib.h>

namespace TrouterFrames {

QByteArray gunzip(const QByteArray &data)
{
    z_stream s{};
    if (inflateInit2(&s, 16 + MAX_WBITS) != Z_OK) return {};
    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    s.avail_in = uInt(data.size());
    QByteArray out;
    char buffer[16384];
    int rc = Z_OK;
    while (rc == Z_OK) {
        s.next_out = reinterpret_cast<Bytef *>(buffer);
        s.avail_out = sizeof(buffer);
        rc = inflate(&s, Z_NO_FLUSH);
        out.append(buffer, int(sizeof(buffer) - s.avail_out));
    }
    inflateEnd(&s);
    return rc == Z_STREAM_END ? out : QByteArray();
}

Frame parseFrame(const QString &frame)
{
    Frame result;
    if (frame.startsWith(QLatin1Char('1'))) { result.kind = Frame::Kind::Connected; return result; }
    const bool request = frame.startsWith(QLatin1Char('3'));
    if (!request && !frame.startsWith(QLatin1Char('5'))) return result;
    result.kind = request ? Frame::Kind::Request : Frame::Kind::Event;
    int colons = 0;
    for (int i = 0; i < frame.size(); ++i) {
        if (frame.at(i) == QLatin1Char(':') && ++colons == 3) {
            result.payload = QJsonDocument::fromJson(frame.mid(i + 1).toUtf8()).object();
            break;
        }
    }
    return result;
}

QString ackFrame(const QJsonObject &request)
{
    const QJsonObject response{{QStringLiteral("id"), request.value(QLatin1String("id"))},
                               {QStringLiteral("status"), 200}, {QStringLiteral("body"), QString()}};
    return QStringLiteral("3:::") + QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
}

Decoded decodeRequest(const QJsonObject &request)
{
    Decoded d;
    d.body = request.value(QLatin1String("body")).toString().toUtf8();
    const QJsonObject headers = request.value(QLatin1String("headers")).toObject();
    if (headers.value(QLatin1String("X-Microsoft-Skype-Content-Encoding")).toString() == QLatin1String("gzip")
        || d.body.startsWith("H4sI"))
        d.body = gunzip(QByteArray::fromBase64(d.body));
    QJsonObject content = QJsonDocument::fromJson(d.body).object();
    d.evt = content.value(QLatin1String("evt")).toInt();
    // purple-teams: compressed ("cp", gzip+base64) or plain ("gp", base64) payloads
    if (content.contains(QLatin1String("cp")))
        content = QJsonDocument::fromJson(gunzip(QByteArray::fromBase64(content.value(QLatin1String("cp")).toString().toUtf8()))).object();
    else if (content.contains(QLatin1String("gp")))
        content = QJsonDocument::fromJson(QByteArray::fromBase64(content.value(QLatin1String("gp")).toString().toUtf8())).object();
    d.content = content;
    return d;
}

Route routeOf(const QString &url)
{
    if (url.contains(QLatin1String("/callAgent/"))) return Route::CallAgent;
    if (url.endsWith(QLatin1String("/messaging"))) return Route::Messaging;
    if (url.endsWith(QLatin1String("/NGCallManagerWin"))) return Route::CallManager;
    if (url.endsWith(QLatin1String("/TeamsUnifiedPresenceService")) || url.endsWith(QLatin1String("/unifiedPresenceService")))
        return Route::Presence;
    return Route::Other;
}

bool isIncomingCallEvent(int evt) { return evt == 107 || evt == 109; }

Caller callerOf(const QJsonObject &content)
{
    const QJsonObject from = content.value(QLatin1String("callNotification")).toObject().value(QLatin1String("from")).toObject();
    return {from.value(QLatin1String("id")).toString(), from.value(QLatin1String("displayName")).toString()};
}

QByteArray lossIndicators(const QJsonObject &event)
{
    if (event.value(QLatin1String("name")).toString() != QLatin1String("trouter.message_loss")) return {};
    return QJsonDocument(event.value(QLatin1String("args")).toArray()).toJson(QJsonDocument::Compact);
}

}
