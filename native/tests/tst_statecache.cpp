// The local cache format (src/statecache.cpp): a change here must still read old files.
#include "statecache.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace StateCache;

class StateCacheTest : public QObject
{
    Q_OBJECT
private slots:
    void conversationRoundTrip()
    {
        ConversationItem c;
        c.id = "19:abc@thread.v2"; c.title = "Team"; c.type = "topic"; c.preview = "Bob: hi";
        c.lastActivity = 1700000000123; c.lastMessageId = "1700000000123"; c.unread = true; c.hasTopic = true;
        c.peerMri = "8:orgid:1"; c.lastFromMri = "8:orgid:2";
        const ConversationItem back = conversationFromJson(conversationToJson(c));
        QCOMPARE(back.id, c.id); QCOMPARE(back.title, c.title); QCOMPARE(back.type, c.type); QCOMPARE(back.preview, c.preview);
        QCOMPARE(back.lastActivity, c.lastActivity); QCOMPARE(back.lastMessageId, c.lastMessageId);
        QCOMPARE(back.unread, c.unread); QCOMPARE(back.hasTopic, c.hasTopic);
        QCOMPARE(back.peerMri, c.peerMri); QCOMPARE(back.lastFromMri, c.lastFromMri);
    }

    void messageRoundTrip()
    {
        MessageItem m;
        m.id = "1700000000123"; m.clientMessageId = "42"; m.from = "Bob"; m.fromId = "8:orgid:2";
        m.content = "hi <there>"; m.richText = "hi &lt;there&gt;"; m.images = {"https://x/1", "https://x/2"};
        m.files = {QVariantMap{{"name", "a.pdf"}, {"url", "https://x/a.pdf"}}};
        m.reactions = {QVariantMap{{"key", "like"}, {"count", 2}}};
        m.time = "2026-11-13T07:00:00.000Z"; m.timestamp = 1700000000123; m.mine = true; m.edited = true; m.mentionsMe = true;
        const MessageItem back = messageFromJson(messageToJson(m));
        QCOMPARE(back.id, m.id); QCOMPARE(back.clientMessageId, m.clientMessageId); QCOMPARE(back.from, m.from);
        QCOMPARE(back.fromId, m.fromId); QCOMPARE(back.content, m.content); QCOMPARE(back.richText, m.richText);
        QCOMPARE(back.images, m.images); QCOMPARE(back.files, m.files); QCOMPARE(back.reactions, m.reactions);
        QCOMPARE(back.time, m.time); QCOMPARE(back.timestamp, m.timestamp);
        QCOMPARE(back.mine, m.mine); QCOMPARE(back.edited, m.edited); QCOMPARE(back.deleted, false); QCOMPARE(back.mentionsMe, m.mentionsMe);
    }

    void cacheFileNameIsStableAndFileSafe()
    {
        const QString name = cacheFileName("19:abc/def@thread.v2");
        QCOMPARE(name, cacheFileName("19:abc/def@thread.v2"));
        QVERIFY(name.endsWith(QLatin1String(".json")));
        QVERIFY(!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char(':')));
        QVERIFY(name != cacheFileName("19:other@thread.v2"));
    }

    void readMessagesFileReadsBothFormats()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MessageItem m; m.id = "7"; m.content = "cached";
        const QJsonArray arr{messageToJson(m)};

        const QString withId = dir.filePath("new.json");
        QFile f(withId);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonObject{{"conversation", "19:x@thread.v2"}, {"messages", arr}}).toJson());
        f.close();
        QString conversationId;
        const QVector<MessageItem> items = readMessagesFile(withId, &conversationId);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().content, QStringLiteral("cached"));
        QCOMPARE(conversationId, QStringLiteral("19:x@thread.v2"));

        const QString bare = dir.filePath("old.json"); // the format before the conversation id was stored
        QFile g(bare);
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(QJsonDocument(arr).toJson());
        g.close();
        conversationId = "stale";
        QCOMPARE(readMessagesFile(bare, &conversationId).size(), 1);
        QCOMPARE(conversationId, QString());

        QVERIFY(readMessagesFile(dir.filePath("missing.json")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(StateCacheTest)
#include "tst_statecache.moc"
