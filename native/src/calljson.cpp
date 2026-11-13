#include "calljson.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QStringList>
#include <functional>

namespace CallJson {

QString findSdp(const QJsonValue &v)
{
    if (v.isString()) return v.toString().startsWith(QLatin1String("v=0")) ? v.toString() : QString();
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (const QString s = findSdp(it.value()); !s.isEmpty()) return s;
    }
    if (v.isArray())
        for (const auto &x : v.toArray())
            if (const QString s = findSdp(x); !s.isEmpty()) return s;
    return {};
}

QString shape(const QJsonValue &v, int depth)
{
    if (depth > 2) return QStringLiteral("…");
    if (v.isObject()) {
        QStringList p;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) p << it.key() + QLatin1Char(':') + shape(it.value(), depth + 1);
        return QLatin1Char('{') + p.join(QLatin1Char(',')) + QLatin1Char('}');
    }
    if (v.isArray()) return QStringLiteral("[%1]").arg(v.toArray().size());
    return v.isString() ? QStringLiteral("s") : QStringLiteral("v");
}

QList<QPair<QString, QString>> mediaControllerLinks(const QJsonValue &v, const QString &ownTrouterUrl)
{
    QList<QPair<QString, QString>> found;
    if (v.isArray()) {
        for (const auto &x : v.toArray()) found += mediaControllerLinks(x, ownTrouterUrl);
        return found;
    }
    if (!v.isObject()) return found;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.value().isString()) {
            const QString url = it.value().toString();
            if ((it.key() == QLatin1String("applyChannelParameters") || it.key() == QLatin1String("controlVideoStreaming")
                 || it.key() == QLatin1String("mediaRenegotiation"))
                && url.startsWith(QLatin1String("https://")) && !url.startsWith(ownTrouterUrl))
                found.append({it.key(), url});
        } else {
            found += mediaControllerLinks(it.value(), ownTrouterUrl);
        }
    }
    return found;
}

QList<RosterStream> rosterStreams(const QJsonObject &body, const QString &selfMri)
{
    QList<RosterStream> streams;
    std::function<void(const QString &, const QJsonValue &)> scan = [&](const QString &who, const QJsonValue &v) {
        if (v.isArray()) { for (const auto &x : v.toArray()) scan(who, x); return; }
        if (!v.isObject()) return;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == QLatin1String("mediaStreams") && it.value().isArray()) {
                for (const auto &s : it.value().toArray()) {
                    const QJsonObject stream = s.toObject();
                    RosterStream r;
                    r.who = who;
                    r.type = stream.value(QLatin1String("type")).toString().toLower();
                    r.label = stream.value(QLatin1String("label")).toString();
                    r.direction = stream.value(QLatin1String("direction")).toString().toLower();
                    r.sourceId = stream.value(QLatin1String("sourceId")).toInt(stream.value(QLatin1String("sourceId")).toString().toInt());
                    streams.append(r);
                }
            } else {
                scan(who, it.value());
            }
        }
    };
    const QJsonObject participants = body.value(QLatin1String("participants")).toObject();
    for (auto it = participants.begin(); it != participants.end(); ++it)
        if (it.key() != selfMri) scan(it.key(), it.value());
    return streams;
}

QString callEndText(const QJsonObject &body)
{
    const QJsonObject end = body.value(QLatin1String("callEnd")).toObject();
    const QString reason = end.value(QLatin1String("reason")).toString(body.value(QLatin1String("reason")).toString());
    const int code = end.value(QLatin1String("code")).toInt(body.value(QLatin1String("code")).toInt());
    if (reason == QLatin1String("declined") || code == 603) return QCoreApplication::translate("NativeCall", "Call declined");
    if (reason == QLatin1String("busy") || code == 486) return QCoreApplication::translate("NativeCall", "Busy");
    if (reason == QLatin1String("noAnswer") || code == 480 || code == 408) return QCoreApplication::translate("NativeCall", "No answer");
    if (reason == QLatin1String("clientError") || code >= 400)
        return QCoreApplication::translate("NativeCall", "Call failed (%1)").arg(end.value(QLatin1String("phrase")).toString(QString::number(code)).left(80));
    return QCoreApplication::translate("NativeCall", "Call ended");
}

IncomingCall parseIncomingCall(const QJsonObject &notification)
{
    const QJsonObject call = notification.value(QLatin1String("callNotification")).toObject();
    const QJsonObject from = call.value(QLatin1String("from")).toObject();
    IncomingCall invite;
    invite.callerMri = from.value(QLatin1String("id")).toString();
    invite.callerName = from.value(QLatin1String("displayName")).toString();
    invite.offer = findSdp(call.value(QLatin1String("mediaContent")));
    invite.legId = call.value(QLatin1String("mediaContent")).toObject().value(QLatin1String("mediaLegId")).toString();
    invite.participantId = call.value(QLatin1String("to")).toObject().value(QLatin1String("participantId")).toString();
    invite.attachUrl = call.value(QLatin1String("links")).toObject().value(QLatin1String("attach")).toString();
    invite.joinUrl = notification.value(QLatin1String("conversationInvitation")).toObject().value(QLatin1String("conversationController")).toString();
    invite.threadId = notification.value(QLatin1String("groupChat")).toObject().value(QLatin1String("threadId")).toString();
    return invite;
}

AttachResponse parseAttachResponse(const QJsonObject &response)
{
    AttachResponse r;
    const QJsonObject invitation = response.value(QLatin1String("callInvitation")).toObject();
    r.invitationLinks = invitation.value(QLatin1String("links")).toObject();
    r.participantId = response.value(QLatin1String("participants")).toObject().value(QLatin1String("to")).toObject()
                          .value(QLatin1String("participantId")).toString();
    r.offer = findSdp(invitation.value(QLatin1String("mediaContent")));
    r.legId = invitation.value(QLatin1String("mediaContent")).toObject().value(QLatin1String("mediaLegId")).toString();
    for (const auto &a : response.value(QLatin1String("additionalActionResponses")).toArray()) {
        const QJsonObject action = a.toObject();
        const QJsonObject inner = action.contains(QLatin1String("response")) ? action.value(QLatin1String("response")).toObject() : action;
        if (inner.contains(QLatin1String("links"))) {
            r.conversationLinks = inner.value(QLatin1String("links")).toObject();
            r.hasConversationLinks = true;
        }
    }
    return r;
}

}
