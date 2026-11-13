// The list models behind the interface (src/models.cpp): ordering, updates, signals.
#include "models.h"

#include <QSignalSpy>
#include <QtTest>

namespace {
ConversationItem conversation(const QString &id, qint64 lastActivity, bool pinned = false, bool unread = false)
{
    ConversationItem c;
    c.id = id; c.title = id; c.lastActivity = lastActivity; c.pinned = pinned; c.unread = unread;
    return c;
}
MessageItem message(const QString &id, qint64 timestamp, bool mine = false, const QString &content = QStringLiteral("hi"))
{
    MessageItem m;
    m.id = id; m.timestamp = timestamp; m.mine = mine; m.content = content;
    return m;
}
QStringList ids(const QAbstractItemModel &model, int role)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) out << model.index(i, 0).data(role).toString();
    return out;
}
}

class ModelsTest : public QObject
{
    Q_OBJECT
private slots:
    void conversationsSortPinnedFirstThenNewest()
    {
        ConversationModel m;
        m.setItems({conversation("old", 100), conversation("pinned", 50, true), conversation("new", 200)});
        QCOMPARE(ids(m, ConversationModel::IdRole), QStringList({"pinned", "new", "old"}));
        QCOMPARE(m.rowCount(), 3);
        QVERIFY(m.find("old"));
        QVERIFY(!m.find("missing"));
        QVERIFY(!m.data(m.index(7), ConversationModel::IdRole).isValid());
    }

    void conversationUpsertMovesAnExistingRow()
    {
        ConversationModel m;
        m.setItems({conversation("a", 300), conversation("b", 200), conversation("c", 100)});
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.upsert(conversation("c", 400));
        QCOMPARE(ids(m, ConversationModel::IdRole), QStringList({"c", "a", "b"}));
        QCOMPARE(inserted.count(), 1);
        m.upsert(conversation("d", 250));
        QCOMPARE(ids(m, ConversationModel::IdRole), QStringList({"c", "a", "d", "b"}));
    }

    void conversationFlagsPinToTheTopAndMute()
    {
        ConversationModel m;
        m.setItems({conversation("a", 300), conversation("b", 200)});
        m.setFlags("b", true, true);
        QCOMPARE(ids(m, ConversationModel::IdRole), QStringList({"b", "a"}));
        QCOMPARE(m.index(0).data(ConversationModel::PinnedRole).toBool(), true);
        QCOMPARE(m.index(0).data(ConversationModel::MutedRole).toBool(), true);
        m.setFlags("b", true, true); // unchanged: no move
        QCOMPARE(ids(m, ConversationModel::IdRole), QStringList({"b", "a"}));
    }

    void conversationUnreadCountAndSignals()
    {
        ConversationModel m;
        m.setItems({conversation("a", 300), conversation("b", 200, false, true)});
        QCOMPARE(m.unreadCount(), 1);
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.setUnread("a", true);
        QCOMPARE(m.unreadCount(), 2);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().at(2).value<QList<int>>(), QList<int>{ConversationModel::UnreadRole});
        m.setUnread("a", true); // already unread: no signal
        QCOMPARE(changed.count(), 1);
        m.setTitle("a", "Renamed");
        m.setPeer("a", "8:orgid:x");
        QCOMPARE(m.find("a")->title, QStringLiteral("Renamed"));
        QCOMPARE(m.index(0).data(ConversationModel::PeerRole).toString(), QStringLiteral("8:orgid:x"));
        m.remove("a");
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.unreadCount(), 1);
    }

    void conversationFilterByTitleAndUnread()
    {
        ConversationModel m;
        m.setItems({conversation("Alice", 300, false, true), conversation("Albert", 200), conversation("Bob", 100, false, true)});
        ConversationFilter f;
        f.setSourceModel(&m);
        f.setFilterRole(ConversationModel::TitleRole);
        f.setFilterCaseSensitivity(Qt::CaseInsensitive);
        f.setFilterFixedString("al");
        QCOMPARE(f.rowCount(), 2);
        f.setUnreadOnly(true);
        QCOMPARE(ids(f, ConversationModel::IdRole), QStringList({"Alice"}));
        f.setFilterFixedString(QString());
        QCOMPARE(ids(f, ConversationModel::IdRole), QStringList({"Alice", "Bob"}));
        f.setUnreadOnly(false);
        QCOMPARE(f.rowCount(), 3);
    }

    void messagesMergeAddsNewerAndUpdatesShown()
    {
        MessageModel m;
        MessageItem shown = message("200", 200);
        shown.replyCount = 3;
        m.setItems({shown, message("100", 100)});
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        MessageItem edited = message("200", 200, false, "edited");
        edited.edited = true;
        edited.reactions = {QVariantMap{{"key", "like"}}};
        m.merge({message("300", 300), edited, message("50", 50)}); // 50 is older: history paging, not merged
        QCOMPARE(ids(m, MessageModel::IdRole), QStringList({"300", "200", "100"}));
        const MessageItem *row = m.find("200");
        QCOMPARE(row->content, QStringLiteral("edited"));
        QVERIFY(row->edited);
        QCOMPARE(row->replyCount, 3); // kept: replies are counted by the thread, not the page
        QCOMPARE(row->reactions.size(), 1);
        QCOMPARE(changed.count(), 2); // content, then reactions
        m.merge({message("300", 300)}); // nothing new: no change
        QCOMPARE(m.rowCount(), 3);
    }

    void messagesAppendOlderSkipsKnownIds()
    {
        MessageModel m;
        m.setItems({message("200", 200), message("100", 100)});
        m.appendOlder({message("100", 100), message("90", 90), message("80", 80)});
        QCOMPARE(ids(m, MessageModel::IdRole), QStringList({"200", "100", "90", "80"}));
        m.appendOlder({message("80", 80)});
        QCOMPARE(m.rowCount(), 4);
    }

    void messagesSeenLineUnderNewestOwnMessage()
    {
        MessageModel m;
        m.setItems({message("400", 400), message("300", 300, true), message("250", 250, true)});
        m.setReadHorizons({300, 250, 350});
        QCOMPARE(m.find("300")->seenCount, 2); // two members reached id 300
        QCOMPARE(m.find("300")->seenTotal, 3);
        QCOMPARE(m.find("250")->seenCount, -1); // only the newest own message shows it
        QCOMPARE(m.find("400")->seenCount, -1);
        QCOMPARE(m.index(1).data(MessageModel::SeenCountRole).toInt(), 2);
        m.setItems({}); // another conversation: horizons forgotten
        m.setItems({message("1", 1, true)});
        QCOMPARE(m.find("1")->seenTotal, 0);
    }

    void messagesReplyCountAndReactions()
    {
        MessageModel m;
        m.setItems({message("1", 1)});
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.setReplyCount("1", 2);
        m.setReplyCount("1", 2);
        m.setReactions("1", {QVariantMap{{"key", "heart"}}});
        QCOMPARE(changed.count(), 2);
        QCOMPARE(m.index(0).data(MessageModel::ReplyCountRole).toInt(), 2);
        QCOMPARE(m.index(0).data(MessageModel::ReactionsRole).toList().size(), 1);
        QCOMPARE(m.roleNames().value(MessageModel::RichTextRole), QByteArray("richText"));
    }

    void channelsUpdateActivity()
    {
        ChannelModel m;
        ChannelItem general; general.id = "19:g@thread.tacv2"; general.name = "General"; general.general = true;
        ChannelItem dev; dev.id = "19:d@thread.tacv2"; dev.name = "Dev";
        m.setItems({general, dev});
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.updateActivity(dev.id, 500, true);
        m.updateActivity(dev.id, 500, true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(m.find(dev.id)->lastActivity, qint64(500));
        QCOMPARE(m.index(1).data(ChannelModel::UnreadRole).toBool(), true);
        QCOMPARE(m.index(0).data(ChannelModel::GeneralRole).toBool(), true);
        QVERIFY(!m.find("nope"));
    }

    void activitySortsNewestFirstAndMarksRead()
    {
        ActivityModel m;
        ActivityItem a; a.id = "a"; a.timestamp = 100;
        ActivityItem b; b.id = "b"; b.timestamp = 300;
        ActivityItem c; c.id = "c"; c.timestamp = 200; c.read = true;
        m.setItems({a, b, c});
        QCOMPARE(ids(m, ActivityModel::IdRole), QStringList({"b", "c", "a"}));
        QCOMPARE(m.unreadCount(), 2);
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.markAllRead();
        QCOMPARE(m.unreadCount(), 0);
        QCOMPARE(changed.count(), 1);
        m.markAllRead();
        QCOMPARE(changed.count(), 1);
    }

    void calendarAllDayAndTimeRange()
    {
        CalendarModel m;
        CalendarEvent allDay;
        allDay.id = "1"; allDay.iCalUid = "u1";
        allDay.start = QDateTime(QDate(2026, 11, 13), QTime(0, 0));
        allDay.end = QDateTime(QDate(2026, 11, 14), QTime(0, 0));
        CalendarEvent meeting;
        meeting.id = "2"; meeting.iCalUid = "u2";
        meeting.start = QDateTime(QDate(2026, 11, 13), QTime(10, 0));
        meeting.end = QDateTime(QDate(2026, 11, 13), QTime(10, 30));
        m.setItems({allDay, meeting});
        QCOMPARE(m.index(0).data(CalendarModel::AllDayRole).toBool(), true);
        QCOMPARE(m.index(1).data(CalendarModel::AllDayRole).toBool(), false);
        QVERIFY(m.index(1).data(CalendarModel::TimeRangeRole).toString().contains(QStringLiteral(" – ")));
        QVERIFY(m.index(1).data(CalendarModel::DayRole).toString().contains(QStringLiteral("13")));
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        m.setMyResponse("u2", "Accepted");
        QCOMPARE(changed.count(), 1);
        QCOMPARE(m.index(1).data(CalendarModel::MyResponseRole).toString(), QStringLiteral("Accepted"));
        QCOMPARE(m.items().size(), 2);
    }

    void calendarNowRole()
    {
        CalendarModel m;
        CalendarEvent e;
        e.id = "1";
        e.start = QDateTime::currentDateTime().addSecs(-600);
        e.end = QDateTime::currentDateTime().addSecs(600);
        CalendarEvent past = e;
        past.id = "2"; past.start = e.start.addDays(-1); past.end = e.end.addDays(-1);
        m.setItems({e, past});
        QCOMPARE(m.index(0).data(CalendarModel::NowRole).toBool(), true);
        QCOMPARE(m.index(1).data(CalendarModel::NowRole).toBool(), false);
    }
};

QTEST_GUILESS_MAIN(ModelsTest)
#include "tst_models.moc"
