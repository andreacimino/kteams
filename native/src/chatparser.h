#pragma once

#include "models.h"

#include <QDateTime>
#include <QHash>
#include <functional>
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
// The one-to-one chat with a person (their MRI), from the members of every chat; empty when
// there is none. With several, the smallest id, so the answer does not depend on hash order.
QString oneToOneChatWith(const QHash<QString, QStringList> &chatMembers, const std::function<bool(const QString &)> &isOneToOne,
                         const QString &mri);
// A page of messages (newest first, as the API returns them). oldestRaw: earliest time seen,
// system events included, for paging.
QVector<MessageItem> parseMessages(const SelfContext &ctx, const QJsonArray &arr, qint64 *oldestRaw);


// "teams" of the chats-and-teams summary -> channels, archived/deleted ones and those the user
// is not a member of left out; General first in each team, then by name.
QVector<ChannelItem> parseChannels(const QJsonArray &teams);
// Calendar events of the calendar service, sorted by start; entries without a valid start are dropped.
QVector<CalendarEvent> parseCalendar(const QJsonArray &events);
// Entries of the activity feed (mentions, replies, reactions, missed calls...), in feed order.
// `where` names the chat or channel a thread id belongs to ("" when it is a one-to-one chat or unknown).
using WhereResolver = std::function<QString(const QString &threadId)>;
QVector<ActivityItem> parseActivity(const QJsonArray &arr, const WhereResolver &where);
// Milliseconds from `now` until the reminder of an event (`minutesBefore` before it starts),
// or -1 when there should be none: no id, declined, already due, or more than a week away.
qint64 reminderDelayMsecs(const CalendarEvent &ev, const QDateTime &now, int minutesBefore);

}
