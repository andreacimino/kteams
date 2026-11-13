// The chat service client (src/teamsclient.cpp) against a fake HTTP server on loopback:
// what it sends (method, path, query, headers, body) and what it emits for the answers.
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
struct Request {
    QByteArray method;
    QByteArray target;       // path and query, as sent
    QByteArray body;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QUrl url() const { return QUrl(QString::fromLatin1(target)); }
    QString path() const { return url().path(QUrl::FullyEncoded); }
    QString query(const QString &name) const { return QUrlQuery(url()).queryItemValue(name, QUrl::FullyDecoded); }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

class FakeServer : public QObject
{
    Q_OBJECT
public:
    FakeServer() { connect(&m_server, &QTcpServer::newConnection, this, &FakeServer::accept); }
    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    void stop() { m_server.close(); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    // What the next answers look like.
    int status = 200;
    QByteArray answer = "{}";
    QByteArray extraHeaders;
    QList<Request> requests;
signals:
    void requestReceived();
private slots:
    void accept()
    {
        QTcpSocket *socket = m_server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
private:
    void read(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        Request r;
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        r.method = first.value(0);
        r.target = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0) r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = r.headers.value("content-length", "0").toInt();
        if (buffer.size() < headerEnd + 4 + length) return;
        r.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        requests.append(r);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(answer.size()) + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + answer);
        socket->disconnectFromHost();
        emit requestReceived();
    }
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

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

    // Runs `call`, waits for the server to see the request and for `signal` (when given).
    template <typename Call> const Request &send(Call call, QSignalSpy *spy = nullptr)
    {
        const qsizetype before = server.requests.size();
        QSignalSpy seen(&server, &FakeServer::requestReceived);
        call();
        if (!seen.wait(5000)) QTest::qFail("the server saw no request", __FILE__, __LINE__);
        if (spy && spy->isEmpty()) spy->wait(5000);
        Q_ASSERT(server.requests.size() == before + 1);
        return server.requests.last();
    }

private slots:
    void initTestCase() { QVERIFY(server.start()); }

    void init()
    {
        server.status = 200;
        server.answer = "{}";
        server.extraHeaders.clear();
        server.requests.clear();
        client.setTokens(bearerTokens(server.base()));
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
};

QTEST_GUILESS_MAIN(TeamsClientTest)
#include "tst_teamsclient.moc"
