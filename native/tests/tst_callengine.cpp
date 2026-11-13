// The call engine with a real webrtcbin: the renegotiation of a call that ended with
// "SdpParsingFailure". Opt-in: it opens the microphone for a few seconds, so it runs only with
// KTEAMS_TEST_ENGINE=1 on a machine with GStreamer's webrtcbin and a PulseAudio/PipeWire server.
//   KTEAMS_TEST_ENGINE=1 ctest --test-dir native/build -R tst_callengine --output-on-failure
#include "callengine.h"
#include "mediadevices.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <gst/gst.h>

namespace {
QString fixture(const char *name)
{
    QFile f(QStringLiteral(KTEAMS_TEST_DATA_DIR "/") + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
}

class CallEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qgetenv("KTEAMS_TEST_ENGINE") != "1") QSKIP("opt-in: set KTEAMS_TEST_ENGINE=1 (opens the microphone)");
        gst_init(nullptr, nullptr);
        GstElementFactory *webrtcbin = gst_element_factory_find("webrtcbin");
        if (!webrtcbin) QSKIP("webrtcbin is not installed");
        gst_object_unref(webrtcbin);
        if (MediaDevices::audioInputs().isEmpty()) QSKIP("no PulseAudio/PipeWire server with a microphone");
    }

    // We are the offerer (an outgoing call); Teams answers, then renegotiates with an offer of its
    // own that lists its H.264 profile (42C02A), not ours (42e01f). webrtcbin used to reject both
    // video sections, and the answer made them "active" again: Teams ended the call.
    void theRenegotiationOfAnOutgoingCallIsAnsweredWithTheVideoSectionsAccepted()
    {
        const QString teamsAnswer = fixture("renegotiation-teams-answer.sdp"), teamsOffer = fixture("renegotiation-teams-offer.sdp");
        QVERIFY(!teamsAnswer.isEmpty() && !teamsOffer.isEmpty());
        CallEngine engine;
        QSignalSpy failed(&engine, &CallEngine::failed);
        QString answer;
        bool offerSeen = false;
        connect(&engine, &CallEngine::offerReady, &engine, [&](const QString &) {
            if (offerSeen) return;
            offerSeen = true;
            engine.setRemoteAnswer(teamsAnswer);
            QTimer::singleShot(1500, &engine, [&] { engine.setRemoteOffer(teamsOffer); });
        });
        connect(&engine, &CallEngine::answerReady, &engine, [&](const QString &sdp) { answer = sdp; });
        QVERIFY(engine.start({}, false, 102, 107, QStringLiteral("42e01f"), 107));
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty(), 25000);
        QCOMPARE(failed.count(), 0);

        const QStringList lines = answer.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        QStringList mLines = lines.filter(QRegularExpression("^m="));
        QCOMPARE(mLines.size(), 3);
        for (const QString &m : std::as_const(mLines)) QVERIFY2(!m.contains(QRegularExpression("^m=\\w+ 0 ")), qPrintable("rejected: " + m));
        QVERIFY(mLines[1].endsWith(" 107"));
        QVERIFY(mLines[2].endsWith(" 107"));
        QVERIFY(lines.contains("a=group:BUNDLE audio0 video1 video2"));
        QVERIFY(lines.filter("profile-level-id=42e01f").size() >= 2); // ours, in both video sections
        QVERIFY(lines.filter("52.112.227.237").isEmpty()); // Teams' own address is never ours in an answer
        QVERIFY(lines.filter("a=setup:passive").isEmpty()); // we answer as the active DTLS side
        QVERIFY(lines.filter("a=setup:active").size() >= 3);
    }
    // We are the answerer (Tommaso called us): Teams offers its video sections receive-only for
    // its side ("recvonly") and the screen section inactive. Our answer must say we send on the
    // camera section, or the camera's packets never leave (seen 2026-11-13: "video out 1 pkts").
    void theAnswerToAnIncomingCallLetsUsSendTheCamera()
    {
        const QString teamsOffer = fixture("incoming-teams-offer.sdp");
        QVERIFY(!teamsOffer.isEmpty());
        CallEngine engine;
        QString answer;
        connect(&engine, &CallEngine::answerReady, &engine, [&](const QString &sdp) { answer = sdp; });
        QVERIFY(engine.start({}, true, 111, 102, QStringLiteral("42001f"), 102));
        engine.setRemoteOffer(teamsOffer);
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty(), 25000);

        // The direction line of each media section, in order.
        QStringList directions;
        for (const QString &part : answer.split(QStringLiteral("\r\nm="), Qt::SkipEmptyParts)) {
            static const QRegularExpression direction(QStringLiteral("\\r\\na=(sendrecv|sendonly|recvonly|inactive)\\r"));
            directions << (direction.match(part + QStringLiteral("\r")).hasMatch() ? direction.match(part + QStringLiteral("\r")).captured(1) : QStringLiteral("none"));
        }
        qInfo() << "answer directions:" << directions;
        QVERIFY(directions.size() >= 4); // the header, audio, camera, screen
        QVERIFY2(directions[2] == QLatin1String("sendonly") || directions[2] == QLatin1String("sendrecv"),
                 qPrintable("camera section answered " + directions[2]));
    }
};

QTEST_GUILESS_MAIN(CallEngineTest)
#include "tst_callengine.moc"
