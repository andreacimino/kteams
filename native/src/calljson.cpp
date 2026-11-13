#include "calljson.h"

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

}
