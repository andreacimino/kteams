#include "statecache.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace StateCache {

QJsonObject conversationToJson(const ConversationItem &c)
{
    return {{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("type"), c.type},
            {QStringLiteral("preview"), c.preview}, {QStringLiteral("lastActivity"), c.lastActivity},
            {QStringLiteral("lastMessageId"), c.lastMessageId}, {QStringLiteral("unread"), c.unread},
            {QStringLiteral("hasTopic"), c.hasTopic}, {QStringLiteral("peerMri"), c.peerMri}, {QStringLiteral("lastFromMri"), c.lastFromMri}};
}
ConversationItem conversationFromJson(const QJsonObject &o)
{
    ConversationItem c;
    c.id = o.value(QLatin1String("id")).toString(); c.title = o.value(QLatin1String("title")).toString();
    c.type = o.value(QLatin1String("type")).toString(); c.preview = o.value(QLatin1String("preview")).toString();
    c.lastActivity = o.value(QLatin1String("lastActivity")).toVariant().toLongLong();
    c.lastMessageId = o.value(QLatin1String("lastMessageId")).toString(); c.unread = o.value(QLatin1String("unread")).toBool();
    c.hasTopic = o.value(QLatin1String("hasTopic")).toBool(); c.peerMri = o.value(QLatin1String("peerMri")).toString();
    c.lastFromMri = o.value(QLatin1String("lastFromMri")).toString();
    return c;
}
QJsonObject messageToJson(const MessageItem &m)
{
    return {{QStringLiteral("id"), m.id}, {QStringLiteral("clientMessageId"), m.clientMessageId}, {QStringLiteral("from"), m.from},
            {QStringLiteral("fromId"), m.fromId}, {QStringLiteral("content"), m.content}, {QStringLiteral("richText"), m.richText},
            {QStringLiteral("images"), QJsonArray::fromStringList(m.images)}, {QStringLiteral("files"), QJsonArray::fromVariantList(m.files)},
            {QStringLiteral("reactions"), QJsonArray::fromVariantList(m.reactions)}, {QStringLiteral("time"), m.time},
            {QStringLiteral("timestamp"), m.timestamp}, {QStringLiteral("mine"), m.mine}, {QStringLiteral("edited"), m.edited},
            {QStringLiteral("deleted"), m.deleted}, {QStringLiteral("mentionsMe"), m.mentionsMe}};
}
MessageItem messageFromJson(const QJsonObject &o)
{
    MessageItem m;
    m.id = o.value(QLatin1String("id")).toString(); m.clientMessageId = o.value(QLatin1String("clientMessageId")).toString();
    m.from = o.value(QLatin1String("from")).toString(); m.fromId = o.value(QLatin1String("fromId")).toString();
    m.content = o.value(QLatin1String("content")).toString(); m.richText = o.value(QLatin1String("richText")).toString();
    for (const auto &i : o.value(QLatin1String("images")).toArray()) m.images << i.toString();
    m.files = o.value(QLatin1String("files")).toArray().toVariantList(); m.reactions = o.value(QLatin1String("reactions")).toArray().toVariantList();
    m.time = o.value(QLatin1String("time")).toString(); m.timestamp = o.value(QLatin1String("timestamp")).toVariant().toLongLong();
    m.mine = o.value(QLatin1String("mine")).toBool(); m.edited = o.value(QLatin1String("edited")).toBool();
    m.deleted = o.value(QLatin1String("deleted")).toBool(); m.mentionsMe = o.value(QLatin1String("mentionsMe")).toBool();
    return m;
}
QString cacheFileName(const QString &conversationId)
{
    return QString::fromLatin1(QCryptographicHash::hash(conversationId.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".json");
}

// A cache file: {"conversation": id, "messages": [...]} (older files: the bare array).
QVector<MessageItem> readMessagesFile(const QString &path, QString *conversationId)
{
    QVector<MessageItem> items;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return items;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value(QLatin1String("messages")).toArray();
    if (conversationId) *conversationId = doc.isObject() ? doc.object().value(QLatin1String("conversation")).toString() : QString();
    for (const auto &v : arr) items.push_back(messageFromJson(v.toObject()));
    return items;
}

}
