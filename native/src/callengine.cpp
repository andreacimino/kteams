#include "callengine.h"
#include "mediadevices.h"
#include "sdpdialect.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <unistd.h>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#define GST_USE_UNSTABLE_API
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

Q_LOGGING_CATEGORY(lcCallEngine, "kollega.callengine", QtInfoMsg)

namespace {
// Teams web sends its offer once a first candidate is known (~1 s, trickle ICE); waiting a
// little longer lets the relay candidates in, which work behind any NAT.
constexpr int kGatherTimeoutMs = 2500;
// Remote video counts as stopped after this long without a frame (the other side turned
// the camera off: Teams sends no event we handle yet, the packets just stop).
constexpr int kRemoteVideoTimeoutMs = 3000;
// Camera frames are scaled to this size before encoding.
constexpr int kVideoWidth = 640, kVideoHeight = 360, kVideoFps = 15;
// Screen sharing: letterboxed into this size, fewer frames (text, not motion).
constexpr int kShareWidth = 1280, kShareHeight = 720, kShareFps = 10;
}

CallEngine::CallEngine(QObject *parent) : QObject(parent)
{
    static const bool initialized = [] { gst_init(nullptr, nullptr); return true; }();
    Q_UNUSED(initialized);
    m_gatherTimeout.setSingleShot(true);
    m_gatherTimeout.setInterval(kGatherTimeoutMs);
    connect(&m_gatherTimeout, &QTimer::timeout, this, &CallEngine::sendPending);
    m_statsTimer.setInterval(std::chrono::seconds(5));
    connect(&m_statsTimer, &QTimer::timeout, this, &CallEngine::logStats);
    m_videoWatch.setInterval(std::chrono::seconds(1));
    connect(&m_videoWatch, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_remoteVideo && now - m_lastRemoteFrameMs.load() > kRemoteVideoTimeoutMs) {
            m_remoteVideo = false;
            qCInfo(lcCallEngine) << "remote video stopped";
            emit remoteVideoChanged(false);
        }
        if (m_remoteShare && now - m_lastShareFrameMs.load() > kRemoteVideoTimeoutMs) {
            m_remoteShare = false;
            qCInfo(lcCallEngine) << "remote screen sharing stopped";
            emit remoteShareChanged(false);
        }
    });
}

CallEngine::~CallEngine() { stop(); }

bool CallEngine::start(const QStringList &turnServers, bool answerer, int opusPt, int videoPt, const QString &videoProfile, int sharePt)
{
    stop();
    m_rejectedSections.clear();
    m_offerSent = false;
    m_answerer = answerer;
    m_gatheringComplete = false;
    m_candidates.clear();
    m_offerSdp.clear();
    m_answerSdp.clear();
    m_localSdp.clear();
    m_videoPt = videoPt;
    m_sharePt = sharePt;
    m_videoProfile = videoProfile;
    m_videoPackets = m_sharePackets = 0;
    m_connected = false;
    m_remoteFrames = m_shareFrames = 0;
    m_lastRemoteFrameMs = m_lastShareFrameMs = 0;

    // Microphone -> mono 48 kHz -> Opus -> webrtcbin. Payload type 102: the one Microsoft's
    // media controller answers with for Opus (and lists first in Teams web's own offers);
    // an answer with a PT missing from the offer leaves webrtcbin unable to map the stream.
    GError *error = nullptr;
    // A fixed SSRC in the caps: webrtcbin then signals in the SDP the same SSRC rtpbin
    // sends with (otherwise the a=ssrc line and the packets disagree, and a Teams client
    // drops packets from an SSRC it was not told about).
    const quint32 ssrc = QRandomGenerator::global()->bounded(1u, 0x7fffffffu);
    const quint32 videoSsrc = QRandomGenerator::global()->bounded(1u, 0x7fffffffu);
    const quint32 shareSsrc = QRandomGenerator::global()->bounded(1u, 0x7fffffffu);
    const QByteArray launch = QStringLiteral(
        "webrtcbin name=webrtc bundle-policy=max-bundle "
        // Live microphone: a short leaky queue, so audio captured before the media path is
        // up is dropped instead of becoming a lasting delay; Opus tuned for speech.
        // PulseAudio API (served by PipeWire): pipewiresrc and pipewiresink in one pipeline
        // never start (two clock providers block each other), pulsesrc/pulsesink do.
        // Echo cancellation, noise suppression and gain control (WebRTC audio processing):
        // webrtcdsp on the microphone, fed with what the speakers play by webrtcechoprobe.
        "pulsesrc name=mic @MICDEV@client-name=\"Kollega\" do-timestamp=true ! queue max-size-time=200000000 max-size-buffers=0 max-size-bytes=0 leaky=downstream "
        "! audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=1 "
        "! webrtcdsp probe=echoprobe echo-cancel=true noise-suppression=true gain-control=true delay-agnostic=true extended-filter=true "
        "! volume name=micvolume "
        "! audio/x-raw,rate=48000,channels=1 ! opusenc bitrate=32000 audio-type=voice frame-size=20 inband-fec=true ! rtpopuspay pt=%1 "
        "! application/x-rtp,media=audio,encoding-name=OPUS,payload=%1,clock-rate=48000,ssrc=(uint)%2 ! identity name=micout ! webrtc. "
        // Playback path, present from the start (the echo probe must exist when the DSP
        // starts): silence until the remote audio is mixed in.
        "audiotestsrc is-live=true wave=silence ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! audiomixer name=playmix "
        "! audio/x-raw,format=S16LE,rate=48000,channels=1 ! webrtcechoprobe name=echoprobe ! pulsesink @SPKDEV@client-name=\"Kollega\" "
        // Video section, present from the start (Teams web offers it with the camera off
        // too): black frames or the camera (input-selector), H.264 constrained baseline
        // for any Teams client, packetization-mode 1. The valve stays closed while the
        // camera is off, so no video packets leave; sticky events (caps) still pass, which
        // is what lets webrtcbin describe the section in the offer.
        "videotestsrc name=black is-live=true pattern=black ! video/x-raw,format=I420,width=%5,height=%6,framerate=%7/1 ! camsel.sink_0 "
        "input-selector name=camsel ! queue max-size-buffers=2 leaky=downstream ! tee name=camtee "
        "camtee. ! queue max-size-buffers=2 leaky=downstream ! x264enc name=venc tune=zerolatency speed-preset=ultrafast "
        "bitrate=700 key-int-max=%8 bframes=0 threads=2 ! video/x-h264,profile=constrained-baseline "
        "! rtph264pay name=vpay pt=%3 config-interval=-1 aggregate-mode=zero-latency mtu=1200 "
        // No profile-level-id in this filter: rtph264pay refuses to negotiate against a fixed
        // one that differs from the encoder's ("failed to set sps/pps"); the probe on its
        // source pad below writes the agreed profile into the caps event instead.
        "! application/x-rtp,media=video,encoding-name=H264,payload=%3,clock-rate=90000,ssrc=(uint)%4 "
        "! identity name=vidout ! valve name=vidvalve drop=true drop-mode=forward-sticky-events ! webrtc. "
        "camtee. ! queue max-size-buffers=1 leaky=downstream ! appsink name=localpreview emit-signals=true sync=false drop=true max-buffers=1 "
        // Screen sharing section ("applicationsharing-video", third in Teams' order too):
        // same arrangement, the PipeWire stream from the desktop portal as the source.
        "videotestsrc name=shareblack is-live=true pattern=black ! video/x-raw,format=I420,width=%11,height=%12,framerate=%13/1 ! sharesel.sink_0 "
        "input-selector name=sharesel ! queue max-size-buffers=2 leaky=downstream ! x264enc name=senc tune=zerolatency speed-preset=ultrafast "
        "bitrate=1500 key-int-max=%14 bframes=0 threads=2 ! video/x-h264,profile=constrained-baseline "
        "! rtph264pay name=spay pt=%9 config-interval=-1 aggregate-mode=zero-latency mtu=1200 "
        "! application/x-rtp,media=video,encoding-name=H264,payload=%9,clock-rate=90000,ssrc=(uint)%10 "
        "! identity name=shareout ! valve name=sharevalve drop=true drop-mode=forward-sticky-events ! webrtc.")
        .arg(opusPt).arg(ssrc).arg(videoPt).arg(videoSsrc).arg(kVideoWidth).arg(kVideoHeight).arg(kVideoFps).arg(kVideoFps * 3)
        .arg(sharePt).arg(shareSsrc).arg(kShareWidth).arg(kShareHeight).arg(kShareFps).arg(kShareFps * 3)
         // The devices chosen in the settings (the default ones when none, or when it is unplugged).
         .replace(QLatin1String("@MICDEV@"), MediaDevices::pipelineDeviceProperty(MediaDevices::selectedIfPresent(MediaDevices::Kind::AudioInput)))
         .replace(QLatin1String("@SPKDEV@"), MediaDevices::pipelineDeviceProperty(MediaDevices::selectedIfPresent(MediaDevices::Kind::AudioOutput))).toUtf8();
    qCInfo(lcCallEngine) << "sending with ssrc" << ssrc << "pt" << opusPt << "video ssrc" << videoSsrc << "pt" << videoPt << "profile" << videoProfile
                         << "share ssrc" << shareSsrc << "pt" << sharePt;
    m_pipeline = gst_parse_launch(launch.constData(), &error);
    if (!m_pipeline) {
        emit failed(QStringLiteral("GStreamer: %1").arg(QString::fromUtf8(error ? error->message : "pipeline")));
        g_clear_error(&error);
        return false;
    }
    m_webrtc = gst_bin_get_by_name(GST_BIN(m_pipeline), "webrtc");
    m_micPackets = 0;
    m_micRaw = 0;
    if (GstElement *mic = gst_bin_get_by_name(GST_BIN(m_pipeline), "mic")) {
        GstPad *src = gst_element_get_static_pad(mic, "src");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *, gpointer self) -> GstPadProbeReturn {
            ++static_cast<CallEngine *>(self)->m_micRaw;
            return GST_PAD_PROBE_OK;
        }, this, nullptr);
        gst_object_unref(src);
        gst_object_unref(mic);
    }
    if (GstElement *micout = gst_bin_get_by_name(GST_BIN(m_pipeline), "micout")) {
        GstPad *src = gst_element_get_static_pad(micout, "src");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *, gpointer self) -> GstPadProbeReturn {
            ++static_cast<CallEngine *>(self)->m_micPackets;
            return GST_PAD_PROBE_OK;
        }, this, nullptr);
        gst_object_unref(src);
        gst_object_unref(micout);
    }
    m_volume = gst_bin_get_by_name(GST_BIN(m_pipeline), "micvolume");
    m_videoValve = gst_bin_get_by_name(GST_BIN(m_pipeline), "vidvalve");
    m_videoSelector = gst_bin_get_by_name(GST_BIN(m_pipeline), "camsel");
    m_shareValve = gst_bin_get_by_name(GST_BIN(m_pipeline), "sharevalve");
    m_shareSelector = gst_bin_get_by_name(GST_BIN(m_pipeline), "sharesel");
    installVideoProbes("vidout", &m_videoPackets);
    installVideoProbes("shareout", &m_sharePackets);
    if (GstElement *preview = gst_bin_get_by_name(GST_BIN(m_pipeline), "localpreview")) {
        g_signal_connect(preview, "new-sample", G_CALLBACK(+[](GstElement *sink, gpointer selfPtr) -> GstFlowReturn {
            GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
            if (!sample) return GST_FLOW_OK;
            auto *self = static_cast<CallEngine *>(selfPtr);
            if (self->m_camera) self->deliverFrame(sample, Frame::Local);
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }), this);
        gst_object_unref(preview);
    }

    for (const QString &turn : turnServers) {
        gboolean ok = FALSE;
        g_signal_emit_by_name(m_webrtc, "add-turn-server", turn.toUtf8().constData(), &ok);
        if (!ok) qCWarning(lcCallEngine) << "TURN server not accepted" << turn.section(QLatin1Char('@'), 1);
    }

    g_signal_connect(m_webrtc, "on-negotiation-needed", G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtc, "notify::ice-gathering-state", G_CALLBACK(onGatheringState), this);
    g_signal_connect(m_webrtc, "notify::connection-state", G_CALLBACK(onConnectionState), this);
    g_signal_connect(m_webrtc, "notify::ice-connection-state", G_CALLBACK(+[](GstElement *w, GParamSpec *, gpointer) {
        GstWebRTCICEConnectionState s;
        g_object_get(w, "ice-connection-state", &s, nullptr);
        qCInfo(lcCallEngine) << "ice-connection-state" << int(s) << "(0 new,1 checking,2 connected,3 completed,4 failed,5 disconnected,6 closed)";
    }), nullptr);
    g_signal_connect(m_webrtc, "notify::ice-gathering-state", G_CALLBACK(+[](GstElement *w, GParamSpec *, gpointer) {
        GstWebRTCICEGatheringState s;
        g_object_get(w, "ice-gathering-state", &s, nullptr);
        qCInfo(lcCallEngine) << "ice-gathering-state" << int(s) << "(0 new,1 gathering,2 complete)";
    }), nullptr);
    g_signal_connect(m_webrtc, "notify::signaling-state", G_CALLBACK(+[](GstElement *w, GParamSpec *, gpointer) {
        GstWebRTCSignalingState s;
        g_object_get(w, "signaling-state", &s, nullptr);
        qCInfo(lcCallEngine) << "signaling-state" << int(s) << "(0 stable,1 closed,2 have-local-offer,3 have-remote-offer,...)";
    }), nullptr);
    g_signal_connect(m_webrtc, "pad-added", G_CALLBACK(onIncomingPad), this);

    GstBus *bus = gst_element_get_bus(m_pipeline);
    gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer self) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            qCWarning(lcCallEngine) << "GStreamer warning:" << (err ? err->message : "") << (debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
        }
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            const QString text = QString::fromUtf8(err->message);
            qCWarning(lcCallEngine) << "GStreamer error:" << text << (debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
            QMetaObject::invokeMethod(static_cast<CallEngine *>(self), [self, text] { emit static_cast<CallEngine *>(self)->failed(text); }, Qt::QueuedConnection);
        }
        return TRUE;
    }, this);
    gst_object_unref(bus);

    m_statsTimer.start();
    m_videoWatch.start();
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        emit failed(tr("GStreamer: could not start the audio"));
        stop();
        return false;
    }
    return true;
}

void CallEngine::stop()
{
    m_gatherTimeout.stop();
    m_statsTimer.stop();
    m_videoWatch.stop();
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    for (GstPad *p : {m_cameraPad, m_sharePad}) if (p) gst_object_unref(p);
    for (GstElement *e : {m_cameraBin, m_shareBin, m_videoValve, m_videoSelector, m_shareValve, m_shareSelector, m_volume, m_webrtc})
        if (e) gst_object_unref(e);
    gst_object_unref(m_pipeline);
    m_pipeline = m_webrtc = m_volume = m_videoValve = m_videoSelector = m_cameraBin = m_shareValve = m_shareSelector = m_shareBin = nullptr;
    m_cameraPad = m_sharePad = nullptr;
    if (m_shareFd >= 0) { ::close(m_shareFd); m_shareFd = -1; }
    m_connected = false;
    if (m_camera) { m_camera = false; emit cameraChanged(false); }
    if (m_share) { m_share = false; emit screenShareChanged(false); }
    if (m_remoteVideo) { m_remoteVideo = false; emit remoteVideoChanged(false); }
    if (m_remoteShare) { m_remoteShare = false; emit remoteShareChanged(false); }
}

void CallEngine::installVideoProbes(const char *element, std::atomic<quint64> *packets)
{
    GstElement *identity = gst_bin_get_by_name(GST_BIN(m_pipeline), element);
    if (!identity) return;
    // rtph264pay writes the encoder's real profile-level-id and the parameter sets into
    // the caps; the SDP must carry the profile we agreed on (webrtcbin compares the
    // strings with the remote description), and the parameter sets travel in-band.
    // webrtcbin reads the caps from the sticky event or from a caps query, depending on
    // when the offer is made: both get rewritten here, on the last pad before the valve.
    GstPad *src = gst_element_get_static_pad(identity, "src");
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, [](GstPad *, GstPadProbeInfo *info, gpointer selfPtr) -> GstPadProbeReturn {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
        GstCaps *caps = nullptr;
        gst_event_parse_caps(event, &caps);
        GstCaps *rewritten = static_cast<CallEngine *>(selfPtr)->rewriteVideoCaps(caps);
        GST_PAD_PROBE_INFO_DATA(info) = gst_event_new_caps(rewritten);
        gst_caps_unref(rewritten);
        gst_event_unref(event);
        return GST_PAD_PROBE_OK;
    }, this, nullptr);
    gst_pad_add_probe(src, GstPadProbeType(GST_PAD_PROBE_TYPE_QUERY_UPSTREAM | GST_PAD_PROBE_TYPE_PULL), [](GstPad *, GstPadProbeInfo *info, gpointer selfPtr) -> GstPadProbeReturn {
        GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query) != GST_QUERY_CAPS) return GST_PAD_PROBE_OK;
        GstCaps *result = nullptr;
        gst_query_parse_caps_result(query, &result);
        if (!result || gst_caps_is_empty(result) || gst_caps_is_any(result)) return GST_PAD_PROBE_OK;
        GstCaps *rewritten = static_cast<CallEngine *>(selfPtr)->rewriteVideoCaps(result);
        gst_query_set_caps_result(query, rewritten);
        gst_caps_unref(rewritten);
        return GST_PAD_PROBE_OK;
    }, this, nullptr);
    gst_object_unref(src);
    // Packets are counted after the valve: what actually goes to webrtcbin.
    GstElement *valve = nullptr;
    GstPad *valveSrc = gst_element_get_static_pad(identity, "src");
    if (GstPad *peer = gst_pad_get_peer(valveSrc)) { valve = gst_pad_get_parent_element(peer); gst_object_unref(peer); }
    gst_object_unref(valveSrc);
    if (valve) {
        GstPad *out = gst_element_get_static_pad(valve, "src");
        gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *, gpointer counter) -> GstPadProbeReturn {
            ++*static_cast<std::atomic<quint64> *>(counter);
            return GST_PAD_PROBE_OK;
        }, packets, nullptr);
        gst_object_unref(out);
        gst_object_unref(valve);
    }
    gst_object_unref(identity);
}

void CallEngine::setVideoSinks(QVideoSink *remote, QVideoSink *local, QVideoSink *share)
{
    m_remoteSink = remote;
    m_localSink = local;
    m_shareSink = share;
}

bool CallEngine::attachSource(GstElement *&bin, GstPad *&pad, GstElement *selector, GstElement *valve, const QByteArray &description)
{
    GError *error = nullptr;
    bin = gst_parse_bin_from_description(description.constData(), TRUE, &error);
    if (!bin) {
        qCWarning(lcCallEngine) << "source pipeline:" << (error ? error->message : "") << description;
        g_clear_error(&error);
        return false;
    }
    gst_object_ref(bin); // the pipeline keeps one reference, we keep another
    gst_bin_add(GST_BIN(m_pipeline), bin);
    pad = gst_element_request_pad_simple(selector, "sink_%u");
    GstPad *src = gst_element_get_static_pad(bin, "src");
    gst_pad_link(src, pad);
    gst_object_unref(src);
    gst_element_sync_state_with_parent(bin);
    g_object_set(selector, "active-pad", pad, nullptr);
    g_object_set(valve, "drop", FALSE, nullptr);
    return true;
}

void CallEngine::detachSource(GstElement *&bin, GstPad *&pad, GstElement *selector, GstElement *valve)
{
    g_object_set(valve, "drop", TRUE, nullptr);
    GstPad *black = gst_element_get_static_pad(selector, "sink_0");
    g_object_set(selector, "active-pad", black, nullptr);
    gst_object_unref(black);
    if (!bin) return;
    // The source's thread may be waiting inside the selector (a non-active pad is held
    // until the active one catches up): deactivating the pad releases it; only then can
    // the source be stopped without waiting for a push that would never return.
    gst_pad_set_active(pad, FALSE);
    GstPad *src = gst_element_get_static_pad(bin, "src");
    gst_pad_unlink(src, pad);
    gst_object_unref(src);
    gst_element_release_request_pad(selector, pad);
    gst_element_set_state(bin, GST_STATE_NULL);
    gst_object_unref(pad);
    pad = nullptr;
    gst_bin_remove(GST_BIN(m_pipeline), bin);
    gst_object_unref(bin);
    bin = nullptr;
}

void CallEngine::setCameraEnabled(bool on)
{
    if (!m_pipeline || on == m_camera) return;
    if (!m_connected) { qCInfo(lcCallEngine) << "camera: not connected yet, ignored"; return; }
    if (on) {
        // (capsfilter spelled out: a bare "video/x-raw,..." after "device=/dev/video0" does
        // not parse inside gst_parse_bin_from_description.)
        const QByteArray description = QStringLiteral("v4l2src device=%1 ! videoconvert ! videoscale ! videorate "
                                                      "! capsfilter caps=\"video/x-raw,format=I420,width=%2,height=%3,framerate=%4/1\"")
                                           .arg(MediaDevices::cameraToUse(QString::fromLocal8Bit(qgetenv("KOLLEGA_CAMERA")), MediaDevices::selectedIfPresent(MediaDevices::Kind::Camera)))
                                           .arg(kVideoWidth).arg(kVideoHeight).arg(kVideoFps).toLatin1();
        if (!attachSource(m_cameraBin, m_cameraPad, m_videoSelector, m_videoValve, description)) {
            emit failed(tr("Camera not available"));
            return;
        }
        qCInfo(lcCallEngine) << "camera on";
        ensureSendable(1);
    } else {
        detachSource(m_cameraBin, m_cameraPad, m_videoSelector, m_videoValve);
        qCInfo(lcCallEngine) << "camera off";
    }
    m_camera = on;
    emit cameraChanged(on);
}

void CallEngine::startScreenShare(int fd, unsigned nodeId)
{
    if (!m_pipeline || m_share || !m_connected) { qCInfo(lcCallEngine) << "screen sharing: not connected yet, ignored"; if (fd >= 0) ::close(fd); return; }
    // The portal's stream, letterboxed (add-borders) into the fixed size the encoder expects;
    // without a stream (fd < 0: the loopback test) a moving test pattern stands in.
    const QString source = fd < 0 ? QStringLiteral("videotestsrc is-live=true pattern=ball")
                                  : QStringLiteral("pipewiresrc fd=%1 path=%2 do-timestamp=true").arg(fd).arg(nodeId);
    const QByteArray description = (source + QStringLiteral(" ! videoconvert ! videoscale add-borders=true ! videorate "
                                                            "! capsfilter caps=\"video/x-raw,format=I420,width=%1,height=%2,framerate=%3/1\"")
                                       .arg(kShareWidth).arg(kShareHeight).arg(kShareFps)).toLatin1();
    if (!attachSource(m_shareBin, m_sharePad, m_shareSelector, m_shareValve, description)) {
        if (fd >= 0) ::close(fd);
        emit failed(tr("Screen sharing not started"));
        return;
    }
    m_shareFd = fd;
    m_share = true;
    qCInfo(lcCallEngine) << "screen sharing on, node" << nodeId;
    ensureSendable(2);
    emit screenShareChanged(true);
}

void CallEngine::stopScreenShare()
{
    if (!m_pipeline || !m_share || !m_connected) return;
    detachSource(m_shareBin, m_sharePad, m_shareSelector, m_shareValve);
    if (m_shareFd >= 0) { ::close(m_shareFd); m_shareFd = -1; }
    m_share = false;
    qCInfo(lcCallEngine) << "screen sharing off";
    emit screenShareChanged(false);
}

GstCaps *CallEngine::rewriteVideoCaps(const GstCaps *caps) const
{
    GstCaps *copy = gst_caps_copy(caps);
    const QByteArray profile = m_videoProfile.toLatin1();
    for (guint i = 0; i < gst_caps_get_size(copy); ++i) {
        GstStructure *s = gst_caps_get_structure(copy, i);
        gst_structure_remove_field(s, "sprop-parameter-sets");
        gst_structure_set(s, "packetization-mode", G_TYPE_STRING, "1", "profile-level-id", G_TYPE_STRING, profile.constData(),
                          "level-asymmetry-allowed", G_TYPE_STRING, "1", nullptr);
    }
    return copy;
}

QString CallEngine::mediaMid(int videoIndex) const
{
    return SdpDialect::mediaMid(m_localSdp, videoIndex);
}
QString CallEngine::localVideoMsid() const
{
    return SdpDialect::localVideoMsid(m_localSdp);
}
void CallEngine::deliverFrame(GstSample *sample, Frame which)
{
    // On a GStreamer streaming thread: copy the planes into a QVideoFrame, hand it to the
    // sink on the main thread. A frame still waiting there is enough: skip this one.
    std::atomic<bool> &pending = which == Frame::Remote ? m_remotePending : which == Frame::Share ? m_sharePending : m_localPending;
    const QPointer<QVideoSink> &sinkRef = which == Frame::Remote ? m_remoteSink : which == Frame::Share ? m_shareSink : m_localSink;
    if (which == Frame::Remote) {
        ++m_remoteFrames;
        m_lastRemoteFrameMs = QDateTime::currentMSecsSinceEpoch();
    } else if (which == Frame::Share) {
        ++m_shareFrames;
        m_lastShareFrameMs = QDateTime::currentMSecsSinceEpoch();
    }
    // "Video started" is told whether or not a sink is attached yet (the UI opens the
    // video page on it, and only then attaches the sinks).
    if (which != Frame::Local) {
        QMetaObject::invokeMethod(this, [this, which] {
            if (which == Frame::Remote && !m_remoteVideo) {
                m_remoteVideo = true;
                qCInfo(lcCallEngine) << "remote video started";
                emit remoteVideoChanged(true);
            } else if (which == Frame::Share && !m_remoteShare) {
                m_remoteShare = true;
                qCInfo(lcCallEngine) << "remote screen sharing started";
                emit remoteShareChanged(true);
            }
        }, Qt::QueuedConnection);
    }
    if (pending.load() || sinkRef.isNull()) return;
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) return;
    GstVideoFrame vframe;
    if (!gst_video_frame_map(&vframe, &info, buffer, GST_MAP_READ)) return;
    QVideoFrame frame(QVideoFrameFormat(QSize(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info)), QVideoFrameFormat::Format_YUV420P));
    if (frame.map(QVideoFrame::WriteOnly)) {
        for (int plane = 0; plane < 3; ++plane) {
            const auto *src = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, plane));
            const int srcStride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, plane);
            const int rows = GST_VIDEO_FRAME_COMP_HEIGHT(&vframe, plane);
            const int width = GST_VIDEO_FRAME_COMP_WIDTH(&vframe, plane);
            uchar *dst = frame.bits(plane);
            const int dstStride = frame.bytesPerLine(plane);
            for (int y = 0; y < rows; ++y) memcpy(dst + y * dstStride, src + y * srcStride, std::min(width, dstStride));
        }
        frame.unmap();
        pending = true;
        QMetaObject::invokeMethod(this, [this, frame, which] {
            QVideoSink *sink = which == Frame::Remote ? m_remoteSink.data() : which == Frame::Share ? m_shareSink.data() : m_localSink.data();
            if (sink) sink->setVideoFrame(frame);
            (which == Frame::Remote ? m_remotePending : which == Frame::Share ? m_sharePending : m_localPending) = false;
        }, Qt::QueuedConnection);
    }
    gst_video_frame_unmap(&vframe);
}

void CallEngine::setMuted(bool muted)
{
    if (m_volume) g_object_set(m_volume, "mute", muted ? TRUE : FALSE, nullptr);
}

void CallEngine::onNegotiationNeeded(GstElement *webrtc, gpointer self)
{
    // Only the first offer comes from here: as answerer the offer is the other side's, and
    // later changes (ensureSendable) make their offer explicitly.
    if (static_cast<CallEngine *>(self)->m_answerer || static_cast<CallEngine *>(self)->m_offerSent) return;
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void CallEngine::onOfferCreated(GstPromise *promise, gpointer selfPtr)
{
    auto *self = static_cast<CallEngine *>(selfPtr);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);
    if (!offer) {
        QMetaObject::invokeMethod(self, [self] { emit self->failed(tr("WebRTC offer not created")); }, Qt::QueuedConnection);
        return;
    }
    g_signal_emit_by_name(self->m_webrtc, "set-local-description", offer, nullptr);
    gchar *text = gst_sdp_message_as_text(offer->sdp);
    const QString sdp = QString::fromUtf8(text);
    g_free(text);
    gst_webrtc_session_description_free(offer);
    QMetaObject::invokeMethod(self, [self, sdp] {
        self->m_offerSdp = sdp;
        self->m_localSdp = sdp;
        self->m_gatherTimeout.start();
    }, Qt::QueuedConnection);
}

void CallEngine::ensureSendable(int videoIndex)
{
    if (!m_webrtc) return;
    GstWebRTCRTPTransceiver *transceiver = nullptr;
    g_signal_emit_by_name(m_webrtc, "get-transceiver", videoIndex, &transceiver);
    if (!transceiver) return;
    GstWebRTCRTPTransceiverDirection current;
    g_object_get(transceiver, "current-direction", &current, nullptr);
    const bool sendable = current == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV || current == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY
                          || current == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_NONE; // not negotiated yet: the first offer/answer will say
    if (!sendable) {
        qCInfo(lcCallEngine) << "video section" << videoIndex << "negotiated direction" << int(current) << "(2 inactive,3 sendonly,4 recvonly): renegotiating";
        g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV, nullptr);
        GstPromise *promise = gst_promise_new_with_change_func(onReofferCreated, this, nullptr);
        g_signal_emit_by_name(m_webrtc, "create-offer", nullptr, promise);
    }
    gst_object_unref(transceiver);
}

void CallEngine::onReofferCreated(GstPromise *promise, gpointer selfPtr)
{
    auto *self = static_cast<CallEngine *>(selfPtr);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    if (reply) gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);
    if (!offer) { qCWarning(lcCallEngine) << "renegotiation offer not created"; return; }
    g_signal_emit_by_name(self->m_webrtc, "set-local-description", offer, nullptr);
    gchar *text = gst_sdp_message_as_text(offer->sdp);
    const QString sdp = QString::fromUtf8(text);
    g_free(text);
    gst_webrtc_session_description_free(offer);
    QMetaObject::invokeMethod(self, [self, sdp] {
        self->m_localSdp = sdp;
        qCInfo(lcCallEngine) << "renegotiation offer ready";
        emit self->reofferReady(self->toTeamsDialect(sdp));
    }, Qt::QueuedConnection);
}

void CallEngine::onIceCandidate(GstElement *, guint, gchar *candidate, gpointer selfPtr)
{
    auto *self = static_cast<CallEngine *>(selfPtr);
    // An empty candidate marks the end of gathering: not a line to send.
    const QString c = QString::fromUtf8(candidate ? candidate : "").trimmed();
    if (c.isEmpty()) return;
    QMetaObject::invokeMethod(self, [self, c] { self->m_candidates << c; }, Qt::QueuedConnection);
}

void CallEngine::onGatheringState(GstElement *webrtc, GParamSpec *, gpointer selfPtr)
{
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    auto *self = static_cast<CallEngine *>(selfPtr);
    QMetaObject::invokeMethod(self, [self] { self->m_gatheringComplete = true; self->sendPending(); }, Qt::QueuedConnection);
}

QString CallEngine::toTeamsDialect(const QString &input) const
{
    return SdpDialect::toTeams(input, m_candidates, m_rejectedSections);
}
void CallEngine::sendOffer()
{
    if (m_offerSent || m_offerSdp.isEmpty()) return;
    m_offerSent = true;
    m_gatherTimeout.stop();
    qCInfo(lcCallEngine) << "offer ready with" << m_candidates.size() << "candidates";
    emit offerReady(toTeamsDialect(m_offerSdp));
}

void CallEngine::sendPending()
{
    // Whatever waits for candidates: the first offer, or an answer (incoming call).
    if (!m_offerSent && !m_offerSdp.isEmpty()) sendOffer();
    if (!m_answerSdp.isEmpty()) {
        m_gatherTimeout.stop();
        const QString sdp = m_answerSdp;
        m_answerSdp.clear();
        qCInfo(lcCallEngine) << "answer ready with" << m_candidates.size() << "candidates";
        const QString answer = toTeamsDialect(sdp);
        static const QRegularExpression secret(QStringLiteral("(ice-pwd:|ice-ufrag:|fingerprint:sha-256 )\\S+"));
        QString masked = answer;
        masked.replace(secret, QStringLiteral("\\1<x>"));
        qCInfo(lcCallEngine).noquote() << "answer SDP (as sent):\n" << masked;
        emit answerReady(answer);
    }
}

QString CallEngine::fromTeamsDialect(const QString &sdp, bool alignProfile)
{
    SdpDialect::FromTeams result = SdpDialect::fromTeams(sdp, alignProfile, m_videoPt, m_videoProfile);
    m_rejectedSections = result.rejected;
    if (!m_rejectedSections.isEmpty()) qCInfo(lcCallEngine) << "offer sections set aside:" << m_rejectedSections.size();
    return result.sdp;
}
void CallEngine::setRemoteAnswer(const QString &sdp)
{
    if (!m_webrtc) return;
    const QString text = fromTeamsDialect(sdp, true);
    {
        static const QRegularExpression secret(QStringLiteral("(ice-pwd:|ice-ufrag:|fingerprint:sha-256 )\\S+"));
        QString masked = text;
        masked.replace(secret, QStringLiteral("\\1<x>"));
        qCInfo(lcCallEngine).noquote() << "answer SDP (as applied):\n" << masked;
    }
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(text.toUtf8().constData(), &message) != GST_SDP_OK) {
        emit failed(tr("invalid SDP answer"));
        return;
    }
    GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, message);
    GstPromise *applied = gst_promise_new_with_change_func([](GstPromise *p, gpointer selfPtr) {
        const GstStructure *reply = gst_promise_get_reply(p);
        QString error;
        if (reply) { const gchar *e = gst_structure_get_string(reply, "error"); if (e) error = QString::fromUtf8(e); else { GError *ge = nullptr; if (gst_structure_get(reply, "error", G_TYPE_ERROR, &ge, nullptr) && ge) { error = QString::fromUtf8(ge->message); g_error_free(ge); } } }
        gst_promise_unref(p);
        QMetaObject::invokeMethod(static_cast<CallEngine *>(selfPtr), [selfPtr, error] {
            if (error.isEmpty()) qCInfo(lcCallEngine) << "remote answer applied";
            else { qCWarning(lcCallEngine) << "remote answer REFUSED by webrtcbin:" << error; emit static_cast<CallEngine *>(selfPtr)->failed(tr("SDP answer not applicable: ") + error); }
        }, Qt::QueuedConnection);
    }, this, nullptr);
    g_signal_emit_by_name(m_webrtc, "set-remote-description", answer, applied);
    gst_webrtc_session_description_free(answer);
}

void CallEngine::setRemoteOffer(const QString &sdp)
{
    if (!m_webrtc) return;
    // A renegotiation offer while we are the offerer lists Teams' own H.264 profile: ours is
    // put in its place, or webrtcbin rejects the video sections (SdpParsingFailure ends the call).
    // As the answerer the profile follows what the remote offered, so it is left alone.
    const QString text = fromTeamsDialect(sdp, !m_answerer);
    {
        static const QRegularExpression secret(QStringLiteral("(ice-pwd:|ice-ufrag:|fingerprint:sha-256 )\\S+"));
        QString masked = text;
        masked.replace(secret, QStringLiteral("\\1<x>"));
        qCInfo(lcCallEngine).noquote() << "remote offer (as applied):\n" << masked;
    }
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(text.toUtf8().constData(), &message) != GST_SDP_OK) {
        emit failed(tr("invalid renegotiation offer"));
        return;
    }
    GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, message);
    GstPromise *applied = gst_promise_new_with_change_func([](GstPromise *p, gpointer selfPtr) {
        gst_promise_unref(p);
        auto *self = static_cast<CallEngine *>(selfPtr);
        // As the answerer our video transceivers come out of the offer as receive-only or
        // inactive (Teams offers "recvonly" for its side), so the answer would say "inactive"
        // and the camera's and the screen's packets would never leave: ask for send and receive.
        if (self->m_answerer) {
            for (int index = 0; index < 8; ++index) {
                GstWebRTCRTPTransceiver *transceiver = nullptr;
                g_signal_emit_by_name(self->m_webrtc, "get-transceiver", index, &transceiver);
                if (!transceiver) break;
                guint mline = G_MAXUINT;
                g_object_get(transceiver, "mlineindex", &mline, nullptr);
                if (mline == 1 || mline == 2) g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV, nullptr);
                gst_object_unref(transceiver);
            }
        }
        GstPromise *answer = gst_promise_new_with_change_func(onAnswerCreated, self, nullptr);
        g_signal_emit_by_name(self->m_webrtc, "create-answer", nullptr, answer);
    }, this, nullptr);
    g_signal_emit_by_name(m_webrtc, "set-remote-description", offer, applied);
    gst_webrtc_session_description_free(offer);
}

void CallEngine::onAnswerCreated(GstPromise *promise, gpointer selfPtr)
{
    auto *self = static_cast<CallEngine *>(selfPtr);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = nullptr;
    if (reply) gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    gst_promise_unref(promise);
    if (!answer) {
        QMetaObject::invokeMethod(self, [self] { emit self->failed(tr("renegotiation answer not created")); }, Qt::QueuedConnection);
        return;
    }
    g_signal_emit_by_name(self->m_webrtc, "set-local-description", answer, nullptr);
    gchar *text = gst_sdp_message_as_text(answer->sdp);
    const QString sdp = QString::fromUtf8(text);
    g_free(text);
    gst_webrtc_session_description_free(answer);
    QMetaObject::invokeMethod(self, [self, sdp] {
        // With candidates already gathered (renegotiation) the answer goes out now;
        // for an incoming call it waits for ICE gathering, as the offer does.
        self->m_answerSdp = sdp;
        self->m_localSdp = sdp;
        if (self->m_gatheringComplete || !self->m_candidates.isEmpty()) self->sendPending();
        else self->m_gatherTimeout.start();
    }, Qt::QueuedConnection);
}

void CallEngine::onConnectionState(GstElement *webrtc, GParamSpec *, gpointer selfPtr)
{
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc, "connection-state", &state, nullptr);
    QString name;
    switch (state) {
    case GST_WEBRTC_PEER_CONNECTION_STATE_NEW: name = QStringLiteral("new"); break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING: name = QStringLiteral("connecting"); break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED: name = QStringLiteral("connected"); break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED: name = QStringLiteral("disconnected"); break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED: name = QStringLiteral("failed"); break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED: name = QStringLiteral("closed"); break;
    }
    auto *self = static_cast<CallEngine *>(selfPtr);
    qCInfo(lcCallEngine) << "connection" << name;
    QMetaObject::invokeMethod(self, [self, name] {
        self->m_connected = name == QLatin1String("connected");
        emit self->connectionStateChanged(name);
    }, Qt::QueuedConnection);
}

void CallEngine::onIncomingPad(GstElement *, GstPad *pad, gpointer selfPtr)
{
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    auto *self = static_cast<CallEngine *>(selfPtr);
    if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
        gchar *str = gst_caps_to_string(caps);
        qCInfo(lcCallEngine) << "incoming stream pad" << GST_PAD_NAME(pad) << str;
        g_free(str);
        gst_caps_unref(caps);
    } else {
        qCInfo(lcCallEngine) << "incoming stream pad" << GST_PAD_NAME(pad) << "(caps not yet known)";
    }
    // Decode whatever was negotiated (Opus, H.264). The pad is "src_<mline>": 1 is the
    // camera section, 2 the screen one; the decodebin remembers which.
    GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
    const int mline = QString::fromLatin1(GST_PAD_NAME(pad)).section(QLatin1Char('_'), -1).toInt();
    g_object_set_data(G_OBJECT(decodebin), "mline", GINT_TO_POINTER(mline));
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(onDecodedPad), self);
    gst_bin_add(GST_BIN(self->m_pipeline), decodebin);
    gst_element_sync_state_with_parent(decodebin);
    GstPad *sink = gst_element_get_static_pad(decodebin, "sink");
    gst_pad_link(pad, sink);
    gst_object_unref(sink);
}

void CallEngine::onDecodedPad(GstElement *decodebin, GstPad *pad, gpointer selfPtr)
{
    auto *self = static_cast<CallEngine *>(selfPtr);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    const QString name = caps ? QString::fromLatin1(gst_structure_get_name(gst_caps_get_structure(caps, 0))) : QString();
    if (caps) gst_caps_unref(caps);
    if (name.startsWith(QLatin1String("video/"))) {
        // Remote video: I420 frames to the QML sink through an appsink (no GStreamer video
        // sink for Qt Quick is installed; the copy is cheap at call sizes).
        const int mline = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(decodebin), "mline"));
        GstElement *out = gst_parse_bin_from_description(
            "queue max-size-buffers=3 leaky=downstream ! videoconvert ! video/x-raw,format=I420 "
            "! appsink name=remotesink emit-signals=true sync=true drop=true max-buffers=2", TRUE, nullptr);
        if (!out) return;
        if (GstElement *sink = gst_bin_get_by_name(GST_BIN(out), "remotesink")) {
            g_object_set_data(G_OBJECT(sink), "mline", GINT_TO_POINTER(mline));
            g_signal_connect(sink, "new-sample", G_CALLBACK(+[](GstElement *s, gpointer selfPtr) -> GstFlowReturn {
                GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(s));
                if (!sample) return GST_FLOW_OK;
                const int line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(s), "mline"));
                static_cast<CallEngine *>(selfPtr)->deliverFrame(sample, line == 2 ? Frame::Share : Frame::Remote);
                gst_sample_unref(sample);
                return GST_FLOW_OK;
            }), self);
            gst_object_unref(sink);
        }
        gst_bin_add(GST_BIN(self->m_pipeline), out);
        gst_element_sync_state_with_parent(out);
        GstPad *sink = gst_element_get_static_pad(out, "sink");
        gst_pad_link(pad, sink);
        gst_object_unref(sink);
        qCInfo(lcCallEngine) << "remote video stream decoding, section" << mline;
        return;
    }
    if (!name.startsWith(QLatin1String("audio/"))) return;
    // Remote audio joins the playback mixer (through the echo probe to the speakers).
    GstElement *out = gst_parse_bin_from_description("queue ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=1", TRUE, nullptr);
    GstElement *mixer = gst_bin_get_by_name(GST_BIN(self->m_pipeline), "playmix");
    if (!out || !mixer) return;
    gst_bin_add(GST_BIN(self->m_pipeline), out);
    gst_element_sync_state_with_parent(out);
    GstPad *sink = gst_element_get_static_pad(out, "sink");
    gst_pad_link(pad, sink);
    gst_object_unref(sink);
    GstPad *mixIn = gst_element_request_pad_simple(mixer, "sink_%u");
    GstPad *outSrc = gst_element_get_static_pad(out, "src");
    gst_pad_link(outSrc, mixIn);
    gst_object_unref(outSrc);
    gst_object_unref(mixIn);
    gst_object_unref(mixer);
    qCInfo(lcCallEngine) << "remote audio playing";
}

TonePlayer::TonePlayer(QObject *parent) : QObject(parent) {}

TonePlayer::~TonePlayer() { stop(); }

void TonePlayer::play(const QString &path, bool loop)
{
    stop();
    m_playbin = gst_element_factory_make("playbin", nullptr);
    if (!m_playbin) return;
    g_object_set(m_playbin, "uri", QUrl::fromLocalFile(path).toString().toUtf8().constData(), nullptr);
    g_object_set_data(G_OBJECT(m_playbin), "kollega-loop", GINT_TO_POINTER(loop ? 1 : 0));
    GstBus *bus = gst_element_get_bus(m_playbin);
    gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer element) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) != GST_MESSAGE_EOS) return TRUE;
        if (g_object_get_data(G_OBJECT(element), "kollega-loop"))
            // Loop: back to the start at the end of the file.
            gst_element_seek_simple(GST_ELEMENT(element), GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
        else
            gst_element_set_state(GST_ELEMENT(element), GST_STATE_NULL); // a cue ends with its sound
        return TRUE;
    }, m_playbin);
    gst_object_unref(bus);
    gst_element_set_state(m_playbin, GST_STATE_PLAYING);
}

void TonePlayer::stop()
{
    if (!m_playbin) return;
    GstBus *bus = gst_element_get_bus(m_playbin);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(m_playbin, GST_STATE_NULL);
    gst_object_unref(m_playbin);
    m_playbin = nullptr;
}

void CallEngine::logStats()
{
    if (!m_webrtc) return;
    // Answered on a GStreamer thread: summarise there, log through the Qt side.
    GstPromise *promise = gst_promise_new_with_change_func([](GstPromise *p, gpointer selfPtr) {
        const GstStructure *reply = gst_promise_get_reply(p);
        struct Summary { quint64 sent = 0, received = 0; qint64 lostAtPeer = -1; double rtt = -1; bool peerReport = false; } sum;
        if (reply) {
            gst_structure_foreach_id_str(reply, [](const GstIdStr *, const GValue *v, gpointer data) -> gboolean {
                if (!GST_VALUE_HOLDS_STRUCTURE(v)) return TRUE;
                const GstStructure *s = gst_value_get_structure(v);
                GstWebRTCStatsType type;
                if (!gst_structure_get(s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr)) return TRUE;
                auto *sum = static_cast<Summary *>(data);
                guint64 n = 0;
                if (type == GST_WEBRTC_STATS_OUTBOUND_RTP && gst_structure_get_uint64(s, "packets-sent", &n)) sum->sent += n;
                if (type == GST_WEBRTC_STATS_INBOUND_RTP && gst_structure_get_uint64(s, "packets-received", &n)) sum->received += n;
                if (type == GST_WEBRTC_STATS_REMOTE_INBOUND_RTP) {
                    // The peer's RTCP receiver report about our stream: it is getting our packets.
                    sum->peerReport = true;
                    gint64 lost = 0;
                    if (gst_structure_get_int64(s, "packets-lost", &lost)) sum->lostAtPeer = lost;
                    double rtt = 0;
                    if (gst_structure_get_double(s, "round-trip-time", &rtt)) sum->rtt = rtt;
                }
                return TRUE;
            }, &sum);
        }
        gst_promise_unref(p);
        auto *self = static_cast<CallEngine *>(selfPtr);
        const quint64 mic = self->m_micPackets.load(), raw = self->m_micRaw.load();
        const quint64 video = self->m_videoPackets.load(), frames = self->m_remoteFrames.load();
        const quint64 share = self->m_sharePackets.load(), shareFrames = self->m_shareFrames.load();
        QMetaObject::invokeMethod(self, [sum, mic, raw, video, frames, share, shareFrames] {
            qCInfo(lcCallEngine).noquote() << QStringLiteral("stats: mic raw %6 bufs, mic chain %5 pkts, video out %7 pkts, video in %8 frames, screen out %9 pkts, screen in %10 frames, sent %1 pkts, received %2 pkts, peer reports our stream: %3%4")
                .arg(sum.sent).arg(sum.received).arg(sum.peerReport ? QStringLiteral("yes") : QStringLiteral("NO"),
                     sum.peerReport ? QStringLiteral(" (lost %1, rtt %2 ms)").arg(sum.lostAtPeer).arg(int(sum.rtt * 1000)) : QString())
                .arg(mic).arg(raw).arg(video).arg(frames).arg(share).arg(shareFrames);
        }, Qt::QueuedConnection);
    }, this, nullptr);
    g_signal_emit_by_name(m_webrtc, "get-stats", nullptr, promise);
}
