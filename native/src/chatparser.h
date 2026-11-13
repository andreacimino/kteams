#pragma once

#include "models.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

// Teams' chat service JSON -> the items the models show: conversations, messages, reactions.
// Pure functions: who we are and who the other people are come in as a SelfContext, so the
// tests can feed them JSON without a Backend (see tests/tst_chatparser.cpp).
namespace ChatParser {

struct Person { QString displayName; QString givenName; QString email; QString jobTitle; QString department; };

// Who we are, and what is known about the people around. References: the caller owns the data.
struct SelfContext {
    QString skypeName;                                // own MRI without the "8:" prefix
    const QHash<QString, Person> &people;             // MRI -> names
    const QHash<QString, QStringList> &chatMembers;   // conversation id -> MRIs of the other members
};

// Own MRI ("8:orgid:<guid>").
QString selfMri(const SelfContext &ctx);
// "from" is a contact URL ending in the MRI, e.g. ".../contacts/8:orgid:<guid>".
bool isSelf(const SelfContext &ctx, const QString &fromUrl);
// "Donata Gabelloni" (one-to-one), "Donata, Riccardo e Tommaso", "Giovanni, Michele, +2"; empty when no member name is known.
QString titleFromMembers(const SelfContext &ctx, const QString &conversationId, bool oneToOne);
// properties.emotions -> [{key, emoji, count, mine, names}]
QVariantList parseReactions(const SelfContext &ctx, const QJsonObject &properties);
// properties.mentions: is the user mentioned?
bool mentionsSelf(const SelfContext &ctx, const QJsonObject &properties);
// One entry of the conversations list. False for the ones not shown (system, hidden).
// lastFromOther: the last message is a chat message from somebody else.
bool parseConversation(const SelfContext &ctx, const QJsonObject &o, ConversationItem &it, bool &lastFromOther);
// A page of messages (newest first, as the API returns them). oldestRaw: earliest time seen,
// system events included, for paging.
QVector<MessageItem> parseMessages(const SelfContext &ctx, const QJsonArray &arr, qint64 *oldestRaw);

}
