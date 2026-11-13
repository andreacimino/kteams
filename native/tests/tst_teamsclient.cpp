// The chat service client (src/teamsclient.cpp) against a fake HTTP server on loopback:
// what it sends (method, path, query, headers, body) and what it emits for the answers.
#include "fakeserver.h"
#include "teamsclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

namespace {
TeamsTokens bearerTokens(const QString &base)
{
    TeamsTokens t;
    t.chatBearer = "chat-bearer";
    t.skypeToken = "skype-token";
    t.chatServiceBase = base;
    return t;
}
}

class TeamsClientTest : public QObject
{
    Q_OBJECT
    FakeServer server;
    TeamsClient client;

    // Runs `call`, waits for the server to see one more request and, when given, for `spy` to get a signal.
    template <typename Call> const Request &send(Call call, QSignalSpy *spy = nullptr)
    {
        const qsizetype expected = server.requests.size() + 1;
        call();
        if (!server.waitForRequests(expected)) QTest::qFail("the server saw no request", __FILE__, __LINE__);
        if (spy && spy->isEmpty()) spy->wait(5000);
        return server.requests.last();
    }

private slots:
    void initTestCase() { QVERIFY(server.start()); }

    void init()
    {
        server.reset();
        client.setTokens(bearerTokens(server.base()));
        TeamsHosts hosts;
        hosts.teams = hosts.presence = hosts.media = server.base();
        client.setHosts(hosts);
    }

    // --- authentication headers -----------------------------------------------------------

    void bearerTokenWinsAndTheClientIdentifiesAsTeamsWeb()
    {
        QSignalSpy spy(&client, &TeamsClient::selfPropertiesFetched);
        const Request &r = send([&] { client.fetchSelfProperties(); }, &spy);
        QCOMPARE(r.method, QByteArray("GET"));
        QCOMPARE(r.path(), QStringLiteral("/v1/users/ME/properties"));
        QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer chat-bearer"));
        QVERIFY(!r.headers.contains("x-skypetoken"));
        QVERIFY(r.headers.value("user-agent").contains("Teams/"));
        QCOMPARE(r.headers.value("origin"), QByteArray("https://teams.microsoft.com"));
        QCOMPARE(r.headers.value("accept"), QByteArray("application/json; ver=1.0;"));
        QCOMPARE(r.headers.value("x-stratus-caller"), QByteArray("skypeteams"));
        QCOMPARE(spy.count(), 1);
    }

    void skypeTokenIsUsedWhenThereIsNoBearer()
    {
        TeamsTokens t = bearerTokens(server.base());
        t.chatBearer.clear();
        client.setTokens(t);
        QVERIFY(client.canFetch());
        QSignalSpy spy(&client, &TeamsClient::selfPropertiesFetched);
        const Request &r = send([&] { client.fetchSelfProperties(); }, &spy);
        QCOMPARE(r.headers.value("x-skypetoken"), QByteArray("skype-token"));
        QVERIFY(!r.headers.contains("authorization"));
    }

    void noTokenMeansNoRequest()
    {
        client.setTokens({});
        QVERIFY(!client.canFetch());
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        client.fetchConversations();
        client.sendMessage("19:a", "<p>x</p>", "Me");
        QCOMPARE(errors.count(), 2);
        QCOMPARE(errors[0].at(1).toInt(), 0);
        QCOMPARE(errors[0].at(2).toString(), QStringLiteral("missing chat token"));
        QTest::qWait(100);
        QVERIFY(server.requests.isEmpty());
    }

    // --- reading --------------------------------------------------------------------------

    void fetchConversationsFullAndIncremental()
    {
        server.answer = R"({"conversations":[{"id":"19:a"},{"id":"19:b"}]})";
        QSignalSpy spy(&client, &TeamsClient::conversationsFetched);
        const Request &full = send([&] { client.fetchConversations(); }, &spy);
        QCOMPARE(full.path(), QStringLiteral("/v1/users/ME/conversations"));
        QCOMPARE(full.query("startTime"), QStringLiteral("0"));
        QCOMPARE(full.query("pageSize"), QStringLiteral("100"));
        QCOMPARE(full.query("view"), QStringLiteral("msnp24Equivalent"));
        QVERIFY(full.query("targetType").contains(QStringLiteral("Thread")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy[0].at(0).toJsonArray().size(), 2);
        QCOMPARE(spy[0].at(1).toBool(), false);

        spy.clear();
        const Request &incremental = send([&] { client.fetchConversations(1700000000000); }, &spy);
        QCOMPARE(incremental.query("startTime"), QStringLiteral("1700000000000"));
        QCOMPARE(spy[0].at(1).toBool(), true);
    }

    void fetchMessagesEncodesTheConversationIdAndPages()
    {
        server.answer = R"({"messages":[{"id":"1"}]})";
        QSignalSpy spy(&client, &TeamsClient::messagesFetched);
        const Request &newest = send([&] { client.fetchMessages("19:abc@thread.v2"); }, &spy);
        QCOMPARE(newest.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aabc%40thread.v2/messages"));
        QCOMPARE(newest.query("startTime"), QStringLiteral("0"));
        QCOMPARE(newest.query("pageSize"), QString::number(TeamsClient::kHistoryPageSize));
        QVERIFY(newest.query("endTime").isEmpty());
        QCOMPARE(spy[0].at(0).toString(), QStringLiteral("19:abc@thread.v2"));
        QCOMPARE(spy[0].at(1).toJsonArray().size(), 1);
        QCOMPARE(spy[0].at(2).toBool(), false);

        spy.clear();
        const Request &older = send([&] { client.fetchMessages("19:abc@thread.v2", 1700000000000); }, &spy);
        QCOMPARE(older.query("endTime"), QStringLiteral("1700000000000"));
        QCOMPARE(spy[0].at(2).toBool(), true);
    }

    void fetchReadHorizons()
    {
        server.answer = R"({"id":"19:t","consumptionhorizons":[]})";
        QSignalSpy spy(&client, &TeamsClient::readHorizonsFetched);
        const Request &r = send([&] { client.fetchReadHorizons("19:t@thread.v2"); }, &spy);
        QCOMPARE(r.path(), QStringLiteral("/v1/threads/19%3At%40thread.v2/consumptionhorizons"));
        QCOMPARE(spy[0].at(0).toString(), QStringLiteral("19:t@thread.v2"));
        QCOMPARE(spy[0].at(1).toJsonObject().value("id").toString(), QStringLiteral("19:t"));
    }

    void anAnswerThatIsNotJsonGivesEmptyResults()
    {
        server.answer = "<html>gateway</html>";
        QSignalSpy spy(&client, &TeamsClient::conversationsFetched);
        send([&] { client.fetchConversations(); }, &spy);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy[0].at(0).toJsonArray().isEmpty());
    }

    // --- sending --------------------------------------------------------------------------

    void sendMessageBody()
    {
        QSignalSpy sent(&client, &TeamsClient::messageSent);
        const Request &r = send([&] { client.sendMessage("19:abc@thread.v2", "<p>hi</p>", "Andrea", {}, {{"mentions", "[]"}}); }, &sent);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aabc%40thread.v2/messages"));
        QCOMPARE(r.headers.value("content-type"), QByteArray("application/json"));
        const QJsonObject body = r.json();
        QCOMPARE(body.value("content").toString(), QStringLiteral("<p>hi</p>"));
        QCOMPARE(body.value("messagetype").toString(), QStringLiteral("RichText/Html"));
        QCOMPARE(body.value("contenttype").toString(), QStringLiteral("text"));
        QCOMPARE(body.value("imdisplayname").toString(), QStringLiteral("Andrea"));
        QVERIFY(body.value("clientmessageid").toString().toLongLong() > 1700000000000);
        QCOMPARE(body.value("properties").toObject().value("mentions").toString(), QStringLiteral("[]"));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent[0].at(0).toString(), QStringLiteral("19:abc@thread.v2"));
    }

    void aChannelReplyGoesToThePostsThread()
    {
        QSignalSpy sent(&client, &TeamsClient::messageSent);
        const Request &r = send([&] { client.sendMessage("19:chan@thread.tacv2", "<p>re</p>", "Me", "1700000000100"); }, &sent);
        QCOMPARE(r.path(), QStringLiteral("/v1/users/ME/conversations/19%3Achan%40thread.tacv2%3Bmessageid%3D1700000000100/messages"));
        QVERIFY(!r.json().contains("properties")); // none given: none sent
    }

    void editDeleteAndTyping()
    {
        QSignalSpy changed(&client, &TeamsClient::messageChanged);
        const Request &edit = send([&] { client.editMessage("19:a", "55", "<p>new</p>"); }, &changed);
        QCOMPARE(edit.method, QByteArray("PUT"));
        QCOMPARE(edit.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa/messages/55"));
        QCOMPARE(edit.json().value("content").toString(), QStringLiteral("<p>new</p>"));

        changed.clear();
        const Request &del = send([&] { client.deleteMessage("19:a", "55"); }, &changed);
        QCOMPARE(del.method, QByteArray("DELETE"));
        QCOMPARE(del.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa/messages/55"));
        QCOMPARE(changed.count(), 1);

        const Request &typing = send([&] { client.sendTyping("19:a"); });
        QCOMPARE(typing.method, QByteArray("POST"));
        QCOMPARE(typing.json().value("messagetype").toString(), QStringLiteral("Control/Typing"));
        QCOMPARE(typing.json().value("contenttype").toString(), QStringLiteral("Application/Message"));
    }

    void markReadSendsTheConsumptionHorizon()
    {
        const Request &r = send([&] { client.markRead("19:a", "1700000000100", "42"); });
        QCOMPARE(r.method, QByteArray("PUT"));
        QCOMPARE(r.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa/properties"));
        QCOMPARE(r.query("name"), QStringLiteral("consumptionhorizon"));
        const QString horizon = r.json().value("consumptionhorizon").toString();
        QVERIFY2(QRegularExpression(QStringLiteral("^1700000000100;\\d{13};42$")).match(horizon).hasMatch(), qPrintable(horizon));
    }

    // --- reactions ------------------------------------------------------------------------

    void addingAReactionIsAPutWithATimestamp()
    {
        const Request &r = send([&] { client.setReaction("19:a", "55", "like", true); });
        QCOMPARE(r.method, QByteArray("PUT"));
        QCOMPARE(r.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa/messages/55/properties"));
        QCOMPARE(r.query("name"), QStringLiteral("emotions"));
        const QJsonObject emotion = r.json().value("emotions").toObject();
        QCOMPARE(emotion.value("key").toString(), QStringLiteral("like"));
        QVERIFY(emotion.contains("value"));
    }

    void removingAReactionIsADeleteWithoutTimestamp()
    {
        const Request &r = send([&] { client.setReaction("19:a", "55", "heart", false); });
        QCOMPARE(r.method, QByteArray("DELETE"));
        const QJsonObject emotion = r.json().value("emotions").toObject();
        QCOMPARE(emotion.value("key").toString(), QStringLiteral("heart"));
        QVERIFY(!emotion.contains("value"));
    }

    void aFailedReactionIsReported()
    {
        server.status = 500;
        server.answer = "boom";
        QSignalSpy failed(&client, &TeamsClient::reactionFailed);
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        send([&] { client.setReaction("19:a", "55", "like", true); }, &failed);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("19:a"));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors[0].at(1).toInt(), 500);
    }

    void aSuccessfulReactionIsSilent()
    {
        QSignalSpy failed(&client, &TeamsClient::reactionFailed);
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        send([&] { client.setReaction("19:a", "55", "like", true); });
        QTest::qWait(150);
        QCOMPARE(failed.count() + errors.count(), 0);
    }

    // --- errors ---------------------------------------------------------------------------

    void httpErrorsCarryTheStatusAndTheStartOfTheBody()
    {
        server.status = 401;
        server.answer = "token expired " + QByteArray(500, 'x');
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        QSignalSpy fetched(&client, &TeamsClient::conversationsFetched);
        send([&] { client.fetchConversations(); }, &errors);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors[0].at(0).toString(), QStringLiteral("conversazioni"));
        QCOMPARE(errors[0].at(1).toInt(), 401);
        const QString message = errors[0].at(2).toString();
        QVERIFY(message.contains(QStringLiteral("token expired")));
        QVERIFY(message.size() < 600); // the body is cut, not logged whole
        QCOMPARE(fetched.count(), 0);
    }

    void networkErrorsHaveStatusZero()
    {
        FakeServer gone;
        QVERIFY(gone.start());
        const QString base = gone.base();
        gone.stop();
        TeamsClient lonely;
        lonely.setTokens(bearerTokens(base));
        QSignalSpy errors(&lonely, &TeamsClient::errorOccurred);
        lonely.fetchConversations();
        QVERIFY(errors.wait(5000));
        QCOMPARE(errors[0].at(1).toInt(), 0);
    }

    // --- chats and members ----------------------------------------------------------------

    void newOneToOneChatBodyAndIdFromTheAnswer()
    {
        server.answer = R"({"id":"19:new@unq.gbl.spaces"})";
        QSignalSpy created(&client, &TeamsClient::chatCreated);
        const Request &r = send([&] { client.createChat({"8:orgid:bob"}, "8:orgid:me", true); }, &created);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/v1/threads"));
        const QJsonObject body = r.json();
        const QJsonArray members = body.value("members").toArray();
        QCOMPARE(members.size(), 2);
        QCOMPARE(members[0].toObject().value("id").toString(), QStringLiteral("8:orgid:bob"));
        QCOMPARE(members[0].toObject().value("role").toString(), QStringLiteral("User"));
        QCOMPARE(members[1].toObject().value("id").toString(), QStringLiteral("8:orgid:me"));
        QCOMPARE(members[1].toObject().value("role").toString(), QStringLiteral("Admin"));
        const QJsonObject properties = body.value("properties").toObject();
        QCOMPARE(properties.value("threadType").toString(), QStringLiteral("chat"));
        QCOMPARE(properties.value("uniquerosterthread").toString(), QStringLiteral("true"));
        QCOMPARE(properties.value("fixedRoster").toString(), QStringLiteral("true"));
        QCOMPARE(created[0].at(0).toString(), QStringLiteral("19:new@unq.gbl.spaces"));
    }

    void groupChatHasNoFixedRosterAndTheIdCanComeFromTheLocationHeader()
    {
        server.answer = "{}";
        server.extraHeaders = "Location: https://emea.ng.msg.teams.microsoft.com/v1/threads/19%3Agroup%40thread.v2\r\n";
        QSignalSpy created(&client, &TeamsClient::chatCreated);
        const Request &r = send([&] { client.createChat({"8:orgid:a", "8:orgid:b"}, "8:orgid:me", false); }, &created);
        const QJsonObject properties = r.json().value("properties").toObject();
        QVERIFY(!properties.contains("fixedRoster"));
        QVERIFY(!properties.contains("uniquerosterthread"));
        QCOMPARE(r.json().value("members").toArray().size(), 3);
        QCOMPARE(created[0].at(0).toString(), QStringLiteral("19:group@thread.v2"));
    }

    void conversationPropertiesTopicAndMembers()
    {
        const Request &prop = send([&] { client.setConversationProperty("19:a", "favorite", "true"); });
        QCOMPARE(prop.method, QByteArray("PUT"));
        QCOMPARE(prop.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa/properties"));
        QCOMPARE(prop.query("name"), QStringLiteral("favorite"));
        QCOMPARE(prop.json().value("favorite").toString(), QStringLiteral("true"));

        const Request &topic = send([&] { client.setThreadTopic("19:g@thread.v2", "Project Falcon"); });
        QCOMPARE(topic.path(), QStringLiteral("/v1/threads/19%3Ag%40thread.v2/properties"));
        QCOMPARE(topic.query("name"), QStringLiteral("topic"));
        QCOMPARE(topic.json().value("topic").toString(), QStringLiteral("Project Falcon"));

        const Request &add = send([&] { client.addMember("19:g@thread.v2", "8:orgid:bob"); });
        QCOMPARE(add.method, QByteArray("PUT"));
        QCOMPARE(add.path(), QStringLiteral("/v1/threads/19%3Ag%40thread.v2/members/8%3Aorgid%3Abob"));
        QCOMPARE(add.json().value("role").toString(), QStringLiteral("User"));

        const Request &remove = send([&] { client.removeMember("19:g@thread.v2", "8:orgid:bob"); });
        QCOMPARE(remove.method, QByteArray("DELETE"));
        QCOMPARE(remove.path(), QStringLiteral("/v1/threads/19%3Ag%40thread.v2/members/8%3Aorgid%3Abob"));
    }

    // --- sign-in, CSA, profiles, calendar, search -----------------------------------------

    void skypeTokenFromTheAuthService()
    {
        server.answer = R"({"tokens":{"skypeToken":"st","expiresIn":86400},"region":"emea"})";
        QSignalSpy ready(&client, &TeamsClient::skypeTokenReady);
        const Request &r = send([&] { client.fetchSkypeToken("spaces-access"); }, &ready);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/api/authsvc/v1.0/authz"));
        QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer spaces-access"));
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready[0].at(0).toString(), QStringLiteral("st"));
        QCOMPARE(ready[0].at(1).toString(), QStringLiteral("emea"));
        QCOMPARE(ready[0].at(2).toInt(), 86400);
    }

    void skypeTokenInTheOlderAnswerShape()
    {
        server.answer = R"({"skypeToken":{"skypetoken":"st2"},"region":"amer"})";
        QSignalSpy ready(&client, &TeamsClient::skypeTokenReady);
        send([&] { client.fetchSkypeToken("x"); }, &ready);
        QCOMPARE(ready[0].at(0).toString(), QStringLiteral("st2"));
        QCOMPARE(ready[0].at(1).toString(), QStringLiteral("amer"));
    }

    void anAnswerWithoutASkypeTokenIsAnError()
    {
        server.answer = R"({"region":"emea"})";
        QSignalSpy ready(&client, &TeamsClient::skypeTokenReady);
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        send([&] { client.fetchSkypeToken("x"); }, &errors);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(errors[0].at(0).toString(), QStringLiteral("authsvc"));
        QCOMPARE(errors[0].at(2).toString(), QStringLiteral("answer without skypeToken"));
    }

    void chatsAndTeamsNeedBothTokens()
    {
        server.answer = R"({"chats":[],"teams":[{"id":"t"}]})";
        QSignalSpy spy(&client, &TeamsClient::chatsAndTeamsFetched);
        const Request &r = send([&] { client.fetchChatsAndTeams("csa-bearer"); }, &spy);
        QCOMPARE(r.path(), QStringLiteral("/api/csa/api/v1/teams/users/me"));
        QCOMPARE(r.query("enableMembershipSummary"), QStringLiteral("true"));
        QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer csa-bearer"));
        QCOMPARE(r.headers.value("x-skypetoken"), QByteArray("skype-token"));
        QCOMPARE(spy[0].at(0).toJsonObject().value("teams").toArray().size(), 1);
    }

    void profilesAreRequestedInTheRegionAndReadFromEitherKey()
    {
        TeamsTokens t = bearerTokens(server.base());
        t.region = "emea";
        client.setTokens(t);
        server.answer = R"({"value":[{"mri":"8:orgid:a"},{"mri":"8:orgid:b"}]})";
        QSignalSpy spy(&client, &TeamsClient::profilesFetched);
        const Request &r = send([&] { client.fetchProfiles({"8:orgid:a", "8:orgid:b"}, "spaces"); }, &spy);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/api/mt/emea/beta/users/fetchShortProfile"));
        QCOMPARE(r.query("isMailAddress"), QStringLiteral("false"));
        QCOMPARE(r.query("skypeTeamsInfo"), QStringLiteral("true"));
        QCOMPARE(QJsonDocument::fromJson(r.body).array(), (QJsonArray{"8:orgid:a", "8:orgid:b"}));
        QCOMPARE(spy[0].at(0).toJsonArray().size(), 2);

        spy.clear();
        server.answer = R"({"resolvedUsers":[{"mri":"8:orgid:c"}]})";
        send([&] { client.fetchProfiles({"8:orgid:c"}, "spaces"); }, &spy);
        QCOMPARE(spy[0].at(0).toJsonArray().size(), 1);
    }

    void withoutARegionTheDefaultOneIsUsed()
    {
        QSignalSpy spy(&client, &TeamsClient::profilesFetched);
        const Request &r = send([&] { client.fetchProfiles({"8:orgid:a"}, "spaces"); }, &spy);
        QVERIFY(r.path().startsWith(QStringLiteral("/api/mt/apac/")));
    }

    void calendarRangeIsSentWithTheUtcOffsetLikeStrftimeZ()
    {
        TeamsTokens t = bearerTokens(server.base());
        t.region = "emea";
        client.setTokens(t);
        server.answer = R"({"value":[{"subject":"Standup"}]})";
        QSignalSpy spy(&client, &TeamsClient::calendarFetched);
        const QTimeZone plusOne = QTimeZone::fromSecondsAheadOfUtc(3600);
        const QTimeZone minusThreeThirty = QTimeZone::fromSecondsAheadOfUtc(-(3 * 3600 + 30 * 60));
        const Request &r = send([&] {
            client.fetchCalendar(QDateTime(QDate(2026, 11, 9), QTime(0, 0), plusOne), QDateTime(QDate(2026, 11, 16), QTime(0, 0), minusThreeThirty), "spaces");
        }, &spy);
        QCOMPARE(r.method, QByteArray("GET"));
        QCOMPARE(r.path(), QStringLiteral("/api/mt/emea/v2.0/me/calendars/default/calendarView"));
        QCOMPARE(r.query("StartDate"), QStringLiteral("2026-11-09T00:00:00+0100"));
        QCOMPARE(r.query("EndDate"), QStringLiteral("2026-11-16T00:00:00-0330"));
        QCOMPARE(spy[0].at(0).toJsonArray().size(), 1);
    }

    void userSearchPostsTheRawTextAndReadsAnyOfThreeKeys_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("results") << QByteArray(R"({"results":[{"mri":"8:orgid:a"}]})");
        QTest::newRow("value") << QByteArray(R"({"value":[{"mri":"8:orgid:a"}]})");
        QTest::newRow("values") << QByteArray(R"({"values":[{"mri":"8:orgid:a"}]})");
    }
    void userSearchPostsTheRawTextAndReadsAnyOfThreeKeys()
    {
        QFETCH(QByteArray, answer);
        server.answer = answer;
        QSignalSpy spy(&client, &TeamsClient::usersFound);
        const Request &r = send([&] { client.searchUsers("ann", "spaces"); }, &spy);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/api/mt/apac/beta/users/searchV2"));
        QCOMPARE(r.query("source"), QStringLiteral("newChat"));
        QCOMPARE(r.body, QByteArray("ann"));
        QCOMPARE(spy[0].at(0).toString(), QStringLiteral("ann"));
        QCOMPARE(spy[0].at(1).toJsonArray().size(), 1);
    }

    // --- presence -------------------------------------------------------------------------

    void presenceOfSeveralPeople()
    {
        server.answer = R"([{"mri":"8:orgid:a","presence":{"availability":"Available"}}])";
        QSignalSpy spy(&client, &TeamsClient::presenceFetched);
        const Request &r = send([&] { client.fetchPresence({"8:orgid:a", "8:orgid:b"}, "pres"); }, &spy);
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/v1/presence/getpresence/"));
        QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer pres"));
        QCOMPARE(r.headers.value("x-ms-client-user-agent"), QByteArray("Teams-V2-Desktop"));
        const QJsonArray body = QJsonDocument::fromJson(r.body).array();
        QCOMPARE(body.size(), 2);
        QCOMPARE(body[1].toObject().value("mri").toString(), QStringLiteral("8:orgid:b"));
        QCOMPARE(spy[0].at(0).toJsonArray().size(), 1);
    }

    void registeringTheDeviceAsAnEndpoint()
    {
        QSignalSpy registered(&client, &TeamsClient::presenceEndpointRegistered);
        const Request &r = send([&] { client.registerPresenceEndpoint("endpoint-1", "pres"); }, &registered);
        QCOMPARE(r.method, QByteArray("PUT"));
        QCOMPARE(r.path(), QStringLiteral("/v1/me/endpoints/"));
        QCOMPARE(r.json().value("id").toString(), QStringLiteral("endpoint-1"));
        QCOMPARE(r.json().value("availability").toString(), QStringLiteral("Available"));
        QCOMPARE(r.json().value("activityReporting").toString(), QStringLiteral("Transport"));
        QCOMPARE(registered.count(), 1);
    }

    void activityAvailabilityAndNote()
    {
        const Request &activity = send([&] { client.reportActivity("endpoint-1", false, "pres"); });
        QCOMPARE(activity.method, QByteArray("POST"));
        QCOMPARE(activity.path(), QStringLiteral("/v1/me/reportmyactivity/"));
        QCOMPARE(activity.json().value("endpointId").toString(), QStringLiteral("endpoint-1"));
        QCOMPARE(activity.json().value("isActive").toBool(), false);

        const Request &availability = send([&] { client.forceAvailability("DoNotDisturb", "pres"); });
        QCOMPARE(availability.method, QByteArray("PUT"));
        QCOMPARE(availability.path(), QStringLiteral("/v1/me/forceavailability/"));
        QCOMPARE(availability.json().value("availability").toString(), QStringLiteral("DoNotDisturb"));

        const Request &note = send([&] { client.publishNote("in <b>reunione</b> & co", "pres"); });
        QCOMPARE(note.path(), QStringLiteral("/v1/me/publishnote"));
        QCOMPARE(note.json().value("message").toString(), QStringLiteral("in &lt;b&gt;reunione&lt;/b&gt; &amp; co")); // the note is HTML
        QVERIFY(note.json().value("expiry").toString().startsWith(QStringLiteral("9999-")));
    }

    // --- pictures -------------------------------------------------------------------------

    void sendingAPictureCreatesUploadsAndThenPostsTheMessage()
    {
        server.respond = [](const Request &) { return QByteArray(R"({"id":"0-eu-d1-xyz"})"); };
        QSignalSpy busy(&client, &TeamsClient::imageUploading);
        QSignalSpy sent(&client, &TeamsClient::messageSent);
        const QByteArray picture("\x89PNG-bytes", 10);
        client.sendImage("19:a@thread.v2", picture, "pic.png", "Me");
        QVERIFY(server.waitForRequests(3));
        QVERIFY(sent.wait(2000) || sent.count() == 1);

        const Request &create = server.requests[0];
        QCOMPARE(create.method, QByteArray("POST"));
        QCOMPARE(create.path(), QStringLiteral("/v1/objects/"));
        QCOMPARE(create.headers.value("authorization"), QByteArray("skype_token skype-token"));
        QCOMPARE(create.json().value("type").toString(), QStringLiteral("pish/image"));
        QCOMPARE(create.json().value("filename").toString(), QStringLiteral("pic.png"));
        QCOMPARE(create.json().value("permissions").toObject().value("19:a@thread.v2").toArray(), QJsonArray{"read"});

        const Request &upload = server.requests[1];
        QCOMPARE(upload.method, QByteArray("PUT"));
        QCOMPARE(upload.path(), QStringLiteral("/v1/objects/0-eu-d1-xyz/content/imgpsh"));
        QCOMPARE(upload.headers.value("content-type"), QByteArray("application/octet-stream"));
        QCOMPARE(upload.body, picture);

        const Request &message = server.requests[2];
        QCOMPARE(message.path(), QStringLiteral("/v1/users/ME/conversations/19%3Aa%40thread.v2/messages"));
        QVERIFY(message.json().value("content").toString().contains(QStringLiteral("0-eu-d1-xyz/views/imgo")));
        QCOMPARE(busy.count(), 2);
        QCOMPARE(busy[0].at(0).toBool(), true);
        QCOMPARE(busy[1].at(0).toBool(), false);
        QCOMPARE(sent.count(), 1);
    }

    void aFailedPictureCreationStopsThere()
    {
        server.status = 500;
        server.answer = "no";
        QSignalSpy busy(&client, &TeamsClient::imageUploading);
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        client.sendImage("19:a", "x", "pic.png", "Me");
        QVERIFY(errors.wait(5000));
        QCOMPARE(errors[0].at(0).toString(), QStringLiteral("immagine"));
        QCOMPARE(errors[0].at(1).toInt(), 500);
        QCOMPARE(busy.count(), 2);
        QCOMPARE(busy[1].at(0).toBool(), false);
        QTest::qWait(150);
        QCOMPARE(server.requests.size(), 1); // no upload, no message
    }

    void aPictureNeedsTheSkypeToken()
    {
        TeamsTokens t = bearerTokens(server.base());
        t.skypeToken.clear();
        client.setTokens(t);
        QSignalSpy errors(&client, &TeamsClient::errorOccurred);
        client.sendImage("19:a", "x", "pic.png", "Me");
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors[0].at(2).toString(), QStringLiteral("missing skypetoken"));
        QTest::qWait(100);
        QVERIFY(server.requests.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TeamsClientTest)
#include "tst_teamsclient.moc"
