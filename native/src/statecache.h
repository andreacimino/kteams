#pragma once

#include "models.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

// The local cache's file format: conversations and messages as JSON, so the chat list and
// the last messages appear before the network answers (see tests/tst_statecache.cpp).
namespace StateCache {

QJsonObject conversationToJson(const ConversationItem &c);
ConversationItem conversationFromJson(const QJsonObject &o);
QJsonObject messageToJson(const MessageItem &m);
MessageItem messageFromJson(const QJsonObject &o);
// Name of a conversation's messages file (a hash: conversation ids are not file-name safe).
QString cacheFileName(const QString &conversationId);
// A cache file: {"conversation": id, "messages": [...]} (older files: the bare array).
QVector<MessageItem> readMessagesFile(const QString &path, QString *conversationId = nullptr);

}
