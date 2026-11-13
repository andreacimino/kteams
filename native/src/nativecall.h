#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <functional>
#include "callengine.h"

class QVideoSink;

// Native Teams call, signaling side (prototype, audio only). The protocol is the one the
// Teams web app uses (docs/calling-protocol.md):
//   TURN credentials  GET  teams.microsoft.com/trap/tokens
//   start             POST api.flightproxy.teams.microsoft.com/api/v2/epconv  (SDP offer inside)
//   answer / events   callbacks to our Trouter: <surl>callAgent/<agent>/<n>/call|conversation/<event>/
//   hang up           POST the "leave" link returned by epconv
// For now it places Teams' own test call ("UserInitiatedTestCall"), which reaches no person.
class NativeCall : public QObject
{
    Q_OBJECT
public:
    struct Identity {
        QString skypeToken;
        QString selfMri;
        QString displayName;
        QString endpointId;   // our Trouter endpoint
        QString trouterUrl;   // our Trouter "surl" (callbacks are delivered there)
    };

    explicit NativeCall(QObject *parent = nullptr);

    void startTestCall(const Identity &identity);
    // An incoming call (Trouter NGCallManagerWin notification): attaches and rings.
    void incoming(const Identity &identity, const QJsonObject &notification);
    void accept();
    void reject();
    bool isIncoming() const { return m_incoming; }
    // The chat thread of the call (a chat, a group or a meeting); empty for the test call and
    // for an incoming call, whose chat is found from the caller.
    QString threadId() const { return m_threadId; }
    // One-to-one calls: the other person's MRI (the caller of an incoming call).
    QString peerMri() const { return m_callees.isEmpty() ? m_calleeMri : m_callees.first(); }
    QString peerName() const { return m_calleeName; }
    // Audio call to one person: their MRI and the one-to-one chat thread id.
    // Audio call to one person (one MRI) or a group (every other member of the chat); the
    // thread id is the chat's. Group calls run through Microsoft's mixer.
    void startCall(const Identity &identity, const QStringList &calleeMris, const QString &threadId, const QString &calleeName);
    // Joins a Teams meeting: its thread ("19:meeting_...@thread.v2") and, from the join
    // link's context, the tenant and organizer ids (Teams web's meetingInfo).
    void joinMeeting(const Identity &identity, const QString &threadId, const QString &tenantId, const QString &organizerId, const QString &subject);
    void hangUp();
    void setMuted(bool muted) { m_engine.setMuted(muted); }
    bool isActive() const { return m_active; }
    QString state() const { return m_state; }

    // Video: the camera goes out on the call's video section; the remote stream and the
    // camera preview are shown in the sinks (QML VideoOutput.videoSink).
    void setCameraEnabled(bool on) { m_engine.setCameraEnabled(on); }
    bool cameraEnabled() const { return m_engine.cameraEnabled(); }
    bool remoteVideoActive() const { return m_engine.remoteVideoActive(); }
    void setVideoSinks(QVideoSink *remote, QVideoSink *local, QVideoSink *share) { m_engine.setVideoSinks(remote, local, share); }
    // Screen sharing: the PipeWire stream from the desktop portal (fd is taken over).
    void startScreenShare(int fd, unsigned nodeId) { m_engine.startScreenShare(fd, nodeId); }
    void stopScreenShare() { m_engine.stopScreenShare(); }
    bool screenShareEnabled() const { return m_engine.screenShareEnabled(); }
    bool remoteShareActive() const { return m_engine.remoteShareActive(); }
    bool mediaConnected() const { return m_engine.isConnected(); }

    // A Trouter request for one of our callback links; returns whether it was ours.
    bool handleCallback(const QString &url, const QJsonObject &body);

signals:
    void stateChanged();
    void activeChanged();
    void incomingChanged();
    void videoChanged();

private:
    void setState(const QString &state);
    void finish(const QString &state);
    void onOffer(const QString &sdp);
    QString link(const QString &kind, const QString &name) const;
    QJsonObject selfParticipant() const;
    void postJson(const QString &context, const QByteArray &verb, const QString &url, const QJsonObject &body,
                  std::function<void(const QJsonObject &)> onReply = {});
    void addTestBot();
    // Media controller links (applyChannelParameters, controlVideoStreaming) arrive inside
    // other messages: picked up wherever they show up.
    void rememberMediaLinks(const QJsonValue &v);
    // What we can send (Teams web announces it right after the call is accepted).
    void sendMaxVideoSendCapabilities();
    // Ask the media controller for a participant's video source on our video section.
    void requestRemoteVideo(int sourceId, bool screen);
    // Roster entries list each endpoint's media streams: a video one that is sending gets requested.
    void parseRosterStreams(const QJsonObject &body);
    QJsonObject m_mediaLinks;
    int m_videoSequence = 0;
    QSet<int> m_requestedSources;
    QSet<int> m_pendingSources;  // seen before the applyChannelParameters link was known
    QSet<int> m_screenSources;   // the pending ones that are screen sharing
    void begin(const Identity &identity);
    QString m_calleeMri;    // empty: the test call
    QStringList m_callees;
    QString m_threadId;
    bool m_meeting = false;
    QString m_tenantId, m_organizerId;
    QString m_calleeName;

    CallEngine m_engine;
    QNetworkAccessManager m_nam;
    Identity m_id;
    QString m_agentId;
    QString m_participantId;
    QJsonObject m_links;      // from epconv: leave, mute, unmute, subscribe...
    QJsonObject m_callLinks;  // from acceptance: acknowledgement, hold, mediaRenegotiation...
    QString m_renegotiationAnswerLink;
    QString m_renegotiationLegId;
    bool m_incoming = false;      // ringing, not yet answered
    QString m_remoteOffer;        // the caller's SDP
    QString m_remoteLegId;
    QJsonObject m_invitationLinks; // acceptance, callLeg (reject), progress, mediaAnswer
    TonePlayer m_tone;
    TonePlayer m_cue;          // the short sounds: connected, ended
    bool m_connected = false;  // the media connected at some point in this call
    QStringList m_turn;
    void fetchTurn(std::function<void(const QStringList &)> done);
    QJsonObject acceptanceLinks() const;
    QString m_state;
    bool m_active = false;
};
