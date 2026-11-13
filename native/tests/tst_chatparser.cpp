// Teams chat service JSON -> conversations, messages, reactions (src/chatparser.cpp).
#include "chatparser.h"

#include <QJsonDocument>
#include <QtTest>

using namespace ChatParser;

namespace {
const QString kSelf = QStringLiteral("orgid:00000000-self");
const QString kBob = QStringLiteral("8:orgid:bob");
const QString kAlice = QStringLiteral("8:orgid:alice");
const QString kCarol = QStringLiteral("8:orgid:carol");
const QString kDave = QStringLiteral("8:orgid:dave");

QJsonObject obj(const QByteArray &json) { return QJsonDocument::fromJson(json).object(); }
// Teams sends properties.files, .emotions and .mentions as a JSON array encoded in a string.
QString encoded(const QJsonArray &a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); }
const QString kImg = QStringLiteral("<img src=\"https://eu-api.asm.skype.com/v1/objects/0-x/views/imgo\">");
QJsonObject message(const QString &id, const QString &type, const QString &content, const QJsonObject &props = {}, const QString &fromMri = QStringLiteral("8:orgid:bob"))
{
    return {{"id", id}, {"messagetype", type}, {"content", content}, {"from", "x/" + fromMri}, {"properties", props}};
}
QString from(const QString &mri) { return QStringLiteral("https://emea.ng.msg.teams.microsoft.com/v1/users/ME/contacts/") + mri; }

QJsonObject lastMessage(const QString &fromMri, const QString &type, const QString &content, const QString &id = QStringLiteral("1700000000200"))
{
    return {{"id", id}, {"from", from(fromMri)}, {"messagetype", type}, {"content", content},
            {"imdisplayname", "Bob Smith"}, {"composetime", "2026-11-13T07:00:00.000Z"}};
}
QJsonObject conversation(const QString &id, const QJsonObject &last, const QJsonObject &props = {}, const QJsonObject &thread = {})
{
    return {{"id", id}, {"threadProperties", thread}, {"lastMessage", last}, {"properties", props}};
}
}

class ChatParserTest : public QObject
{
    Q_OBJECT
    QHash<QString, Person> people;
    QHash<QString, QStringList> members;

    SelfContext ctx() const { return {kSelf, people, members}; }

private slots:
    void init()
    {
        people = {{kBob, {"Bob Smith", "Bob", "", "", ""}}, {kAlice, {"Alice Jones", "", "", "", ""}},
                  {kCarol, {"Carol Wu", "Carol", "", "", ""}}, {kDave, {"Dave Lee", "Dave", "", "", ""}}};
        members = {{"19:one@unq.gbl.spaces", {kBob}},
                   {"19:three@thread.v2", {kAlice, kBob, kCarol}},
                   {"19:four@thread.v2", {kAlice, kBob, kCarol, kDave}},
                   {"19:strangers@thread.v2", {"8:orgid:nobody"}}};
    }

    // --- identity -------------------------------------------------------------------------

    void selfIsRecognisedByTheEndOfTheContactUrl()
    {
        QCOMPARE(selfMri(ctx()), QStringLiteral("8:orgid:00000000-self"));
        QVERIFY(isSelf(ctx(), from("8:" + kSelf)));
        QVERIFY(!isSelf(ctx(), from(kBob)));
        const QHash<QString, Person> none;
        const QHash<QString, QStringList> noMembers;
        QVERIFY(!isSelf({QString(), none, noMembers}, from("8:" + kSelf))); // not signed in yet: nobody is "me"
    }

    // --- titles from members --------------------------------------------------------------

    void oneToOneTitleIsTheFullName()
    {
        QCOMPARE(titleFromMembers(ctx(), "19:one@unq.gbl.spaces", true), QStringLiteral("Bob Smith"));
    }

    void groupTitleListsGivenNamesAndFallsBackToTheFirstWord()
    {
        const QString title = titleFromMembers(ctx(), "19:three@thread.v2", false);
        QVERIFY2(title.startsWith(QStringLiteral("Alice, Bob")), qPrintable(title)); // Alice has no givenName: first word
        QVERIFY2(title.endsWith(QStringLiteral("Carol")), qPrintable(title));
    }

    void bigGroupTitleShowsTwoNamesAndTheRest()
    {
        QCOMPARE(titleFromMembers(ctx(), "19:four@thread.v2", false), QStringLiteral("Alice, Bob, +2"));
    }

    void titleIsEmptyWhenNoNameIsKnown()
    {
        QVERIFY(titleFromMembers(ctx(), "19:strangers@thread.v2", false).isEmpty());
        QVERIFY(titleFromMembers(ctx(), "19:unknown@thread.v2", true).isEmpty());
    }

    // --- conversations --------------------------------------------------------------------

    void oneToOneConversationIsUnreadWhenTheLastMessageIsPastTheHorizon()
    {
        const QJsonObject thread{{"productThreadType", "OneToOneChat"}};
        const QJsonObject unreadProps{{"consumptionhorizon", "1700000000100;1700000000100;55"}};
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(parseConversation(ctx(), conversation("19:one@unq.gbl.spaces", lastMessage(kBob, "RichText/Html", "<p>hello &amp; bye</p>"),
                                                      unreadProps, thread), it, fromOther));
        QCOMPARE(it.type, QStringLiteral("oneToOne"));
        QCOMPARE(it.title, QStringLiteral("Bob Smith"));
        QCOMPARE(it.peerMri, kBob);
        QCOMPARE(it.preview, QStringLiteral("hello & bye")); // no "Name: " prefix in a one-to-one
        QCOMPARE(it.lastMessageId, QStringLiteral("1700000000200"));
        QCOMPARE(it.lastFromMri, kBob);
        QCOMPARE(it.lastActivity, qint64(1794553200000)); // 2026-11-13T07:00:00Z
        QVERIFY(fromOther);
        QVERIFY(it.unread);

        const QJsonObject readProps{{"consumptionhorizon", "1700000000200;1700000000200;55"}};
        QVERIFY(parseConversation(ctx(), conversation("19:one@unq.gbl.spaces", lastMessage(kBob, "Text", "x"), readProps, thread), it, fromOther));
        QVERIFY(!it.unread);
    }

    void ownLastMessageIsNeverUnread()
    {
        ConversationItem it;
        bool fromOther = true;
        const QJsonObject props{{"consumptionhorizon", "1;1;1"}};
        QVERIFY(parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage("8:" + kSelf, "Text", "mine"), props), it, fromOther));
        QVERIFY(!fromOther);
        QVERIFY(!it.unread);
    }

    void groupConversationPreviewNamesTheSender()
    {
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage(kBob, "Text", "  see   you  "),
                                                      {}, {{"threadType", "chat"}}), it, fromOther));
        QCOMPARE(it.type, QStringLiteral("chat"));
        QCOMPARE(it.preview, QStringLiteral("Bob Smith: see you")); // whitespace collapsed
        QVERIFY(!it.hasTopic);
        QVERIFY(it.title.endsWith(QStringLiteral("Carol")));
    }

    void topicWinsOverMemberNames()
    {
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage(kBob, "Text", "x"), {}, {{"topic", "Project Falcon"}}), it, fromOther));
        QCOMPARE(it.title, QStringLiteral("Project Falcon"));
        QVERIFY(it.hasTopic);
    }

    void previewShowsAFileOrAPictureWhenThereIsNoText()
    {
        ConversationItem it;
        bool fromOther = false;
        QJsonObject withFile = lastMessage(kBob, "RichText/Html", "");
        withFile["properties"] = QJsonObject{{"files", "[{\"fileName\":\"plan.pdf\",\"objectUrl\":\"https://x/plan.pdf\"}]"}};
        QVERIFY(parseConversation(ctx(), conversation("19:one@unq.gbl.spaces", withFile, {}, {{"uniquerosterthread", "true"}}), it, fromOther));
        QCOMPARE(it.preview, QStringLiteral("📎 plan.pdf"));

        QVERIFY(parseConversation(ctx(), conversation("19:one@unq.gbl.spaces",
                    lastMessage(kBob, "RichText/Html", "<img src=\"https://eu-api.asm.skype.com/v1/objects/0-x/views/imgo\">"),
                    {}, {{"uniquerosterthread", "true"}}), it, fromOther));
        QVERIFY(it.preview.contains(QStringLiteral("Picture")));
    }

    void callEventsAreNotPreviewedNorUnread()
    {
        ConversationItem it;
        bool fromOther = true;
        const QJsonObject props{{"consumptionhorizon", "1;1;1"}};
        QVERIFY(parseConversation(ctx(), conversation("19:one@unq.gbl.spaces", lastMessage(kBob, "Event/Call", "<partlist/>"), props,
                                                      {{"uniquerosterthread", "true"}}), it, fromOther));
        QVERIFY(!fromOther);
        QVERIFY(it.preview.isEmpty());
        QVERIFY(!it.unread);
    }

    void pinnedMutedAndHiddenConversations()
    {
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage(kBob, "Text", "x"),
                                                      {{"favorite", "true"}, {"alerts", "false"}}), it, fromOther));
        QVERIFY(it.pinned);
        QVERIFY(it.muted);
        QVERIFY(!parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage(kBob, "Text", "x"), {{"hidden", "true"}}), it, fromOther));
        QVERIFY(parseConversation(ctx(), conversation("19:three@thread.v2", lastMessage(kBob, "Text", "x"), {{"hidden", "false"}}), it, fromOther));
        QVERIFY(!it.pinned && !it.muted);
    }

    void systemConversationsAreSkippedExceptPersonalNotes()
    {
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(!parseConversation(ctx(), conversation("48:notifications", lastMessage(kBob, "Text", "x")), it, fromOther));
        QVERIFY(!parseConversation(ctx(), conversation("48:calllogs", lastMessage(kBob, "Text", "x")), it, fromOther));
        QVERIFY(!parseConversation(ctx(), conversation("", lastMessage(kBob, "Text", "x")), it, fromOther));
        QVERIFY(parseConversation(ctx(), conversation("48:notes", lastMessage("8:" + kSelf, "Text", "todo")), it, fromOther));
        QVERIFY(!it.title.isEmpty());
    }

    void untitledOneToOneBorrowsTheSendersNameAndUntitledGroupGetsADefault()
    {
        ConversationItem it;
        bool fromOther = false;
        QVERIFY(parseConversation(ctx(), conversation("19:stranger@unq.gbl.spaces", lastMessage("8:orgid:stranger", "Text", "yo"),
                                                      {}, {{"uniquerosterthread", "true"}}), it, fromOther));
        QCOMPARE(it.title, QStringLiteral("Bob Smith")); // imdisplayname of the last message
        QVERIFY(parseConversation(ctx(), conversation("19:nobody@thread.v2", lastMessage(kBob, "Text", "x")), it, fromOther));
        QCOMPARE(it.title, QStringLiteral("Group chat"));
    }

    // --- messages -------------------------------------------------------------------------

    void messagesKeepOnlyChatMessagesWithContent()
    {
        QJsonArray page{
            obj(R"({"id":"300","messagetype":"Text","content":"hello","from":"x/8:orgid:bob","imdisplayname":"Bob Smith","composetime":"2026-11-13T07:00:03.000Z"})"),
            obj(R"({"id":"250","messagetype":"Event/Call","content":"<partlist/>","from":"x/8:orgid:bob","composetime":"2026-11-13T07:00:02.000Z"})"),
            obj(R"({"id":"200","messagetype":"RichText/Html","content":"","from":"x/8:orgid:bob","composetime":"2026-11-13T07:00:01.000Z"})"),
            obj(R"({"id":"100","messagetype":"RichText/Html","content":"<p>old <b>one</b></p>","from":"x/8:orgid:bob","composetime":"2026-11-13T07:00:00.000Z"})")};
        qint64 oldest = 0;
        const QVector<MessageItem> items = parseMessages(ctx(), page, &oldest);
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].id, QStringLiteral("300"));
        QCOMPARE(items[0].content, QStringLiteral("hello"));
        QCOMPARE(items[0].richText, QStringLiteral("hello"));
        QCOMPARE(items[1].content, QStringLiteral("old one"));
        QVERIFY(items[1].richText.contains(QStringLiteral("<b>one</b>")));
        QCOMPARE(oldest, qint64(1794553200000)); // paging uses every message, system events included
    }

    void messageSenderOwnershipAndFallbackName()
    {
        QJsonArray page{
            obj(R"({"id":"2","messagetype":"Text","content":"mine","from":"x/8:orgid:00000000-self","imdisplayname":"Me"})"),
            obj(R"({"id":"1","messagetype":"Text","content":"anon","from":"x/8:orgid:ghost"})")};
        const QVector<MessageItem> items = parseMessages(ctx(), page, nullptr);
        QVERIFY(items[0].mine);
        QCOMPARE(items[0].fromId, QStringLiteral("8:orgid:00000000-self"));
        QVERIFY(!items[1].mine);
        QCOMPARE(items[1].from, QStringLiteral("8:orgid:ghost")); // no display name: the MRI
    }

    void plainTextMessagesAreLinkified()
    {
        const QVector<MessageItem> items = parseMessages(ctx(), {message("1", "Text", "see https://example.com <now>")}, nullptr);
        QCOMPARE(items.first().content, QStringLiteral("see https://example.com <now>"));
        QCOMPARE(items.first().richText, QStringLiteral("see <a href=\"https://example.com\">https://example.com</a> &lt;now&gt;"));
    }

    void editedAndDeletedMessages()
    {
        const QJsonObject file{{"fileName", "a.pdf"}, {"objectUrl", "https://x/a"}};
        QJsonObject edited = message("1", "RichText/Html", "<p>fixed too</p>");
        edited["skypeeditedid"] = "9";
        QJsonArray page{message("3", "RichText/Html", "<p>gone</p>", {{"deletetime", "1700"}, {"files", encoded({file})}}),
                        message("2", "RichText/Html", "<p>fixed</p>", {{"edittime", "1700"}}),
                        edited};
        const QVector<MessageItem> items = parseMessages(ctx(), page, nullptr);
        QCOMPARE(items.size(), 3);
        QVERIFY(items[0].deleted && !items[0].edited);
        QVERIFY(items[0].files.isEmpty());
        QVERIFY(items[0].content.contains(QStringLiteral("deleted")));
        QVERIFY(items[1].edited && !items[1].deleted);
        QVERIFY(items[2].edited);
    }

    void messageMentionsReactionsFilesAndPictures()
    {
        const QJsonObject props{
            {"mentions", encoded({QJsonObject{{"mri", "8:" + kSelf}, {"mentionType", "person"}}})},
            {"emotions", encoded({QJsonObject{{"key", "like"}, {"users", QJsonArray{QJsonObject{{"mri", kBob}}}}}})},
            {"files", encoded({QJsonObject{{"fileName", "a.pdf"}, {"fileType", "pdf"}, {"objectUrl", "https://x/a.pdf"}}})}};
        const MessageItem m = parseMessages(ctx(), {message("1", "RichText/Html", "<p>look</p>" + kImg, props)}, nullptr).first();
        QVERIFY(m.mentionsMe);
        QCOMPARE(m.reactions.size(), 1);
        QCOMPARE(m.files.size(), 1);
        QCOMPARE(m.images, QStringList{QStringLiteral("https://eu-api.asm.skype.com/v1/objects/0-x/views/imgo")});
    }

    void pictureOnlyMessageIsKept()
    {
        const QVector<MessageItem> items = parseMessages(ctx(), {message("1", "RichText/Html", kImg)}, nullptr);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().images.size(), 1);
    }

    // --- reactions and mentions -----------------------------------------------------------

    void reactionsCountUsersAndMarkMine()
    {
        const QJsonObject props{{"emotions", QJsonArray{
            obj(R"({"key":"like","users":[{"mri":"8:orgid:bob"},{"mri":"8:orgid:00000000-self"},{"mri":"8:orgid:stranger"}]})"),
            obj(R"({"key":"heart","users":[]})"),
            obj(R"({"key":"1f602_facewithtearsofjoy","users":[{"mri":"8:orgid:alice"}]})")}}};
        const QVariantList reactions = parseReactions(ctx(), props);
        QCOMPARE(reactions.size(), 2); // the empty one is dropped
        const QVariantMap like = reactions[0].toMap();
        QCOMPARE(like["key"].toString(), QStringLiteral("like"));
        QCOMPARE(like["emoji"].toString(), QStringLiteral("👍"));
        QCOMPARE(like["count"].toInt(), 3);
        QCOMPARE(like["mine"].toBool(), true);
        QVERIFY(like["names"].toString().contains(QStringLiteral("Bob Smith")));
        QVERIFY(like["names"].toString().contains(QStringLiteral("Someone"))); // unknown person
        const QVariantMap laugh = reactions[1].toMap();
        QCOMPARE(laugh["emoji"].toString(), QStringLiteral("😂"));
        QCOMPARE(laugh["mine"].toBool(), false);
        QCOMPARE(laugh["names"].toString(), QStringLiteral("Alice Jones"));
        QVERIFY(parseReactions(ctx(), {}).isEmpty());
    }

    void mentionsAcceptAnArrayOrAStringEncodedArray()
    {
        const QString me = QStringLiteral("8:orgid:00000000-self");
        QVERIFY(mentionsSelf(ctx(), {{"mentions", QJsonArray{QJsonObject{{"mri", me}}}}}));
        QVERIFY(mentionsSelf(ctx(), {{"mentions", QStringLiteral("[{\"mri\":\"%1\"}]").arg(me)}}));
        QVERIFY(!mentionsSelf(ctx(), {{"mentions", QStringLiteral("[{\"mri\":\"8:orgid:bob\"}]")}}));
        QVERIFY(!mentionsSelf(ctx(), {}));
    }
};

QTEST_GUILESS_MAIN(ChatParserTest)
#include "tst_chatparser.moc"
