#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <atomic>

class QVideoSink;
typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
typedef struct _GstPromise GstPromise;
typedef struct _GstSample GstSample;
typedef struct _GstCaps GstCaps;
typedef struct _GParamSpec GParamSpec;
typedef void *gpointer;
typedef unsigned int guint;
typedef char gchar;

// Media side of a native call: WebRTC through GStreamer's webrtcbin.
// Microphone -> Opus -> WebRTC (ICE + DTLS-SRTP, Microsoft TURN relays) -> speakers, plus
// two H.264 video sections, as Teams names them: "main-video" (the camera) and
// "applicationsharing-video" (the screen). Each sends only while its source is on and shows
// what arrives in a QVideoSink (a QML VideoOutput). The SDP offer is produced once ICE
// gathering has found candidates; the answer comes from the signaling (NativeCall).
class CallEngine : public QObject
{
    Q_OBJECT
public:
    explicit CallEngine(QObject *parent = nullptr);
    ~CallEngine() override;

    // turnServers: "turn(s)://user:password@host:port?transport=udp|tcp" (user/password URL-encoded).
    // answerer: the remote offer comes first (incoming call); no offer is generated.
    // opusPt / videoPt / sharePt: payload types; answering, the ones the offer uses.
    // videoProfile: the H.264 profile-level-id we advertise (the offer's one when answering).
    bool start(const QStringList &turnServers, bool answerer = false, int opusPt = 102, int videoPt = 107,
               const QString &videoProfile = QStringLiteral("42e01f"), int sharePt = 107);
    void setRemoteAnswer(const QString &sdp);
    // A renegotiation offer from the media controller: applied, answered (answerReady).
    void setRemoteOffer(const QString &sdp);
    void setMuted(bool muted);
    void stop();
    bool isRunning() const { return m_pipeline != nullptr; }
    // The media transport is up: only then can sources be switched (before that webrtcbin
    // holds its sink pads, and a source change would wait on them forever).
    bool isConnected() const { return m_connected; }

    // Where the remote video, the camera preview and the remote screen are shown (may be null).
    void setVideoSinks(QVideoSink *remote, QVideoSink *local, QVideoSink *share);
    // Camera on: its frames go out on the video section (nothing is sent while it is off).
    void setCameraEnabled(bool on);
    bool cameraEnabled() const { return m_camera; }
    // Screen sharing from a PipeWire stream (desktop portal): fd is taken over.
    void startScreenShare(int fd, unsigned nodeId);
    void stopScreenShare();
    bool screenShareEnabled() const { return m_share; }
    // Makes a video section sendable when the negotiated direction forbids it (a Teams
    // desktop client offers the screen section inactive and the camera one recvonly): the
    // transceiver turns sendrecv and a new offer goes out through reofferReady.
    void ensureSendable(int videoIndex);
    bool remoteVideoActive() const { return m_remoteVideo; }
    bool remoteShareActive() const { return m_remoteShare; }
    // The msid of our video section in the SDP we sent (Teams names streams by it), or "".
    QString localVideoMsid() const;
    // mids of our video and screen sections in the SDP we sent.
    QString videoMid() const { return mediaMid(1); }
    QString shareMid() const { return mediaMid(2); }

signals:
    void offerReady(const QString &sdp);
    // A renegotiation offer from our side (ensureSendable), in the Teams dialect.
    void reofferReady(const QString &sdp);
    void answerReady(const QString &sdp);
    void connectionStateChanged(const QString &state);
    void failed(const QString &message);
    void remoteVideoChanged(bool active);
    void remoteShareChanged(bool active);
    void cameraChanged(bool on);
    void screenShareChanged(bool on);

private:
    enum class Frame { Remote, Local, Share };
    static void onNegotiationNeeded(GstElement *webrtc, gpointer self);
    static void onOfferCreated(GstPromise *promise, gpointer self);
    static void onReofferCreated(GstPromise *promise, gpointer self);
    static void onIceCandidate(GstElement *webrtc, guint mline, gchar *candidate, gpointer self);
    static void onGatheringState(GstElement *webrtc, GParamSpec *, gpointer self);
    static void onConnectionState(GstElement *webrtc, GParamSpec *, gpointer self);
    static void onIncomingPad(GstElement *webrtc, GstPad *pad, gpointer self);
    static void onDecodedPad(GstElement *decodebin, GstPad *pad, gpointer self);
    void sendOffer();
    void sendPending();
    static void onAnswerCreated(GstPromise *promise, gpointer self);
    QString toTeamsDialect(const QString &sdp) const;
    QString fromTeamsDialect(const QString &sdp, bool alignProfile = false);
    // A decoded frame (I420) for one of the sinks, from a GStreamer thread.
    void deliverFrame(GstSample *sample, Frame which);
    // The H.264 RTP caps as the SDP must show them: agreed profile, no parameter sets.
    GstCaps *rewriteVideoCaps(const GstCaps *caps) const;
    // Caps rewriting and packet counting on the identity before a video section's valve.
    void installVideoProbes(const char *element, std::atomic<quint64> *packets);
    // Puts a live source bin on a section (input-selector + valve), or takes it off.
    bool attachSource(GstElement *&bin, GstPad *&pad, GstElement *selector, GstElement *valve, const QByteArray &description);
    void detachSource(GstElement *&bin, GstPad *&pad, GstElement *selector, GstElement *valve);
    QString mediaMid(int videoIndex) const;
    // Media sections webrtcbin cannot handle (Microsoft "x-data"), kept out of the offer
    // it sees and put back rejected in the answer: {mid, "m=" line}.
    QList<QPair<QString, QString>> m_rejectedSections;

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtc = nullptr;
    GstElement *m_volume = nullptr;
    GstElement *m_videoValve = nullptr;    // closed while the camera is off: no video packets
    GstElement *m_videoSelector = nullptr; // black frames or the camera
    GstElement *m_cameraBin = nullptr;
    GstPad *m_cameraPad = nullptr;         // the selector's pad the camera feeds
    GstElement *m_shareValve = nullptr;
    GstElement *m_shareSelector = nullptr;
    GstElement *m_shareBin = nullptr;
    GstPad *m_sharePad = nullptr;
    int m_shareFd = -1;
    QString m_offerSdp;
    QString m_answerSdp;      // an answer waiting for ICE candidates
    QString m_localSdp;       // the last offer/answer we produced (before the dialect)
    QStringList m_candidates;
    bool m_offerSent = false;
    bool m_answerer = false;
    bool m_gatheringComplete = false;
    int m_videoPt = 107;
    int m_sharePt = 108;
    QString m_videoProfile;
    QTimer m_gatherTimeout;
    QTimer m_statsTimer;      // logs sent/received packets and the peer's receiver reports
    QTimer m_videoWatch;      // remote video counts as stopped after a few seconds without frames
    std::atomic<quint64> m_micPackets{0}; // RTP packets leaving the microphone chain
    std::atomic<quint64> m_micRaw{0};     // raw buffers leaving pipewiresrc
    std::atomic<quint64> m_videoPackets{0};
    std::atomic<quint64> m_sharePackets{0};
    std::atomic<quint64> m_remoteFrames{0};
    std::atomic<quint64> m_shareFrames{0};
    std::atomic<qint64> m_lastRemoteFrameMs{0};
    std::atomic<qint64> m_lastShareFrameMs{0};
    std::atomic<bool> m_remotePending{false}, m_localPending{false}, m_sharePending{false};
    QPointer<QVideoSink> m_remoteSink, m_localSink, m_shareSink;
    bool m_camera = false;
    bool m_share = false;
    bool m_connected = false;
    bool m_remoteVideo = false;
    bool m_remoteShare = false;
    void logStats();
};

// Ringtone / ring-back tone from a sound file, looped until stopped.
class TonePlayer : public QObject
{
    Q_OBJECT
public:
    explicit TonePlayer(QObject *parent = nullptr);
    ~TonePlayer() override;
    void play(const QString &path);
    void stop();

private:
    GstElement *m_playbin = nullptr;
};
