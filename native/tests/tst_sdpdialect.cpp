// SDP translation between webrtcbin and Microsoft's media controller (src/sdpdialect.cpp).
#include "sdpdialect.h"

#include <QtTest>

using namespace SdpDialect;

namespace {
const QString kHostCandidate = QStringLiteral("candidate:1 1 UDP 2015363327 192.168.1.138 37465 typ host");

QString sdp(const QStringList &lines) { return lines.join(QLatin1String("\r\n")) + QLatin1String("\r\n"); }
QStringList linesOf(const QString &s) { return s.split(QLatin1String("\r\n"), Qt::SkipEmptyParts); }

// What webrtcbin writes for an offer with audio and a bundled camera section.
QString webrtcbinOffer()
{
    return sdp({"v=0", "o=- 1937430083206632265 0 IN IP4 0.0.0.0", "s=-", "t=0 0", "a=ice-options:trickle",
                "a=group:BUNDLE audio0 video1",
                "m=audio 9 UDP/TLS/RTP/SAVPF 102", "c=IN IP4 0.0.0.0", "a=setup:actpass", "a=ice-ufrag:abc", "a=ice-pwd:def",
                "a=rtcp-mux", "a=rtcp-mux-only", "a=sendrecv", "a=rtpmap:102 OPUS/48000/2", "a=ssrc:111 cname:u@h", "a=mid:audio0",
                "a=fingerprint:sha-256 AA:BB",
                "m=video 0 UDP/TLS/RTP/SAVPF 107", "c=IN IP4 0.0.0.0", "a=bundle-only", "a=rtcp-mux", "a=sendrecv",
                "a=rtpmap:107 H264/90000", "a=framerate:15", "a=ssrc:222 cname:u@h", "a=mid:video1"});
}
}

class SdpDialectTest : public QObject
{
    Q_OBJECT
private slots:
    // --- webrtcbin -> Teams ---------------------------------------------------------------

    void everySectionGetsTheRealAddressAndPort()
    {
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate}, {}));
        QVERIFY(out.contains("m=audio 37465 RTP/SAVP 102"));
        QVERIFY(out.contains("m=video 37465 RTP/SAVP 107")); // port 0 + bundle-only is "disabled" for Teams
        QCOMPARE(out.count("c=IN IP4 192.168.1.138"), 2);
        QVERIFY(!out.contains("c=IN IP4 0.0.0.0"));
        QCOMPARE(out.count("a=rtcp:37465"), 2);
        QVERIFY(!out.contains("a=bundle-only"));
        QVERIFY(!out.contains("a=rtcp-mux-only"));
        QVERIFY(!out.contains("a=framerate:15"));
    }

    void sessionLevelRewrites()
    {
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate}, {}));
        QVERIFY(out.contains("o=- 1937430083206632265 0 IN IP4 127.0.0.1"));
        const int bandwidth = out.indexOf("b=CT:4000");
        QVERIFY(bandwidth >= 0);
        QCOMPARE(out.value(bandwidth + 1), QStringLiteral("t=0 0")); // b= comes before t=
    }

    void sectionsAreLabelledTheWayTeamsNamesThem()
    {
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate}, {}));
        QVERIFY(out.contains("a=label:main-audio"));
        QVERIFY(out.contains("a=label:main-video"));
        QVERIFY(out.indexOf("a=label:main-video") < out.indexOf("a=mid:video1"));
        QVERIFY(out.contains("a=x-signaling-fb:* x-message app send:src recv:src,vc"));
        const QStringList withScreen = linesOf(toTeams(webrtcbinOffer() + sdp({"m=video 0 UDP/TLS/RTP/SAVPF 108", "c=IN IP4 0.0.0.0", "a=bundle-only", "a=mid:video2"}), {kHostCandidate}, {}));
        QVERIFY(withScreen.contains("a=label:applicationsharing-video")); // the second video section is the screen
    }

    void ssrcRangeIsAnnouncedAfterEachSectionsConnectionLine()
    {
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate}, {}));
        const int audioC = out.indexOf("c=IN IP4 192.168.1.138");
        QCOMPARE(out.value(audioC + 1), QStringLiteral("a=x-ssrc-range:111-111"));
        QVERIFY(out.contains("a=x-ssrc-range:222-222"));
        // Already announced: left alone.
        const QString announced = webrtcbinOffer().replace("a=mid:audio0", "a=x-ssrc-range:111-111\r\na=mid:audio0");
        QCOMPARE(linesOf(toTeams(announced, {kHostCandidate}, {})).count("a=x-ssrc-range:111-111"), 1);
    }

    void theFirstUdpIpv4HostCandidateGivesTheAddress()
    {
        const QStringList candidates{"candidate:2 1 TCP 1015021823 10.0.0.9 9 typ host tcptype active",
                                     "candidate:3 1 UDP 2015364095 fe80::1 45989 typ host",
                                     "candidate:4 1 UDP 1677729535 203.0.113.7 40000 typ srflx raddr 10.0.0.9 rport 5",
                                     "candidate:5 1 UDP 2015363327 10.0.0.9 41000 typ host",
                                     "candidate:6 1 UDP 2015363583 10.0.0.10 42000 typ host"};
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), candidates, {}));
        QVERIFY(out.contains("m=audio 41000 RTP/SAVP 102"));
        QVERIFY(out.contains("c=IN IP4 10.0.0.9"));
    }

    void withoutAHostCandidateTheAddressesStayAsTheyAre()
    {
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {}, {}));
        QVERIFY(out.contains("m=audio 9 RTP/SAVP 102"));
        QVERIFY(out.contains("c=IN IP4 0.0.0.0"));
        QVERIFY(!out.contains("a=rtcp:9"));
        QVERIFY(out.contains("o=- 1937430083206632265 0 IN IP4 127.0.0.1")); // the rest still applies
        QVERIFY(out.contains("b=CT:4000"));
    }

    void candidatesGoBeforeTheSecondMediaSection()
    {
        const QString relay = QStringLiteral("candidate:7 1 UDP 100 192.168.1.138 37465 typ relay raddr 0.0.0.0 rport 0");
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate, relay, QString()}, {}));
        const int second = out.indexOf("m=video 37465 RTP/SAVP 107");
        QVERIFY(out.indexOf("a=" + kHostCandidate) > 0 && out.indexOf("a=" + kHostCandidate) < second);
        QVERIFY(out.indexOf("a=" + relay) > 0 && out.indexOf("a=" + relay) < second);
        QCOMPARE(out.filter("a=candidate:").size(), 2); // the empty one is skipped

        // A single section: at the end.
        const QString audioOnly = sdp({"v=0", "o=- 1 0 IN IP4 0.0.0.0", "s=-", "t=0 0", "m=audio 9 UDP/TLS/RTP/SAVPF 102", "c=IN IP4 0.0.0.0", "a=mid:audio0"});
        QCOMPARE(linesOf(toTeams(audioOnly, {kHostCandidate}, {})).last(), "a=" + kHostCandidate);
    }

    void aCandidateAlreadyInTheSdpIsNotAddedAgain()
    {
        const QString withCandidate = webrtcbinOffer().replace("a=mid:audio0", "a=" + kHostCandidate + "\r\na=mid:audio0");
        QCOMPARE(linesOf(toTeams(withCandidate, {kHostCandidate}, {})).count("a=" + kHostCandidate), 1);
    }

    void rejectedSectionsComeBackInactiveWithPortZero()
    {
        const RejectedSections rejected{{"3", "m=text 5000 RTP/SAVP 99"}};
        const QStringList out = linesOf(toTeams(webrtcbinOffer(), {kHostCandidate}, rejected));
        const int m = out.indexOf("m=text 0 RTP/SAVP 99");
        QVERIFY(m > 0);
        QCOMPARE(out.mid(m + 1, 3), (QStringList{"c=IN IP4 192.168.1.138", "a=mid:3", "a=inactive"}));
    }

    void outputEndsWithACrLf()
    {
        QVERIFY(toTeams(webrtcbinOffer(), {}, {}).endsWith(QLatin1String("\r\n")));
    }

    void aSectionWebrtcbinRejectedStaysRejected()
    {
        // What webrtcbin answered to a Teams renegotiation offer it could not match: the video sections
        // come back with port 0, no bundle-only, and the offer's attributes (remote address included).
        const QString answer = sdp({"v=0", "o=- 287374 1 IN IP4 0.0.0.0", "s=-", "t=0 0", "a=group:BUNDLE audio0",
            "m=audio 9 UDP/TLS/RTP/SAVPF 102", "c=IN IP4 0.0.0.0", "a=setup:active", "a=ice-ufrag:u", "a=ice-pwd:p", "a=rtcp-mux",
            "a=mid:audio0", "a=rtpmap:102 OPUS/48000/2", "a=ssrc:111 cname:u@h", "a=fingerprint:sha-256 AA",
            "m=video 0 UDP/TLS/RTP/SAVPF 114 122 107 123", "c=IN IP4 52.112.227.237", "a=setup:passive", "a=mid:video1",
            "a=rtpmap:114 H265v2/90000", "a=x-ssrc-range:1001-1100", "a=ssrc:1001 cname:teams", "a=sendrecv", "a=fingerprint:sha-256 BB",
            "m=video 0 UDP/TLS/RTP/SAVPF 114 122 107 116 123", "c=IN IP4 52.112.227.237", "a=setup:passive", "a=mid:video2",
            "a=rtpmap:116 AV1/90000"});
        const QStringList out = linesOf(toTeams(answer, {kHostCandidate}, {}));
        QVERIFY(out.contains("m=audio 37465 RTP/SAVP 102")); // the accepted section gets the real port as before
        const int first = out.indexOf("m=video 0 RTP/SAVP 114 122 107 123");
        QVERIFY(first > 0);
        QCOMPARE(out.mid(first + 1, 3), (QStringList{"c=IN IP4 192.168.1.138", "a=mid:video1", "a=inactive"}));
        const int second = out.indexOf("m=video 0 RTP/SAVP 114 122 107 116 123");
        QVERIFY(second > first);
        QCOMPARE(out.mid(second + 1, 3), (QStringList{"c=IN IP4 192.168.1.138", "a=mid:video2", "a=inactive"}));
        // Nothing of what the rejected sections copied from the offer is left.
        const QString text = out.join('\n');
        QVERIFY(!text.contains("52.112.227.237"));
        QVERIFY(!text.contains("H265v2"));
        QVERIFY(!text.contains("a=setup:passive"));
        QVERIFY(!text.contains("x-ssrc-range:1001"));
        QVERIFY(!text.contains("a=sendrecv\n") || out.indexOf("a=sendrecv") < first); // none inside the rejected ones
    }

    void aRejectedSectionKeepsTheLabelCountOfTheVideoSections()
    {
        const QString answer = sdp({"v=0", "o=- 1 1 IN IP4 0.0.0.0", "s=-", "t=0 0",
            "m=audio 9 UDP/TLS/RTP/SAVPF 102", "c=IN IP4 0.0.0.0", "a=mid:audio0",
            "m=video 0 UDP/TLS/RTP/SAVPF 107", "c=IN IP4 1.2.3.4", "a=mid:video1",
            "m=video 9 UDP/TLS/RTP/SAVPF 107", "c=IN IP4 0.0.0.0", "a=mid:video2"});
        const QStringList out = linesOf(toTeams(answer, {kHostCandidate}, {}));
        QVERIFY(out.contains("a=label:applicationsharing-video")); // the second video section is still the screen
        QVERIFY(!out.contains("a=label:main-video")); // the first one was rejected: no label
    }

    // --- Teams -> webrtcbin ---------------------------------------------------------------

    void theSecureProfileBecomesWhatWebrtcbinReads()
    {
        const FromTeams r = fromTeams(sdp({"v=0", "s=-", "t=0 0", "m=audio 3478 RTP/SAVP 102", "c=IN IP4 52.1.2.3", "a=mid:0", "a=rtpmap:102 OPUS/48000/2"}), false, 107, "42e01f");
        QVERIFY(linesOf(r.sdp).contains("m=audio 3478 UDP/TLS/RTP/SAVPF 102"));
        QVERIFY(r.sdp.endsWith(QLatin1String("\r\n")));
        QVERIFY(r.rejected.isEmpty());
    }

    void aMissingDirectionIsSpelledOut()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=mid:0", "m=video 1 RTP/SAVP 107", "a=sendonly", "a=mid:1"}), false, 107, "x").sdp);
        QCOMPARE(out.count("a=sendrecv"), 1); // the audio section; the video one has its own
        QVERIFY(out.indexOf("a=sendrecv") < out.indexOf("m=video 1 UDP/TLS/RTP/SAVPF 107"));
        QVERIFY(out.contains("a=sendonly"));
    }

    void candidatesLibniceCannotParseAreCleaned()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=mid:0",
            "a=candidate:1 1 UDP 100 52.1.2.3 3478 typ relay raddr 0.0.0.0 rport 0 MTURNID abc123",
            "a=candidate:2 1 TCP 90 52.1.2.3 443 typ relay tcp-pass generation 0",
            "a=candidate:3 1 TCP 80 52.1.2.3 443 typ relay tcp-act generation 0"}), false, 107, "x").sdp);
        QVERIFY(out.contains("a=candidate:1 1 UDP 100 52.1.2.3 3478 typ relay raddr 0.0.0.0 rport 0"));
        QCOMPARE(out.filter("a=candidate:").size(), 1);
    }

    void rtxPayloadsAreRemovedEverywhere()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=video 1 RTP/SAVP 107 108 109", "a=mid:1",
            "a=rtpmap:107 H264/90000", "a=rtpmap:108 rtx/90000", "a=fmtp:108 apt=107", "a=rtcp-fb:108 nack",
            "a=rtpmap:109 RTX/90000", "a=fmtp:107 packetization-mode=1", "a=sendrecv"}), false, 107, "x").sdp);
        QVERIFY(out.contains("m=video 1 UDP/TLS/RTP/SAVPF 107"));
        QVERIFY(!out.join("\n").contains("108"));
        QVERIFY(!out.join("\n").contains("109"));
        QVERIFY(out.contains("a=fmtp:107 packetization-mode=1"));
    }

    void simulcastLinesAreDropped()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=video 1 RTP/SAVP 107", "a=mid:1", "a=rid:1 send", "a=simulcast:send 1", "a=sendrecv"}), false, 107, "x").sdp);
        QVERIFY(out.filter("a=rid:").isEmpty());
        QVERIFY(out.filter("a=simulcast:").isEmpty());
    }

    void theProfileIsTheOneWeOfferedWhenItIsAligned_data()
    {
        QTest::addColumn<bool>("align");
        QTest::addColumn<QString>("expected");
        QTest::newRow("aligned: Teams' answer, or its offer while we are the offerer") << true << "a=fmtp:107 packetization-mode=1;profile-level-id=42e01f";
        QTest::newRow("not aligned: we answer, the profile follows the remote one") << false << "a=fmtp:107 packetization-mode=1;profile-level-id=640c1f";
    }
    void theProfileIsTheOneWeOfferedWhenItIsAligned()
    {
        QFETCH(bool, align);
        QFETCH(QString, expected);
        const QString teams = sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=mid:0", "a=fmtp:102 profile-level-id=111111",
                                   "m=video 1 RTP/SAVP 107", "a=mid:1", "a=fmtp:107 packetization-mode=1;profile-level-id=640c1f"});
        const QStringList out = linesOf(fromTeams(teams, align, 107, "42e01f").sdp);
        QVERIFY(out.contains(expected));
        QVERIFY(out.contains("a=fmtp:102 profile-level-id=111111")); // only the video payload is touched
    }

    void sectionsWebrtcbinCannotTakeAreSetAsideAndLeaveTheBundle()
    {
        const FromTeams r = fromTeams(sdp({"v=0", "a=group:BUNDLE 0 1 2", "m=audio 1 RTP/SAVP 102", "a=mid:0",
                                           "m=text 1 RTP/SAVP 99", "a=mid:1", "a=rtpmap:99 t140/1000",
                                           "m=application 1 UDP/DTLS/SCTP webrtc-datachannel", "a=mid:2"}), false, 107, "x");
        QCOMPARE(r.rejected.size(), 1);
        QCOMPARE(r.rejected[0].first, QStringLiteral("1"));
        QCOMPARE(r.rejected[0].second, QStringLiteral("m=text 1 RTP/SAVP 99"));
        const QStringList out = linesOf(r.sdp);
        QVERIFY(out.filter("m=text").isEmpty());
        QVERIFY(!out.contains("a=rtpmap:99 t140/1000"));
        QVERIFY(out.contains("m=application 1 UDP/DTLS/SCTP webrtc-datachannel")); // data channels are kept
        QVERIFY(out.contains("a=group:BUNDLE 0 2"));
    }

    void aBareRejectedSectionBorrowsTheTransportOfTheFirstOne()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=setup:passive", "a=ice-ufrag:u1", "a=ice-pwd:p1",
                                                       "a=fingerprint:sha-256 AA", "a=mid:0",
                                                       "m=video 0 RTP/AVP 107", "a=mid:1", "a=sendrecv"}), false, 107, "x").sdp);
        const int video = out.indexOf("m=video 0 UDP/TLS/RTP/SAVPF 107");
        QVERIFY(video > 0);
        const QStringList section = out.mid(video);
        for (const char *line : {"a=setup:passive", "a=ice-ufrag:u1", "a=ice-pwd:p1", "a=fingerprint:sha-256 AA"})
            QVERIFY2(section.contains(line), line);
    }

    void sectionsThatBringTheirOwnFingerprintAreLeftAlone()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=fingerprint:sha-256 AA", "a=mid:0",
                                                       "m=video 0 RTP/AVP 107", "a=fingerprint:sha-256 BB", "a=mid:1"}), false, 107, "x").sdp);
        QCOMPARE(out.filter("a=fingerprint:").size(), 2);
    }

    void ssrcRangesBecomeSsrcLines()
    {
        const QStringList out = linesOf(fromTeams(sdp({"v=0", "m=audio 1 RTP/SAVP 102", "a=x-ssrc-range:1000-1000", "a=mid:0",
                                                       "m=video 1 RTP/SAVP 107", "a=x-ssrc-range:1001-1003", "a=mid:1"}), false, 107, "x").sdp);
        QVERIFY(out.contains("a=ssrc:1000 cname:teams"));
        QVERIFY(out.contains("a=ssrc:1001 cname:teams"));
        QVERIFY(out.contains("a=ssrc:1003 cname:teams"));
        QVERIFY(out.indexOf("a=ssrc:1000 cname:teams") < out.indexOf("m=video 1 UDP/TLS/RTP/SAVPF 107"));
        QVERIFY(out.indexOf("a=ssrc:1001 cname:teams") > out.indexOf("m=video 1 UDP/TLS/RTP/SAVPF 107"));
    }

    void ssrcRangesAreNotExpandedWhenTooBigOrAlreadySpelledOut()
    {
        const QStringList big = linesOf(fromTeams(sdp({"v=0", "m=video 1 RTP/SAVP 107", "a=x-ssrc-range:1-5000", "a=mid:1"}), false, 107, "x").sdp);
        QVERIFY(big.filter("a=ssrc:").isEmpty());
        const QStringList spelled = linesOf(fromTeams(sdp({"v=0", "m=video 1 RTP/SAVP 107", "a=x-ssrc-range:10-12", "a=ssrc:10 cname:own", "a=mid:1"}), false, 107, "x").sdp);
        QCOMPARE(spelled.filter("a=ssrc:").size(), 1);
    }

    // --- our own sections -----------------------------------------------------------------

    void midOfTheNthVideoSection()
    {
        const QString local = webrtcbinOffer() + sdp({"m=video 0 UDP/TLS/RTP/SAVPF 108", "a=mid:video2"});
        QCOMPARE(mediaMid(local, 1), QStringLiteral("video1"));
        QCOMPARE(mediaMid(local, 2), QStringLiteral("video2"));
        QCOMPARE(mediaMid(local, 3), QStringLiteral("3")); // none: the number
        QCOMPARE(mediaMid(QString(), 1), QStringLiteral("1"));
    }

    void videoStreamIdFromMsidOrFromTheSsrcLine()
    {
        QCOMPARE(localVideoMsid(sdp({"m=audio 9 X 102", "a=msid:audiostream t0", "m=video 9 X 107", "a=msid:videostream t1"})), QStringLiteral("videostream"));
        QCOMPARE(localVideoMsid(sdp({"m=video 9 X 107", "a=ssrc:222 msid:user@host track1", "a=ssrc:222 cname:x"})), QStringLiteral("user@host"));
        QVERIFY(localVideoMsid(sdp({"m=audio 9 X 102", "a=ssrc:111 msid:onlyaudio t0"})).isEmpty());
        QVERIFY(localVideoMsid(QString()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SdpDialectTest)
#include "tst_sdpdialect.moc"
