// Teams' calling service JSON (src/calljson.cpp): SDP, media controller links, roster streams.
#include "calljson.h"

#include <QJsonArray>
#include <QtTest>

using namespace CallJson;

namespace {
QJsonObject stream(const QString &type, const QString &label, const QString &direction, const QJsonValue &sourceId)
{
    return {{"type", type}, {"label", label}, {"direction", direction}, {"sourceId", sourceId}};
}
QJsonObject participant(const QJsonArray &streams)
{
    return {{"endpoints", QJsonArray{QJsonObject{{"mediaStreams", streams}}}}};
}
}

class CallJsonTest : public QObject
{
    Q_OBJECT
private slots:
    // --- findSdp --------------------------------------------------------------------------

    void sdpIsFoundWhereverItIs()
    {
        const QString sdp = QStringLiteral("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n");
        QCOMPARE(findSdp(sdp), sdp);
        QCOMPARE(findSdp(QJsonObject{{"mediaContent", QJsonObject{{"blob", sdp}, {"contentType", "application/sdp-ngc-1.0"}}}}), sdp);
        QCOMPARE(findSdp(QJsonObject{{"list", QJsonArray{1, "x", QJsonObject{{"deep", QJsonArray{sdp}}}}}}), sdp);
    }

    void noSdpGivesAnEmptyString()
    {
        QVERIFY(findSdp(QStringLiteral("hello")).isEmpty());
        QVERIFY(findSdp(QStringLiteral("xv=0")).isEmpty()); // must start with it
        QVERIFY(findSdp(QJsonObject{{"a", 1}, {"b", QJsonArray{"nope"}}}).isEmpty());
        QVERIFY(findSdp(QJsonValue()).isEmpty());
        QVERIFY(findSdp(42).isEmpty());
    }

    // --- shape ----------------------------------------------------------------------------

    void shapeShowsFieldNamesAndKindsOnly()
    {
        const QJsonObject o{{"callId", "secret-id"}, {"count", 3}, {"links", QJsonArray{1, 2, 3}}, {"mediaContent", QJsonObject{{"blob", "v=0..."}}}};
        const QString shape_ = shape(o);
        QCOMPARE(shape_, QStringLiteral("{callId:s,count:v,links:[3],mediaContent:{blob:s}}"));
        QVERIFY(!shape_.contains(QStringLiteral("secret")));
    }

    void shapeStopsAfterTwoLevels()
    {
        QCOMPARE(shape(QJsonObject{{"a", QJsonObject{{"b", QJsonObject{{"c", QJsonObject{{"d", 1}}}}}}}}), QStringLiteral("{a:{b:{c:…}}}"));
        QCOMPARE(shape(QJsonValue()), QStringLiteral("v"));
        QCOMPARE(shape(QJsonObject()), QStringLiteral("{}"));
    }

    // --- media controller links -----------------------------------------------------------

    void mediaLinksAreCollectedInDocumentOrder()
    {
        const QJsonObject body{{"mediaAnswer", QJsonObject{{"links", QJsonObject{
                                    {"applyChannelParameters", "https://media.example/apply"},
                                    {"controlVideoStreaming", "https://media.example/control"}}}}},
                               {"other", QJsonArray{QJsonObject{{"mediaRenegotiation", "https://media.example/reneg"}}}}};
        const auto links = mediaControllerLinks(body, "https://our.trouter/");
        QCOMPARE(links.size(), 3);
        QCOMPARE(links[0], qMakePair(QStringLiteral("applyChannelParameters"), QStringLiteral("https://media.example/apply")));
        QCOMPARE(links[1], qMakePair(QStringLiteral("controlVideoStreaming"), QStringLiteral("https://media.example/control")));
        QCOMPARE(links[2], qMakePair(QStringLiteral("mediaRenegotiation"), QStringLiteral("https://media.example/reneg")));
    }

    void ourOwnCallbackLinksAndOddValuesAreSkipped()
    {
        const QJsonObject body{{"clientContentForMediaController", QJsonObject{
                                    {"controlVideoStreaming", "https://our.trouter/callAgent/call/x/controlVideoStreaming"}}},
                               {"insecure", QJsonObject{{"applyChannelParameters", "http://media.example/apply"}}},
                               {"wrongType", QJsonObject{{"mediaRenegotiation", 5}}},
                               {"unknownKey", QJsonObject{{"somethingElse", "https://media.example/else"}}}};
        QVERIFY(mediaControllerLinks(body, "https://our.trouter/").isEmpty());
        QVERIFY(mediaControllerLinks(QJsonValue(), "x").isEmpty());
        QVERIFY(mediaControllerLinks(QStringLiteral("https://x"), "x").isEmpty());
    }

    // --- roster ---------------------------------------------------------------------------

    void rosterStreamsOfTheOtherParticipants()
    {
        const QJsonObject body{{"participants", QJsonObject{
            {"8:orgid:me", participant({stream("video", "main-video", "sendrecv", 1)})},
            {"8:orgid:bob", participant({stream("Audio", "main-audio", "SendRecv", 10),
                                         stream("Video", "main-video", "SendOnly", 11),
                                         stream("video", "applicationsharing-video", "sendonly", "12")})}}}};
        const auto streams = rosterStreams(body, "8:orgid:me");
        QCOMPARE(streams.size(), 3); // own streams left out
        QCOMPARE(streams[0].who, QStringLiteral("8:orgid:bob"));
        QCOMPARE(streams[0].type, QStringLiteral("audio")); // lower-cased
        QCOMPARE(streams[0].direction, QStringLiteral("sendrecv"));
        QCOMPARE(streams[0].sourceId, 10);
        QCOMPARE(streams[1].type, QStringLiteral("video"));
        QCOMPARE(streams[1].direction, QStringLiteral("sendonly"));
        QCOMPARE(streams[1].sourceId, 11);
        QCOMPARE(streams[2].label, QStringLiteral("applicationsharing-video"));
        QCOMPARE(streams[2].sourceId, 12); // a number sent as a string
    }

    void rosterWithoutStreamsOrParticipants()
    {
        QVERIFY(rosterStreams({}, "8:orgid:me").isEmpty());
        QVERIFY(rosterStreams({{"participants", QJsonObject{{"8:orgid:bob", participant({})}}}}, "8:orgid:me").isEmpty());
        const auto streams = rosterStreams({{"participants", QJsonObject{{"8:orgid:bob", participant({QJsonObject()})}}}}, "8:orgid:me");
        QCOMPARE(streams.size(), 1); // an empty stream entry still reads as zeros
        QCOMPARE(streams[0].sourceId, 0);
    }
};

QTEST_GUILESS_MAIN(CallJsonTest)
#include "tst_calljson.moc"
