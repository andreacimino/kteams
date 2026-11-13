#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Microsoft's SDP dialect ("sdp-ngc-1.0") and webrtcbin's, and the translation between them.
// Pure text functions (see tests/tst_sdpdialect.cpp).
namespace SdpDialect {

// Media sections we cannot handle (not audio, video or application): (mid, "m=" line).
using RejectedSections = QList<QPair<QString, QString>>;

// webrtcbin's SDP -> what Teams' media controller reads. `candidates` are our ICE candidates
// ("candidate:..."), `rejected` the sections of an offer we answer that go back with port 0.
QString toTeams(const QString &sdp, const QStringList &candidates, const RejectedSections &rejected);

struct FromTeams {
    QString sdp;
    RejectedSections rejected; // sections taken out because webrtcbin cannot take them
};
// Teams' SDP -> what webrtcbin understands. In an answer the H.264 profile-level-id of the
// video payload `videoPt` becomes `videoProfile` (the one we offered).
FromTeams fromTeams(const QString &sdp, bool answer, int videoPt, const QString &videoProfile);

// The a=mid of our n-th video section (1: camera, 2: screen); the number itself if there is none.
QString mediaMid(const QString &localSdp, int videoIndex);
// The stream id webrtcbin gave our video section, from a=msid or an ssrc line; empty if none.
QString localVideoMsid(const QString &localSdp);

}
