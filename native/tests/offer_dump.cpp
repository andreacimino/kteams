// Starts the call engine without any signaling (no TURN, no call) and prints the SDP
// offer it would send, in the Teams dialect; then turns the camera on and off. A check
// that the media pipeline negotiates and describes itself as intended, runnable without
// calling anyone:  QT_FORCE_STDERR_LOGGING=1 ./kollega-offer-test [camera]
#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>
#include "callengine.h"

// Two engines in one process, offerer and answerer, through local ICE: the camera of the
// first must come out of the second as decoded frames ("video in" in its stats).
static int loopback(QCoreApplication &app)
{
    CallEngine a, b;
    QObject::connect(&a, &CallEngine::offerReady, [&](const QString &sdp) { printf("A offer -> B\n"); b.setRemoteOffer(sdp); });
    QObject::connect(&b, &CallEngine::answerReady, [&](const QString &sdp) { printf("B answer -> A\n"); a.setRemoteAnswer(sdp); });
    QObject::connect(&a, &CallEngine::connectionStateChanged, [&](const QString &s) {
        printf("A connection %s\n", qPrintable(s));
        if (s == QStringLiteral("connected")) QTimer::singleShot(500, &app, [&] { a.setCameraEnabled(true); a.startScreenShare(-1, 0); });
    });
    QObject::connect(&b, &CallEngine::connectionStateChanged, [&](const QString &s) { printf("B connection %s\n", qPrintable(s)); });
    QObject::connect(&b, &CallEngine::remoteVideoChanged, [&](bool on) { printf("B remote video %s\n", on ? "STARTED" : "stopped"); });
    QObject::connect(&b, &CallEngine::remoteShareChanged, [&](bool on) { printf("B remote screen %s\n", on ? "STARTED" : "stopped"); });
    for (CallEngine *e : {&a, &b})
        QObject::connect(e, &CallEngine::failed, [&, e](const QString &m) { printf("%s FAILED: %s\n", e == &a ? "A" : "B", qPrintable(m)); app.exit(1); });
    if (!b.start({}, true, 102, 107, QStringLiteral("42e01f"), 107)) return 1;
    if (!a.start({})) return 1;
    QTimer::singleShot(16000, &app, [&] { a.setCameraEnabled(false); a.stopScreenShare(); QTimer::singleShot(4000, &app, &QCoreApplication::quit); });
    return app.exec();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool camera = argc > 1 && QByteArray(argv[1]) == "camera";
    if (argc > 1 && QByteArray(argv[1]) == "loop") return loopback(app);
    CallEngine engine;
    QObject::connect(&engine, &CallEngine::offerReady, [&](const QString &sdp) {
        QString masked = sdp;
        masked.replace(QRegularExpression(QStringLiteral("(ice-pwd:|ice-ufrag:|fingerprint:sha-256 )\\S+")), QStringLiteral("\\1<x>"));
        printf("=== OFFER (Teams dialect) ===\n%s\n=== mid %s msid '%s'\n", qPrintable(masked), qPrintable(engine.videoMid()), qPrintable(engine.localVideoMsid()));
        if (argc > 1 && QByteArray(argv[1]) == "answer") {
            // The media controller's answer to a test call (2026-11-12): audio accepted, video
            // rejected with a bare section. Does webrtcbin accept it and start ICE?
            QString answer;
            bool video = false;
            for (const QString &l : sdp.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts)) {
                if (l.startsWith(QStringLiteral("m=video"))) { video = true; answer += QStringLiteral("m=video 0 RTP/AVP 107\r\na=label:main-video\r\na=sendrecv\r\n"); continue; }
                if (video) continue;
                QString line = l;
                if (line == QStringLiteral("a=setup:actpass")) line = QStringLiteral("a=setup:passive");
                if (line.startsWith(QStringLiteral("a=candidate"))) continue;
                answer += line + QStringLiteral("\r\n");
            }
            printf("=== feeding synthetic answer ===\n");
            engine.setRemoteAnswer(answer);
            QTimer::singleShot(3000, &app, &QCoreApplication::quit);
            return;
        }
        if (!camera) { QTimer::singleShot(100, &app, &QCoreApplication::quit); return; }
        engine.setCameraEnabled(true);
        QTimer::singleShot(6000, &app, [&] { engine.setCameraEnabled(false); QTimer::singleShot(1000, &app, &QCoreApplication::quit); });
    });
    QObject::connect(&engine, &CallEngine::failed, [&](const QString &m) { printf("FAILED: %s\n", qPrintable(m)); app.exit(1); });
    if (!engine.start({})) return 1;
    QTimer::singleShot(15000, &app, [&] { printf("timeout: no offer\n"); app.exit(2); });
    return app.exec();
}
