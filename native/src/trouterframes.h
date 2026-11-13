#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Trouter's wire format: socket.io v1 frames, gzip/base64 bodies, what a push is about.
// Pure functions, no socket (see tests/tst_trouterframes.cpp).
namespace TrouterFrames {

// gzip (not zlib) stream -> bytes; empty when the data is not a complete gzip stream.
QByteArray gunzip(const QByteArray &data);

struct Frame {
    // "1::" connected, "3:::{...}" request from the server, "5:::{...}" event, anything else (ack, ping...).
    enum class Kind { Connected, Request, Event, Other };
    Kind kind = Kind::Other;
    QJsonObject payload; // the JSON after the third colon (requests and events)
};
Frame parseFrame(const QString &frame);

// What we send back for a request, before handling it: its id, status 200, empty body.
QString ackFrame(const QJsonObject &request);

struct Decoded {
    QJsonObject content; // the push itself, decompressed
    int evt = 0;         // the "evt" of the outer envelope (107: incoming call)
    QByteArray body;     // the request body after the outer gzip/base64 step, for logs
};
// Request body -> push content: outer gzip+base64 (announced by a header, or recognisable by
// its magic "H4sI"), then the inner compressed ("cp", gzip+base64) or plain ("gp", base64) payload.
Decoded decodeRequest(const QJsonObject &request);

enum class Route { CallAgent, Messaging, CallManager, Presence, Other };
// Which service a push is for, from the end of its URL.
Route routeOf(const QString &url);

struct Caller { QString mri; QString name; };
// callNotification.from of an incoming call push.
Caller callerOf(const QJsonObject &content);

// "trouter.message_loss" events: the compact JSON of their indicators, so repeats can be
// ignored; a null array for any other event.
QByteArray lossIndicators(const QJsonObject &event);

}
