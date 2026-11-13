#include "sdpdialect.h"

#include <QRegularExpression>

namespace SdpDialect {

QString mediaMid(const QString &localSdp, int videoIndex)
{
    // webrtcbin names its sections "audio0", "video1", "video2"; a Teams offer we answer
    // uses "0", "1", "2". videoIndex: 1 the camera section, 2 the screen one.
    int seen = 0;
    bool wanted = false;
    for (const QString &l : localSdp.split(QLatin1String("\r\n"))) {
        if (l.startsWith(QLatin1String("m="))) wanted = l.startsWith(QLatin1String("m=video")) && ++seen == videoIndex;
        else if (wanted && l.startsWith(QLatin1String("a=mid:"))) return l.mid(6);
    }
    return QString::number(videoIndex);
}

QString localVideoMsid(const QString &localSdp)
{
    // "a=msid:<stream> <track>" inside our video section if webrtcbin wrote one; it usually
    // writes the stream id only on the ssrc line ("a=ssrc:N msid:<stream> <track>").
    bool inVideo = false;
    QString fromSsrc;
    for (const QString &l : localSdp.split(QLatin1String("\r\n"))) {
        if (l.startsWith(QLatin1String("m="))) inVideo = l.startsWith(QLatin1String("m=video"));
        else if (inVideo && l.startsWith(QLatin1String("a=msid:"))) return l.mid(7).section(QLatin1Char(' '), 0, 0);
        else if (inVideo && fromSsrc.isEmpty() && l.startsWith(QLatin1String("a=ssrc:")) && l.contains(QLatin1String(" msid:")))
            fromSsrc = l.section(QLatin1String(" msid:"), 1).section(QLatin1Char(' '), 0, 0);
    }
    return fromSsrc;
}

QString toTeams(const QString &input, const QStringList &candidates, const RejectedSections &rejected)
{
    // Microsoft's media controller reads "c=IN IP4 0.0.0.0" / port 9 as disabled media
    // ("DisabledMediaNotSupported"); Chrome writes its default candidate there, as we do.
    QString ip, port;
    for (const QString &c : std::as_const(candidates)) {
        // "candidate:<f> 1 UDP <prio> <ip> <port> typ host ..."
        const QStringList f = c.split(QLatin1Char(' '));
        if (f.size() >= 8 && f[1] == QLatin1String("1") && f[2].compare(QLatin1String("UDP"), Qt::CaseInsensitive) == 0
            && f[7] == QLatin1String("host") && !f[4].contains(QLatin1Char(':'))) {
            ip = f[4]; port = f[5];
            break;
        }
    }
    // Teams web rewrites libwebrtc's SDP into its "sdp-ngc-1.0" dialect before sending
    // (seen in the capture): RTP/SAVP profile, 127.0.0.1 origin, b=CT, a=label, a=rtcp:<port>.
    // Every media section (an incoming offer may carry video, screen sharing and data too)
    // gets the real address; the audio one also its label.
    QStringList lines = input.split(QLatin1String("\r\n"), Qt::SkipEmptyParts);
    // A section webrtcbin rejected in an answer comes with port 0 and no a=bundle-only (the
    // bundled ones have it) and a copy of the offer's attributes, remote address included.
    // It must stay rejected: made "active" like a bundled one it claims media nobody
    // negotiated, and Teams ends the call with SdpParsingFailure.
    struct Section { bool rejected = false; QString mid; };
    QList<Section> sections;
    for (const QString &l : std::as_const(lines)) {
        if (l.startsWith(QLatin1String("m="))) {
            const QStringList f = l.split(QLatin1Char(' '));
            sections.append({f.value(1) == QLatin1String("0"), QString()});
        } else if (!sections.isEmpty()) {
            if (l == QLatin1String("a=bundle-only")) sections.last().rejected = false;
            else if (l.startsWith(QLatin1String("a=mid:"))) sections.last().mid = l.mid(6);
        }
    }
    QStringList out;
    QString kind;
    int videoSections = 0;
    int sectionIndex = -1;
    bool skipping = false;
    // Port 9 (Chrome) or 0 with a=bundle-only (webrtcbin's bundled sections): Teams reads
    // both as disabled media, so every section gets the real port.
    static const QRegularExpression mline(QStringLiteral("^m=(\\w+) [09] "));
    for (QString l : lines) {
        if (skipping && !l.startsWith(QLatin1String("m="))) continue; // the body of a rejected section
        if (l.startsWith(QLatin1String("m="))) {
            kind = l.section(QLatin1Char(' '), 0, 0).mid(2);
            if (kind == QLatin1String("video")) ++videoSections;
            skipping = sections.value(++sectionIndex).rejected;
            if (skipping) {
                QStringList f = l.split(QLatin1Char(' '));
                if (f.size() > 2) f[2].replace(QLatin1String("UDP/TLS/RTP/SAVPF"), QLatin1String("RTP/SAVP"));
                out << f.join(QLatin1Char(' ')) << QStringLiteral("c=IN IP4 %1").arg(ip.isEmpty() ? QStringLiteral("0.0.0.0") : ip);
                if (!sections[sectionIndex].mid.isEmpty()) out << QStringLiteral("a=mid:%1").arg(sections[sectionIndex].mid);
                out << QStringLiteral("a=inactive");
                continue;
            }
            if (!ip.isEmpty()) l.replace(mline, QStringLiteral("m=\\1 %1 ").arg(port));
            l.replace(QLatin1String("UDP/TLS/RTP/SAVPF"), QLatin1String("RTP/SAVP"));
        } else if (l == QLatin1String("a=bundle-only")) {
            continue;
        } else if (!ip.isEmpty() && l == QLatin1String("c=IN IP4 0.0.0.0")) {
            l = QStringLiteral("c=IN IP4 %1").arg(ip);
        } else if (l.startsWith(QLatin1String("o=- ")) && l.endsWith(QLatin1String(" IN IP4 0.0.0.0"))) {
            l.replace(QLatin1String("0.0.0.0"), QLatin1String("127.0.0.1"));
        } else if (l == QLatin1String("a=rtcp-mux-only")) {
            continue; // Chrome does not send it; keep the parser happy
        } else if (l == QLatin1String("t=0 0")) {
            out << QStringLiteral("b=CT:4000");
        } else if (l == QLatin1String("a=rtcp-mux") && !ip.isEmpty()) {
            out << QStringLiteral("a=rtcp:%1").arg(port);
        } else if (l.startsWith(QLatin1String("a=mid:")) && kind == QLatin1String("audio")) {
            out << QStringLiteral("a=label:main-audio");
        } else if (l.startsWith(QLatin1String("a=mid:")) && kind == QLatin1String("video")) {
            // Teams names the camera section "main-video" and the screen one "applicationsharing-video".
            out << (videoSections == 1 ? QStringLiteral("a=label:main-video") : QStringLiteral("a=label:applicationsharing-video"));
            // Teams' video sections announce the in-band source-request signaling (seen in every
            // Teams offer); the controller may want it before it accepts the section.
            out << QStringLiteral("a=x-signaling-fb:* x-message app send:src recv:src,vc");
        } else if (l.startsWith(QLatin1String("a=framerate:"))) {
            continue; // not in Teams clients' sections
        }
        out << l;
    }
    lines = out;
    // Teams clients announce the SSRCs they will send with "a=x-ssrc-range" (seen in their
    // offers) and seem to accept only those: put ours, from each section's a=ssrc line,
    // after that section's c=.
    static const QRegularExpression ssrcLine(QStringLiteral("^a=ssrc:(\\d+) "));
    if (!input.contains(QLatin1String("a=x-ssrc-range"))) {
        QList<QPair<int, QString>> ranges; // c= line index -> ssrc
        int sectionC = -1;
        QString ssrc;
        const auto flush = [&] { if (sectionC >= 0 && !ssrc.isEmpty()) ranges.append({sectionC, ssrc}); sectionC = -1; ssrc.clear(); };
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].startsWith(QLatin1String("m="))) flush();
            else if (sectionC < 0 && lines[i].startsWith(QLatin1String("c="))) sectionC = i;
            else if (ssrc.isEmpty()) { const auto m = ssrcLine.match(lines[i]); if (m.hasMatch()) ssrc = m.captured(1); }
        }
        flush();
        for (int i = ranges.size() - 1; i >= 0; --i)
            lines.insert(ranges[i].first + 1, QStringLiteral("a=x-ssrc-range:%1-%1").arg(ranges[i].second));
    }
    // Sections we cannot handle go back rejected (port 0), so the answer has as many
    // m-lines as the offer.
    for (const auto &[mid, mline] : rejected) {
        QStringList f = mline.split(QLatin1Char(' '));
        if (f.size() > 2) { f[1] = QStringLiteral("0"); f[2].replace(QLatin1String("UDP/TLS/RTP/SAVPF"), QLatin1String("RTP/SAVP")); }
        lines << f.join(QLatin1Char(' ')) << QStringLiteral("c=IN IP4 %1").arg(ip.isEmpty() ? QStringLiteral("0.0.0.0") : ip);
        if (!mid.isEmpty()) lines << QStringLiteral("a=mid:%1").arg(mid);
        lines << QStringLiteral("a=inactive");
    }
    // Candidates belong to the first (bundled) media section: inserted before the second
    // m= line, not at the end, where they would count for a rejected section.
    QStringList candidateLines;
    for (const QString &c : std::as_const(candidates)) {
        if (c.isEmpty()) continue;
        const QString line = c.startsWith(QLatin1String("candidate:")) ? QStringLiteral("a=") + c : c;
        if (!lines.contains(line)) candidateLines << line;
    }
    int secondMedia = -1, seen = 0;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].startsWith(QLatin1String("m=")) && ++seen == 2) { secondMedia = i; break; }
    if (secondMedia < 0) lines << candidateLines;
    else for (int i = 0; i < candidateLines.size(); ++i) lines.insert(secondMedia + i, candidateLines[i]);
    return lines.join(QLatin1String("\r\n")) + QLatin1String("\r\n");
}

FromTeams fromTeams(const QString &sdp, bool alignProfile, int videoPt, const QString &videoProfile)
{
    FromTeams result;
    // Back from Microsoft's dialect to what webrtcbin understands:
    // - RTP/SAVP profile -> UDP/TLS/RTP/SAVPF;
    // - no direction attribute (RFC 4566 default: sendrecv): webrtcbin needs it spelled out;
    // - candidates: drop the "tcp-pass"/"tcp-act" transports and the trailing "MTURNID ..."
    //   extension, which libnice does not parse; the UDP relay candidates are what matter;
    // - video: RTX payload types (we offer none; a PT without rtpmap confuses the mapping),
    //   rid/simulcast lines, and in an answer the H.264 profile-level-id becomes the one we
    //   advertised (level asymmetry is allowed; webrtcbin compares the strings). That holds
    //   for an offer too when we are the offerer: a renegotiation offer from Teams lists the
    //   profile of its own encoder, which would make webrtcbin reject the section.
    QStringList lines = sdp.split(QLatin1String("\r\n"), Qt::SkipEmptyParts);
    static const QRegularExpression rtxMap(QStringLiteral("^a=rtpmap:(\\d+) rtx/"), QRegularExpression::CaseInsensitiveOption);
    QStringList rtxPts;
    for (const QString &l : lines) { const auto m = rtxMap.match(l); if (m.hasMatch()) rtxPts << m.captured(1); }
    QStringList out;
    bool inMedia = false, hasDirection = false, skipping = false, inVideo = false;
    QString skippedMid;
    QStringList skippedMids;
    const auto flushDirection = [&] {
        if (inMedia && !hasDirection && !skipping) out << QStringLiteral("a=sendrecv");
    };
    static const QRegularExpression ptLine(QStringLiteral("^a=(rtpmap|fmtp|rtcp-fb):(\\d+) "));
    static const QRegularExpression profile(QStringLiteral("profile-level-id=[0-9a-fA-F]+"));
    for (QString l : lines) {
        if (l.startsWith(QLatin1String("m="))) {
            flushDirection();
            inMedia = true;
            hasDirection = false;
            const QString kind = l.section(QLatin1Char(' '), 0, 0).mid(2);
            inVideo = kind == QLatin1String("video");
            skipping = kind != QLatin1String("audio") && kind != QLatin1String("video") && kind != QLatin1String("application");
            if (skipping) { result.rejected.append({QString(), l}); continue; }
            l.replace(QLatin1String(" RTP/SAVP "), QLatin1String(" UDP/TLS/RTP/SAVPF "));
            if (!rtxPts.isEmpty()) {
                QStringList f = l.split(QLatin1Char(' '));
                for (int i = f.size() - 1; i >= 3; --i) if (rtxPts.contains(f[i])) f.removeAt(i);
                l = f.join(QLatin1Char(' '));
            }
        } else if (skipping) {
            if (l.startsWith(QLatin1String("a=mid:"))) {
                result.rejected.last().first = l.mid(6);
                skippedMids << l.mid(6);
            }
            continue;
        } else if (l == QLatin1String("a=sendrecv") || l == QLatin1String("a=sendonly") || l == QLatin1String("a=recvonly") || l == QLatin1String("a=inactive")) {
            hasDirection = true;
        } else if (l.startsWith(QLatin1String("a=candidate:"))) {
            if (l.contains(QLatin1String(" tcp-pass ")) || l.contains(QLatin1String(" tcp-act "))) continue;
            l = l.section(QLatin1String(" MTURNID"), 0, 0);
        } else if (l.startsWith(QLatin1String("a=rid:")) || l.startsWith(QLatin1String("a=simulcast:"))) {
            continue;
        } else if (const auto m = ptLine.match(l); m.hasMatch()) {
            if (rtxPts.contains(m.captured(2))) continue;
            if (alignProfile && inVideo && m.captured(1) == QLatin1String("fmtp") && m.captured(2).toInt() == videoPt)
                l.replace(profile, QStringLiteral("profile-level-id=") + videoProfile);
        }
        out << l;
    }
    flushDirection();
    // A section the controller rejects comes back bare ("m=video 0 RTP/AVP 107", a label,
    // a direction): webrtcbin refuses the whole description without a fingerprint and ICE
    // credentials in every section ("No fingerprint lines in sdp for media 1"), so the
    // first section's are repeated there, and the profile becomes the one it knows.
    {
        QStringList transport;
        bool firstMedia = false, pastFirst = false;
        for (const QString &l : out) {
            if (l.startsWith(QLatin1String("m="))) { if (firstMedia) pastFirst = true; firstMedia = true; }
            if (firstMedia && !pastFirst && (l.startsWith(QLatin1String("a=fingerprint:")) || l.startsWith(QLatin1String("a=ice-ufrag:"))
                                             || l.startsWith(QLatin1String("a=ice-pwd:")) || l.startsWith(QLatin1String("a=setup:"))))
                transport << l;
        }
        QStringList fixed;
        bool rejected = false, hasFingerprint = false;
        const auto flushTransport = [&] { if (rejected && !hasFingerprint) fixed << transport; };
        for (QString l : std::as_const(out)) {
            if (l.startsWith(QLatin1String("m="))) {
                flushTransport();
                const QStringList f = l.split(QLatin1Char(' '));
                rejected = f.size() > 1 && f[1] == QLatin1String("0");
                hasFingerprint = false;
                if (rejected) { l.replace(QLatin1String(" RTP/AVP "), QLatin1String(" UDP/TLS/RTP/SAVPF ")); l.replace(QLatin1String(" RTP/SAVP "), QLatin1String(" UDP/TLS/RTP/SAVPF ")); }
            } else if (l.startsWith(QLatin1String("a=fingerprint:"))) {
                hasFingerprint = true;
            }
            fixed << l;
        }
        flushTransport();
        out = fixed;
    }
    // The controller sends each section's streams with SSRCs from that section's
    // "a=x-ssrc-range" (audio 1000, camera 1001-1100, screen 1101-1200 in its answers) and
    // writes no a=ssrc lines; webrtcbin routes an incoming SSRC by the a=ssrc lines it was
    // given, else by payload type, which both video sections share. Spelling the ranges
    // out as a=ssrc lines keeps camera and screen on their own sections.
    {
        static const QRegularExpression range(QStringLiteral("^a=x-ssrc-range:(\\d+)-(\\d+)$"));
        QStringList withSsrc;
        QStringList pendingSsrc;
        bool hasSsrcLines = false;
        const auto flush = [&] { if (!hasSsrcLines) withSsrc << pendingSsrc; pendingSsrc.clear(); hasSsrcLines = false; };
        for (const QString &l : std::as_const(out)) {
            if (l.startsWith(QLatin1String("m="))) flush();
            else if (l.startsWith(QLatin1String("a=ssrc:"))) hasSsrcLines = true;
            else if (const auto m = range.match(l); m.hasMatch()) {
                const quint64 from = m.captured(1).toULongLong(), to = m.captured(2).toULongLong();
                if (to >= from && to - from < 400)
                    for (quint64 s = from; s <= to; ++s) pendingSsrc << QStringLiteral("a=ssrc:%1 cname:teams").arg(s);
            }
            withSsrc << l;
        }
        flush();
        out = withSsrc;
    }
    // The bundle group must not name sections webrtcbin does not see.
    for (QString &l : out) {
        if (!l.startsWith(QLatin1String("a=group:BUNDLE"))) continue;
        QStringList mids = l.mid(QStringLiteral("a=group:BUNDLE").size()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &m : skippedMids) mids.removeAll(m);
        l = QStringLiteral("a=group:BUNDLE ") + mids.join(QLatin1Char(' '));
    }
    result.sdp = out.join(QLatin1String("\r\n")) + QLatin1String("\r\n");
    return result;
}

}
