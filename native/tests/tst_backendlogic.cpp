// Logic that used to sit inside Backend: team channels, calendar, activity feed, reminders,
// download file names, reply quotes (src/chatparser.cpp, src/content.cpp).
#include "chatparser.h"
#include "content.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace ChatParser;
using namespace Content;

namespace {
QJsonObject channel(const QString &id, const QString &name, const QJsonObject &extra = {})
{
    QJsonObject c{{"id", id}, {"displayName", name}};
    for (auto it = extra.begin(); it != extra.end(); ++it) c[it.key()] = it.value();
    return c;
}
QJsonObject team(const QString &id, const QString &name, const QJsonArray &channels, const QJsonObject &extra = {})
{
    QJsonObject t{{"id", id}, {"displayName", name}, {"channels", channels}};
    for (auto it = extra.begin(); it != extra.end(); ++it) t[it.key()] = it.value();
    return t;
}
QJsonObject activity(const QString &id, const QString &type, const QString &subtype = {}, const QJsonObject &extra = {})
{
    QJsonObject a{{"activityType", type}, {"activitySubtype", subtype}, {"sourceUserImDisplayName", "Bob Smith"},
                  {"sourceUserId", "8:orgid:bob"}, {"messagePreview", "<p>see   this</p>"}, {"sourceThreadId", "19:t@thread.v2"},
                  {"sourceMessageId", qint64(1700000000100)}, {"activityTimestamp", "2026-11-13T07:00:00.000Z"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) a[it.key()] = it.value();
    return {{"id", id}, {"composetime", "2026-11-13T06:00:00.000Z"},
            {"properties", QJsonObject{{"activity", QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact))}, {"isread", "false"}}}};
}
QJsonObject calEvent(const QString &id, const QString &start, const QJsonObject &extra = {})
{
    QJsonObject e{{"objectId", id}, {"subject", "Standup"}, {"startTime", start}, {"endTime", "2026-11-12T15:00:00+01:00"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
    return e;
}
}

class BackendLogicTest : public QObject
{
    Q_OBJECT
private slots:
    // --- team channels --------------------------------------------------------------------

    void channelsGeneralFirstThenByName()
    {
        const QVector<ChannelItem> items = parseChannels({team("t1", "Falcon", {
            channel("19:z", "Zeta"), channel("19:g", "ignored", {{"isGeneral", true}}), channel("19:a", "Alpha")})});
        QCOMPARE(items.size(), 3);
        QVERIFY(items[0].general);
        QCOMPARE(items[0].id, QStringLiteral("19:g"));
        QCOMPARE(items[1].name, QStringLiteral("Alpha"));
        QCOMPARE(items[2].name, QStringLiteral("Zeta"));
        QCOMPARE(items[1].teamId, QStringLiteral("t1"));
        QCOMPARE(items[1].teamName, QStringLiteral("Falcon"));
    }

    void channelsSkipArchivedDeletedAndForeignOnes()
    {
        const QVector<ChannelItem> items = parseChannels({
            team("old", "Old team", {channel("19:x", "X")}, {{"isArchived", true}}),
            team("gone", "Gone team", {channel("19:y", "Y")}, {{"isDeleted", true}}),
            team("t", "Team", {channel("19:keep", "Keep"),
                               channel("19:del", "Deleted", {{"isDeleted", true}}),
                               channel("19:arch", "Archived", {{"isArchived", true}}),
                               channel("19:foreign", "Private", {{"isMember", false}}),
                               channel("19:member", "Member", {{"isMember", true}}),
                               channel("", "No id")})});
        QStringList ids;
        for (const ChannelItem &c : items) ids << c.id;
        QCOMPARE(ids, QStringList({"19:keep", "19:member"}));
    }

    void channelUnreadAndLastActivity()
    {
        const QVector<ChannelItem> items = parseChannels({team("t", "Team", {
            channel("19:a", "A", {{"isMessageRead", false}, {"lastMessage", QJsonObject{{"composeTime", "2026-11-13T07:00:00.000Z"}}}}),
            channel("19:b", "B", {{"isMessageRead", true}, {"lastMessage", QJsonObject{{"composetime", "2026-11-13T07:00:01.000Z"}}}}),
            channel("19:c", "C")})});
        QVERIFY(items[0].unread);
        QCOMPARE(items[0].lastActivity, qint64(1794553200000));
        QVERIFY(!items[1].unread);
        QCOMPARE(items[1].lastActivity, qint64(1794553201000)); // either spelling of the key
        QVERIFY(!items[2].unread); // unknown read state is not "unread"
        QCOMPARE(items[2].lastActivity, qint64(0));
    }

    // --- calendar -------------------------------------------------------------------------

    void calendarEventsSortedByStartWithAllFields()
    {
        const QJsonObject full = calEvent("e2", "2026-11-12T14:00:00+01:00", {
            {"iCalUID", "uid-2"}, {"location", "Room 4"}, {"organizerName", "Bob Smith"}, {"isOnlineMeeting", true},
            {"skypeTeamsDataObject", QJsonObject{{"cid", "19:meeting@thread.v2"}}}, {"showAs", "Busy"},
            {"skypeTeamsMeetingUrl", "https://teams.microsoft.com/l/meetup-join/x"}, {"myResponseType", "Accepted"}});
        const QVector<CalendarEvent> items = parseCalendar({calEvent("e1", "2026-11-12T16:00:00+01:00"), full});
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].id, QStringLiteral("e2")); // earlier start first
        QCOMPARE(items[0].iCalUid, QStringLiteral("uid-2"));
        QCOMPARE(items[0].location, QStringLiteral("Room 4"));
        QCOMPARE(items[0].organizer, QStringLiteral("Bob Smith"));
        QVERIFY(items[0].online);
        QCOMPARE(items[0].chatId, QStringLiteral("19:meeting@thread.v2"));
        QCOMPARE(items[0].showAs, QStringLiteral("Busy"));
        QCOMPARE(items[0].joinUrl, QStringLiteral("https://teams.microsoft.com/l/meetup-join/x"));
        QCOMPARE(items[0].myResponse, QStringLiteral("Accepted"));
        QCOMPARE(items[0].start.toUTC(), QDateTime(QDate(2026, 11, 12), QTime(13, 0), QTimeZone::UTC)); // the offset is honoured
    }

    void calendarDropsEventsWithoutAValidStartAndFallsBackToTheUidForTheId()
    {
        QJsonObject noId = calEvent("", "2026-11-12T14:00:00+01:00", {{"iCalUID", "only-uid"}});
        noId.remove("objectId");
        const QVector<CalendarEvent> items = parseCalendar({calEvent("bad", "not a date"), calEvent("worse", ""), noId});
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].id, QStringLiteral("only-uid"));
        QVERIFY(parseCalendar({}).isEmpty());
    }

    // --- reminders ------------------------------------------------------------------------

    void reminderDelayCountsDownToFiveMinutesBefore()
    {
        const QDateTime now(QDate(2026, 11, 12), QTime(10, 0), QTimeZone::UTC);
        CalendarEvent ev;
        ev.id = "e"; ev.start = now.addSecs(3600);
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(55 * 60 * 1000));
        ev.start = now.addSecs(5 * 60);
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(0)); // due right now
    }

    void noReminderForPastDeclinedOrDistantEvents()
    {
        const QDateTime now(QDate(2026, 11, 12), QTime(10, 0), QTimeZone::UTC);
        CalendarEvent ev;
        ev.id = "e"; ev.start = now.addSecs(4 * 60);
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(-1)); // the five minutes are already gone
        ev.start = now.addDays(8);
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(-1));
        ev.start = now.addDays(6);
        QVERIFY(reminderDelayMsecs(ev, now, 5) > 0);
        ev.myResponse = "Declined";
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(-1));
        ev.myResponse = "Accepted"; ev.id.clear();
        QCOMPARE(reminderDelayMsecs(ev, now, 5), qint64(-1));
    }

    // --- activity feed --------------------------------------------------------------------

    void activityEntryFieldsAndWords_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("subtype");
        QTest::addColumn<QString>("text");
        QTest::newRow("mention") << "mention" << "" << "mentioned you";
        QTest::newRow("reaction") << "reaction" << "like" << "reacted 👍 to your message";
        QTest::newRow("reply") << "reply" << "" << "replied";
        QTest::newRow("missed call") << "missedCall" << "" << "missed call";
        QTest::newRow("voicemail") << "voicemail" << "" << "voicemail";
        QTest::newRow("unknown type is shown as it is") << "teamMembershipChange" << "" << "teamMembershipChange";
        QTest::newRow("case does not matter") << "AtMention" << "" << "mentioned you";
    }
    void activityEntryFieldsAndWords()
    {
        QFETCH(QString, type);
        QFETCH(QString, subtype);
        QFETCH(QString, text);
        const QVector<ActivityItem> items = parseActivity({activity("a1", type, subtype)}, [](const QString &) { return QString(); });
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].text, text);
        QCOMPARE(items[0].id, QStringLiteral("a1"));
        QCOMPARE(items[0].who, QStringLiteral("Bob Smith"));
        QCOMPARE(items[0].whoMri, QStringLiteral("8:orgid:bob"));
        QCOMPARE(items[0].preview, QStringLiteral("see this")); // HTML removed, spaces collapsed
        QCOMPARE(items[0].threadId, QStringLiteral("19:t@thread.v2"));
        QCOMPARE(items[0].messageId, QStringLiteral("1700000000100")); // a number in the JSON
        QCOMPARE(items[0].timestamp, qint64(1794553200000));
        QVERIFY(!items[0].read);
    }

    void activityWhereComesFromTheResolverAndTimestampFallsBackToComposeTime()
    {
        QStringList asked;
        QJsonObject noTimestamp = activity("a2", "reply");
        QJsonObject props = noTimestamp["properties"].toObject();
        QJsonObject inner = QJsonDocument::fromJson(props["activity"].toString().toUtf8()).object();
        inner.remove("activityTimestamp");
        props["activity"] = QString::fromUtf8(QJsonDocument(inner).toJson(QJsonDocument::Compact));
        noTimestamp["properties"] = props;
        const QVector<ActivityItem> items = parseActivity({noTimestamp}, [&](const QString &thread) { asked << thread; return QStringLiteral("Falcon › General"); });
        QCOMPARE(asked, QStringList{QStringLiteral("19:t@thread.v2")});
        QCOMPARE(items[0].where, QStringLiteral("Falcon › General"));
        QCOMPARE(items[0].timestamp, qint64(1794549600000)); // composetime of the entry, 06:00 UTC
    }

    void activitySkipsEntriesWithoutAnActivityAndReadsObjectsAsWellAsStrings()
    {
        QJsonObject asObject{{"id", "a3"}, {"properties", QJsonObject{{"activity", QJsonObject{{"activityType", "mention"}}}, {"isread", true}}}};
        const QVector<ActivityItem> items = parseActivity({QJsonObject{{"id", "empty"}, {"properties", QJsonObject{}}}, asObject},
                                                          [](const QString &) { return QString(); });
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].id, QStringLiteral("a3"));
        QVERIFY(items[0].read);
    }

    // --- file names of downloads ----------------------------------------------------------

    void uniqueFilePathKeepsTheNameWhenFree()
    {
        QTemporaryDir dir;
        QCOMPARE(uniqueFilePath(dir.path(), "report.pdf"), dir.filePath("report.pdf"));
    }

    void uniqueFilePathNumbersTakenNames()
    {
        QTemporaryDir dir;
        for (const char *name : {"report.pdf", "report (1).pdf", "notes", "archive.tar.gz"}) QFile(dir.filePath(name)).open(QIODevice::WriteOnly);
        QCOMPARE(uniqueFilePath(dir.path(), "report.pdf"), dir.filePath("report (2).pdf"));
        QCOMPARE(uniqueFilePath(dir.path(), "notes"), dir.filePath("notes (1)"));
        QCOMPARE(uniqueFilePath(dir.path(), "archive.tar.gz"), dir.filePath("archive.tar (1).gz"));
    }

    void uniqueFilePathNeverLeavesTheFolder()
    {
        QTemporaryDir dir;
        QCOMPARE(uniqueFilePath(dir.path(), "../../etc/passwd"), dir.filePath("passwd"));
        QCOMPARE(uniqueFilePath(dir.path(), "/etc/cron.d/evil"), dir.filePath("evil"));
        QVERIFY(uniqueFilePath(dir.path(), ".bashrc").startsWith(dir.path() + QLatin1Char('/')));
        QVERIFY(!QFileInfo(uniqueFilePath(dir.path(), ".bashrc")).fileName().startsWith(QLatin1Char('.')));
        QVERIFY(uniqueFilePath(dir.path(), QString()).startsWith(dir.path() + QLatin1Char('/')));
        QVERIFY(uniqueFilePath(dir.path(), "..").startsWith(dir.path() + QLatin1Char('/')));
    }

    // --- reply quote ----------------------------------------------------------------------

    void replyQuoteEscapesEveryUserControlledPart()
    {
        const QString html = replyQuoteHtml("1700000000100", "8:orgid:<bob>", "Bob \"B\" <b>", "<script>alert(1)</script> & more");
        QVERIFY(html.contains(QStringLiteral("itemtype=\"http://schema.skype.com/Reply\"")));
        QVERIFY(html.contains(QStringLiteral("itemid=\"1700000000100\"")));
        QVERIFY(html.contains(QStringLiteral("8:orgid:&lt;bob&gt;")));
        QVERIFY(html.contains(QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt; &amp; more")));
        QVERIFY(!html.contains(QStringLiteral("<script>")));
        QVERIFY(!html.contains(QStringLiteral("<b>")));
        QVERIFY(html.startsWith(QStringLiteral("<blockquote")) && html.endsWith(QStringLiteral("</blockquote>")));
    }
};

QTEST_GUILESS_MAIN(BackendLogicTest)
#include "tst_backendlogic.moc"
