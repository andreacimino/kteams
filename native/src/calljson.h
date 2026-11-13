#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

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

// What to show when a call ends, from the body of call/end or conversation/conversationEnd:
// declined, busy, no answer, failed (with the server's phrase), or just "Call ended".
QString callEndText(const QJsonObject &body);

// The "groupChat" of the request that starts a call: null for the test call and for a call to one
// person (as Teams sends it, so the callee gets an ordinary one-to-one call), the chat's thread
// for a group call and for a meeting.
QJsonValue groupChatFor(bool testCall, bool meeting, const QStringList &callees, const QString &threadId);

struct IncomingCall {
    QString callerMri;
    QString callerName;
    QString offer;         // the caller's SDP, if it came in the notification
    QString legId;         // the media leg of that offer
    QString participantId; // the id the service already gave us in this call
    QString attachUrl;     // where to attach; empty: the notification is unusable
    QString joinUrl;       // the conversation controller, for the "join" action
    QString threadId;      // the group chat of a group call or meeting; empty for a one-to-one call
};
// A Trouter NGCallManagerWin notification (evt 107) of an incoming call.
IncomingCall parseIncomingCall(const QJsonObject &notification);

struct AttachResponse {
    QJsonObject invitationLinks;   // acceptance, callLeg (reject), progress, mediaAnswer
    QString participantId;         // ours, as the service sees it; may be empty
    QString offer;                 // the SDP, when it was not in the notification
    QString legId;
    QJsonObject conversationLinks; // leave...: from the "join" action's response
    bool hasConversationLinks = false;
};
// The answer to the attach request.
AttachResponse parseAttachResponse(const QJsonObject &response);

}
