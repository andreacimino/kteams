#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>

// Reading the JSON of Teams' calling service: where the SDP is, which callback links the
// media controller gave us, which streams the roster lists. Pure functions (see tests/tst_calljson.cpp).
namespace CallJson {

// The SDP inside a callback, wherever it is ("blob" of a mediaContent, usually); empty if none.
QString findSdp(const QJsonValue &v);

// Field names only, down to two levels, for the log: the protocol is still being learned.
QString shape(const QJsonValue &v, int depth = 0);

// The media controller's callback links (applyChannelParameters, controlVideoStreaming,
// mediaRenegotiation) found anywhere in `v`, in document order, as (name, url). Links that
// start with `ownTrouterUrl` are ours (clientContentForMediaController) and are skipped.
QList<QPair<QString, QString>> mediaControllerLinks(const QJsonValue &v, const QString &ownTrouterUrl);

struct RosterStream {
    QString who;       // participant MRI
    QString type;      // "video", "audio" (lower case)
    QString label;
    QString direction; // lower case: "sendonly", "sendrecv"...
    int sourceId = 0;
};
// participants[mri]...mediaStreams[{type, label, sourceId, direction}], except `selfMri`'s own.
QList<RosterStream> rosterStreams(const QJsonObject &body, const QString &selfMri);

}
