#include "chatparser.h"

#include "content.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

// The strings keep the "Backend" translation context they had inside Backend, so the
// existing translations still apply.
using namespace Content;

namespace ChatParser {

QString selfMri(const SelfContext &ctx)
{
    return QStringLiteral("8:") + ctx.skypeName;
}

bool isSelf(const SelfContext &ctx, const QString &fromUrl)
{
    // "from" is a contact URL ending in the MRI, e.g. ".../contacts/8:orgid:<guid>".
    return !ctx.skypeName.isEmpty() && fromUrl.endsWith(ctx.skypeName);
}

QString titleFromMembers(const SelfContext &ctx, const QString &conversationId, bool oneToOne)
{
    const auto members = ctx.chatMembers.constFind(conversationId);
    if (members == ctx.chatMembers.cend() || members->isEmpty()) return {};

    QStringList names;
    for (const QString &mri : *members) {
        const auto person = ctx.people.constFind(mri);
        if (person == ctx.people.cend() || person->displayName.isEmpty()) continue;
        if (oneToOne) return person->displayName;
        names << (person->givenName.isEmpty() ? person->displayName.section(QLatin1Char(' '), 0, 0) : person->givenName);
    }
    if (names.isEmpty()) return {};
    const qsizetype total = members->size();
    if (total > 3) return QStringLiteral("%1, %2, +%3").arg(names.value(0), names.value(1)).arg(total - 2);
    if (names.size() == 1) return names.first();
    return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" e ") + names.last();
}

QVariantList parseReactions(const SelfContext &ctx, const QJsonObject &properties)
{
    // purple-teams (teams_messages.c): "emotions": [{"key": "like", "users": [{"mri", "time", "value"}]}]
    const QJsonValue v = properties.value(QLatin1String("emotions"));
    const QJsonArray emotions = v.isString() ? QJsonDocument::fromJson(v.toString().toUtf8()).array() : v.toArray();
    const QString self = selfMri(ctx);
    QVariantList result;
    for (const auto &e : emotions) {
        const QJsonObject o = e.toObject();
        const QJsonArray users = o.value(QLatin1String("users")).toArray();
        if (users.isEmpty()) continue;
        bool mine = false;
        QStringList names;
        for (const auto &u : users) {
            const QString mri = u.toObject().value(QLatin1String("mri")).toString();
            if (mri == self) { mine = true; names << QStringLiteral("Tu"); continue; }
            const QString name = ctx.people.value(mri).displayName;
            names << (name.isEmpty() ? QCoreApplication::translate("Backend", "Someone") : name);
        }
        const QString key = o.value(QLatin1String("key")).toString();
        result << QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("emoji"), emojiForKey(key)},
                              {QStringLiteral("count"), int(users.size())}, {QStringLiteral("mine"), mine},
                              {QStringLiteral("names"), names.join(QStringLiteral(", "))}};
    }
    return result;
}

bool mentionsSelf(const SelfContext &ctx, const QJsonObject &properties)
{
    // "mentions": JSON array (often as a string) of {"mri": ..., "mentionType": ...}
    const QJsonValue v = properties.value(QLatin1String("mentions"));
    const QJsonArray mentions = v.isString() ? QJsonDocument::fromJson(v.toString().toUtf8()).array() : v.toArray();
    const QString self = selfMri(ctx);
    return std::any_of(mentions.cbegin(), mentions.cend(),
                       [&](const QJsonValue &m) { return m.toObject().value(QLatin1String("mri")).toString() == self; });
}

bool parseConversation(const SelfContext &ctx, const QJsonObject &o, ConversationItem &it, bool &lastFromOther)
{
    it.id = o.value(QLatin1String("id")).toString();
    if (it.id.isEmpty()) return false;
    // System conversations (notifications, call logs...), except personal notes.
    if (it.id.startsWith(QLatin1String("48:")) && it.id != QLatin1String("48:notes")) return false;

    const QJsonObject thread = o.value(QLatin1String("threadProperties")).toObject();
    const QJsonObject last = o.value(QLatin1String("lastMessage")).toObject();
    const QJsonObject props = o.value(QLatin1String("properties")).toObject();
    const bool oneToOne = thread.value(QLatin1String("uniquerosterthread")).toString() == QLatin1String("true")
        || thread.value(QLatin1String("productThreadType")).toString() == QLatin1String("OneToOneChat");
    const QString lastFrom = last.value(QLatin1String("from")).toString();
    it.lastFromMri = lastFrom.section(QLatin1Char('/'), -1);
    const bool chatMessage = isChatMessage(last.value(QLatin1String("messagetype")).toString());
    lastFromOther = chatMessage && !lastFrom.isEmpty() && !isSelf(ctx, lastFrom);

    it.type = oneToOne ? QStringLiteral("oneToOne") : thread.value(QLatin1String("threadType")).toString();
    it.title = thread.value(QLatin1String("topic")).toString();
    it.hasTopic = !it.title.isEmpty();
    if (it.title.isEmpty() && it.id == QLatin1String("48:notes")) it.title = QCoreApplication::translate("Backend", "Personal notes");
    if (it.title.isEmpty()) it.title = titleFromMembers(ctx, it.id, oneToOne);
    if (oneToOne) it.peerMri = ctx.chatMembers.value(it.id).value(0);
    if (it.title.isEmpty() && oneToOne && lastFromOther) it.title = last.value(QLatin1String("imdisplayname")).toString();
    if (it.title.isEmpty()) it.title = oneToOne ? QCoreApplication::translate("Backend", "Chat") : QCoreApplication::translate("Backend", "Group chat");

    if (chatMessage) {
        const QString who = last.value(QLatin1String("imdisplayname")).toString();
        const ParsedContent parsed = parseContent(last.value(QLatin1String("content")).toString());
        QString text = parsed.text.simplified();
        const QVariantList files = parseFiles(last.value(QLatin1String("properties")).toObject());
        if (text.isEmpty() && !files.isEmpty()) text = QStringLiteral("📎 ") + files.first().toMap().value(QStringLiteral("name")).toString();
        if (text.isEmpty() && !parsed.images.isEmpty()) text = QCoreApplication::translate("Backend", "📷 Picture");
        it.preview = (who.isEmpty() || oneToOne ? QString() : who + QStringLiteral(": ")) + text;
    }
    it.lastActivity = parseTime(last.value(QLatin1String("composetime")).toString());
    it.lastMessageId = last.value(QLatin1String("id")).toString();

    // Pinned ("favorite"), muted ("alerts" off) and hidden chats: properties of the
    // conversation. Logged once while the names are being confirmed against Teams.
    it.pinned = props.value(QLatin1String("favorite")).toVariant().toString() == QLatin1String("true");
    it.muted = props.value(QLatin1String("alerts")).toVariant().toString() == QLatin1String("false");
    static int logged = 0;
    if (logged < 3 && !props.isEmpty()) { ++logged; qInfo() << "conversation properties:" << props.keys(); }
    if (props.value(QLatin1String("hidden")).toVariant().toString() == QLatin1String("true")) return false;
    // consumptionhorizon = "<last read message id>;<time>;<clientmessageid>"; message ids grow over time.
    const QString horizon = props.value(QLatin1String("consumptionhorizon")).toString().section(QLatin1Char(';'), 0, 0);
    it.unread = lastFromOther && !horizon.isEmpty() && !it.lastMessageId.isEmpty()
        && it.lastMessageId.toLongLong() > horizon.toLongLong();
    return true;
}

QVector<MessageItem> parseMessages(const SelfContext &ctx, const QJsonArray &arr, qint64 *oldestRaw)
{
    // Paging works on all messages (system events included): the next page ends before the oldest.
    QVector<MessageItem> items;
    items.reserve(arr.size());
    // The API returns newest first, which is the order MessageModel wants.
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        const qint64 ts = parseTime(o.value(QLatin1String("composetime")).toString(o.value(QLatin1String("originalarrivaltime")).toString()));
        if (oldestRaw && ts > 0 && (*oldestRaw == 0 || ts < *oldestRaw)) *oldestRaw = ts;

        const QString type = o.value(QLatin1String("messagetype")).toString();
        if (!isChatMessage(type)) continue;
        MessageItem m;
        m.id = o.value(QLatin1String("id")).toString();
        m.clientMessageId = o.value(QLatin1String("clientmessageid")).toString();
        m.rootId = o.value(QLatin1String("rootMessageId")).toString();
        const QString from = o.value(QLatin1String("from")).toString();
        m.mine = isSelf(ctx, from);
        m.fromId = from.section(QLatin1Char('/'), -1);
        m.from = o.value(QLatin1String("imdisplayname")).toString();
        if (m.from.isEmpty()) m.from = from.section(QLatin1Char('/'), -1);
        const QString content = o.value(QLatin1String("content")).toString();
        if (type == QLatin1String("Text")) {
            m.content = content;
        } else {
            const ParsedContent parsed = parseContent(content);
            m.richText = parsed.richText;
            m.content = parsed.text;
            m.images = parsed.images;
        }
        if (m.richText.isEmpty()) m.richText = linkify(m.content);
        m.files = parseFiles(o.value(QLatin1String("properties")).toObject());
        const QJsonObject props = o.value(QLatin1String("properties")).toObject();
        m.reactions = parseReactions(ctx, props);
        // purple-teams: "deletetime" / "edittime" (or skypeeditedid) in the message
        m.deleted = props.contains(QLatin1String("deletetime"));
        m.edited = !m.deleted && (props.contains(QLatin1String("edittime")) || o.contains(QLatin1String("skypeeditedid")));
        m.mentionsMe = mentionsSelf(ctx, props);
        if (m.deleted) {
            m.content = QCoreApplication::translate("Backend", "This message was deleted.");
            m.richText = m.content.toHtmlEscaped();
            m.images.clear();
            m.files.clear();
        }
        m.time = o.value(QLatin1String("composetime")).toString(o.value(QLatin1String("originalarrivaltime")).toString());
        m.timestamp = ts;
        if (!m.content.isEmpty() || !m.images.isEmpty() || !m.files.isEmpty()) items.push_back(m);
    }
    return items;
}

}
