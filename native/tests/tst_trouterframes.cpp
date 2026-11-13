// Trouter's wire format (src/trouterframes.cpp): frames, gzip bodies, routing of pushes.
#include "trouterframes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <zlib.h>

using namespace TrouterFrames;

namespace {
QByteArray gzip(const QByteArray &data)
{
    z_stream s{};
    deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    s.avail_in = uInt(data.size());
    QByteArray out(int(deflateBound(&s, uLong(data.size()))) + 32, '\0');
    s.next_out = reinterpret_cast<Bytef *>(out.data());
    s.avail_out = uInt(out.size());
    deflate(&s, Z_FINISH);
    out.resize(int(s.total_out));
    deflateEnd(&s);
    return out;
}
QByteArray compact(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QString gzipBase64(const QJsonObject &o) { return QString::fromLatin1(gzip(compact(o)).toBase64()); }
}

class TrouterFramesTest : public QObject
{
    Q_OBJECT
private slots:
    // --- gunzip ---------------------------------------------------------------------------

    void gunzipRoundTrip()
    {
        QCOMPARE(gunzip(gzip("hello trouter")), QByteArray("hello trouter"));
        QCOMPARE(gunzip(gzip(QByteArray())), QByteArray());
    }

    void gunzipBigPayloadsNeedSeveralBuffers()
    {
        QByteArray big;
        for (int i = 0; i < 20000; ++i) big += QByteArray::number(i) + ',';
        QVERIFY(big.size() > 16384 * 2);
        QCOMPARE(gunzip(gzip(big)), big);
    }

    void gunzipRefusesWhatIsNotACompleteGzipStream()
    {
        QVERIFY(gunzip("not gzip at all").isEmpty());
        QVERIFY(gunzip(QByteArray()).isEmpty());
        const QByteArray whole = gzip("some content that compresses to a few bytes");
        QVERIFY(gunzip(whole.left(whole.size() - 6)).isEmpty()); // truncated
    }

    // --- frames ---------------------------------------------------------------------------

    void connectedAndOtherFrames()
    {
        QCOMPARE(parseFrame("1::").kind, Frame::Kind::Connected);
        QCOMPARE(parseFrame("6:::7+[]").kind, Frame::Kind::Other);
        QCOMPARE(parseFrame("2::").kind, Frame::Kind::Other); // heartbeat
        QCOMPARE(parseFrame(QString()).kind, Frame::Kind::Other);
    }

    void requestFramesCarryTheJsonAfterTheThirdColon()
    {
        const Frame f = parseFrame(QStringLiteral("3:::{\"id\":7,\"url\":\"/v4/f/x/messaging\",\"body\":\"a:b:c\"}"));
        QCOMPARE(f.kind, Frame::Kind::Request);
        QCOMPARE(f.payload.value("id").toInt(), 7);
        QCOMPARE(f.payload.value("body").toString(), QStringLiteral("a:b:c")); // later colons belong to the JSON
    }

    void eventFramesWithAnAckId()
    {
        const Frame f = parseFrame(QStringLiteral("5:3+::{\"name\":\"trouter.message_loss\",\"args\":[{\"etag\":1}]}"));
        QCOMPARE(f.kind, Frame::Kind::Event);
        QCOMPARE(f.payload.value("name").toString(), QStringLiteral("trouter.message_loss"));
    }

    void brokenPayloadsGiveAnEmptyObject()
    {
        QVERIFY(parseFrame("3:::{not json").payload.isEmpty());
        QVERIFY(parseFrame("3::").payload.isEmpty());
        QVERIFY(parseFrame("3").payload.isEmpty());
        QCOMPARE(parseFrame("3").kind, Frame::Kind::Request);
    }

    void ackRepeatsTheRequestIdWithStatus200()
    {
        const QString ack = ackFrame({{"id", 42}, {"url", "x"}});
        QVERIFY(ack.startsWith(QStringLiteral("3:::")));
        const QJsonObject response = QJsonDocument::fromJson(ack.mid(4).toUtf8()).object();
        QCOMPARE(response.value("id").toInt(), 42);
        QCOMPARE(response.value("status").toInt(), 200);
        QCOMPARE(response.value("body").toString(), QString());
        QCOMPARE(parseFrame(ack).kind, Frame::Kind::Request);
    }

    // --- request bodies -------------------------------------------------------------------

    void plainBody()
    {
        const Decoded d = decodeRequest({{"body", QString::fromUtf8(compact({{"evt", 5}, {"type", "EventMessage"}}))}});
        QCOMPARE(d.content.value("type").toString(), QStringLiteral("EventMessage"));
        QCOMPARE(d.evt, 5);
        QVERIFY(d.body.contains("EventMessage"));
    }

    void gzipBodyAnnouncedByTheHeader()
    {
        const QJsonObject request{{"body", gzipBase64({{"type", "EventMessage"}})},
                                  {"headers", QJsonObject{{"X-Microsoft-Skype-Content-Encoding", "gzip"}}}};
        QCOMPARE(decodeRequest(request).content.value("type").toString(), QStringLiteral("EventMessage"));
    }

    void gzipBodyRecognisedByItsMagicWithoutTheHeader()
    {
        const QString body = gzipBase64({{"type", "EventMessage"}});
        QVERIFY(body.startsWith(QStringLiteral("H4sI")));
        QCOMPARE(decodeRequest({{"body", body}}).content.value("type").toString(), QStringLiteral("EventMessage"));
    }

    void innerCompressedPayload()
    {
        const QJsonObject outer{{"evt", 107}, {"cp", gzipBase64({{"callNotification", QJsonObject{{"from", QJsonObject{{"id", "8:orgid:x"}}}}}})}};
        const Decoded d = decodeRequest({{"body", QString::fromUtf8(compact(outer))}});
        QCOMPARE(d.evt, 107); // read from the envelope, before the payload replaces it
        QCOMPARE(callerOf(d.content).mri, QStringLiteral("8:orgid:x"));
    }

    void innerPlainPayload()
    {
        const QJsonObject outer{{"gp", QString::fromLatin1(compact({{"presence", QJsonArray{1, 2}}}).toBase64())}};
        const Decoded d = decodeRequest({{"body", QString::fromUtf8(compact(outer))}});
        QCOMPARE(d.content.value("presence").toArray().size(), 2);
    }

    void unreadableBodiesGiveEmptyContent()
    {
        QVERIFY(decodeRequest({{"body", "garbage"}}).content.isEmpty());
        QVERIFY(decodeRequest({{"body", "H4sIgarbage"}}).content.isEmpty());
        QVERIFY(decodeRequest({}).content.isEmpty());
        QVERIFY(decodeRequest({{"body", "{\"cp\":\"@@@\"}"}}).content.isEmpty());
    }

    // --- routing --------------------------------------------------------------------------

    void routes_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<int>("route");
        const auto r = [](Route x) { return int(x); };
        QTest::newRow("call callback") << "https://host/v4/f/abc/callAgent/call/xyz/mediaAnswer" << r(Route::CallAgent);
        QTest::newRow("messaging") << "https://host/v4/f/abc/messaging" << r(Route::Messaging);
        QTest::newRow("call manager") << "https://host/v4/f/abc/NGCallManagerWin" << r(Route::CallManager);
        QTest::newRow("presence service") << "https://host/v4/f/abc/TeamsUnifiedPresenceService" << r(Route::Presence);
        QTest::newRow("presence service, old name") << "https://host/v4/f/abc/unifiedPresenceService" << r(Route::Presence);
        QTest::newRow("unknown") << "https://host/v4/f/abc/somethingElse" << r(Route::Other);
        QTest::newRow("empty") << "" << r(Route::Other);
        QTest::newRow("messaging not at the end") << "https://host/messaging/other" << r(Route::Other);
    }
    void routes()
    {
        QFETCH(QString, url);
        QFETCH(int, route);
        QCOMPARE(int(routeOf(url)), route);
    }

    void callerOfAnIncomingCall()
    {
        const Caller c = callerOf({{"callNotification", QJsonObject{{"from", QJsonObject{{"id", "8:orgid:bob"}, {"displayName", "Bob Smith"}}}}}});
        QCOMPARE(c.mri, QStringLiteral("8:orgid:bob"));
        QCOMPARE(c.name, QStringLiteral("Bob Smith"));
        QVERIFY(callerOf({}).mri.isEmpty());
    }

    void whichCallManagerPushesRing_data()
    {
        QTest::addColumn<int>("evt");
        QTest::addColumn<bool>("rings");
        QTest::newRow("a call") << 107 << true;
        QTest::newRow("a call in a group chat or meeting") << 109 << true;
        QTest::newRow("another event") << 108 << false;
        QTest::newRow("no evt") << 0 << false;
        QTest::newRow("a negative number") << -1 << false;
    }
    void whichCallManagerPushesRing()
    {
        QFETCH(int, evt);
        QFETCH(bool, rings);
        QCOMPARE(isIncomingCallEvent(evt), rings);
    }

    void aGroupCallPushIsDecodedLikeACall()
    {
        // The push seen for a call in a group chat: evt 109 and the fields of an invitation.
        const QJsonObject body{{"evt", 109}, {"callNotification", QJsonObject{{"from", QJsonObject{{"id", "8:orgid:bob"}, {"displayName", "Bob Smith"}}}}},
                               {"conversationInvitation", QJsonObject{}}, {"groupChat", QJsonObject{{"threadId", "19:g@thread.v2"}}},
                               {"groupContext", QJsonObject{}}, {"meetingProperties", QJsonObject{}}};
        const Decoded d = decodeRequest({{"body", QString::fromUtf8(compact(body))}, {"url", "https://x/v4/f/y/NGCallManagerWin"}});
        QVERIFY(isIncomingCallEvent(d.evt));
        QCOMPARE(callerOf(d.content).mri, QStringLiteral("8:orgid:bob"));
        QCOMPARE(callerOf(d.content).name, QStringLiteral("Bob Smith"));
        QCOMPARE(routeOf("https://x/v4/f/y/NGCallManagerWin"), Route::CallManager);
    }

    void messageLossIndicatorsIdentifyRepeats()
    {
        const QJsonObject loss{{"name", "trouter.message_loss"}, {"args", QJsonArray{QJsonObject{{"etag", "7"}}}}};
        QVERIFY(!lossIndicators(loss).isNull());
        QCOMPARE(lossIndicators(loss), lossIndicators(loss));
        QJsonObject other = loss;
        other["args"] = QJsonArray{QJsonObject{{"etag", "8"}}};
        QVERIFY(lossIndicators(loss) != lossIndicators(other));
        QVERIFY(lossIndicators({{"name", "something.else"}}).isNull());
        QVERIFY(!lossIndicators({{"name", "trouter.message_loss"}}).isNull()); // no args: still a report ("[]")
    }
};

QTEST_GUILESS_MAIN(TrouterFramesTest)
#include "tst_trouterframes.moc"
