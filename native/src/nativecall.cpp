#include "nativecall.h"
#include "calljson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

Q_LOGGING_CATEGORY(lcNativeCall, "kollega.nativecall", QtInfoMsg)

using namespace CallJson;

namespace {
constexpr auto kConversationService = "https://api.flightproxy.teams.microsoft.com/api/v2/epconv";
constexpr auto kTrap = "https://teams.microsoft.com/trap/tokens";
// Teams' test-call bot (the one Teams web adds to "UserInitiatedTestCall" conversations).
constexpr auto kTestBotMri = "28:cf28171e-fcfd-47e4-a1d6-79460b0b3ca0";
constexpr auto kRingtone = "/usr/share/sounds/ocean/stereo/phone-incoming-call.oga";
constexpr auto kRingback = "/usr/share/sounds/freedesktop/stereo/phone-outgoing-calling.oga";
// Short cues: the call is connected (the ringing stops and someone can be heard), and it ended.
constexpr auto kConnectedCue = "/usr/share/sounds/freedesktop/stereo/service-login.oga";
constexpr auto kEndedCue = "/usr/share/sounds/freedesktop/stereo/service-logout.oga";
constexpr int kEndpointCapabilities = 73463;
constexpr int kClientEndpointCapabilities = 248477418;
// Relays seen in Teams web's RTCPeerConnection configuration (EU region).
const QStringList kRelays{QStringLiteral("turn://%1:%2@gateway-eu.az.relay.teams.cloud.microsoft:3478?transport=udp"),
                          QStringLiteral("turn://%1:%2@gateway-eu.az.relay.teams.cloud.microsoft:443?transport=tcp"),
                          QStringLiteral("turns://%1:%2@gateway-eu.az.relay.teams.cloud.microsoft:443?transport=tcp")};

QString hex8() { return QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, QLatin1Char('0')); }
}

NativeCall::NativeCall(QObject *parent) : QObject(parent)
{
    connect(&m_engine, &CallEngine::offerReady, this, &NativeCall::onOffer);
    connect(&m_engine, &CallEngine::answerReady, this, [this](const QString &sdp) {
        const QJsonObject clientContent{
            {QStringLiteral("controlVideoStreaming"), link(QStringLiteral("call"), QStringLiteral("controlVideoStreaming"))},
            {QStringLiteral("csrcInfo"), link(QStringLiteral("call"), QStringLiteral("csrcInfo"))}};
        if (m_renegotiationAnswerLink.isEmpty()) {
            // Incoming call: accept with our answer (Teams web: qX()).
            const QString acceptance = m_invitationLinks.value(QLatin1String("acceptance")).toString();
            if (acceptance.isEmpty()) return;
            QJsonObject mediaContent{{QStringLiteral("blob"), sdp}, {QStringLiteral("contentType"), QStringLiteral("application/sdp-ngc-1.0")}};
            if (!m_remoteLegId.isEmpty()) mediaContent.insert(QStringLiteral("mediaLegId"), m_remoteLegId);
            const QJsonObject body{{QStringLiteral("callAcceptance"), QJsonObject{
                {QStringLiteral("acceptedBy"), selfParticipant()},
                {QStringLiteral("acceptedCallModalities"), QJsonArray{QStringLiteral("Audio")}},
                {QStringLiteral("capabilities"), QJsonValue::Null},
                {QStringLiteral("endpointCapabilities"), kEndpointCapabilities},
                {QStringLiteral("clientEndpointCapabilities"), kClientEndpointCapabilities},
                {QStringLiteral("links"), acceptanceLinks()},
                {QStringLiteral("clientContentForMediaController"), clientContent},
                {QStringLiteral("mediaContent"), mediaContent},
                {QStringLiteral("pstnContent"), QJsonValue::Null},
                {QStringLiteral("callKeepAliveInterval"), QJsonValue::Null}}}};
            postJson(QStringLiteral("acceptance"), "POST", acceptance, body, [this](const QJsonObject &response) {
                qCInfo(lcNativeCall).noquote() << "acceptance response" << shape(response).left(600);
                rememberMediaLinks(response);
                if (m_mediaLinks.contains(QLatin1String("applyChannelParameters"))) sendMaxVideoSendCapabilities();
            });
            setState(tr("Connecting audio…"));
            return;
        }
        // Teams web: _X() -> {mediaAnswer: {callModalities, sender, links, clientContentForMediaController, mediaContent}}
        QJsonObject mediaContent{{QStringLiteral("blob"), sdp}, {QStringLiteral("contentType"), QStringLiteral("application/sdp-ngc-1.0")}};
        if (!m_renegotiationLegId.isEmpty()) mediaContent.insert(QStringLiteral("mediaLegId"), m_renegotiationLegId);
        const QJsonObject body{{QStringLiteral("mediaAnswer"), QJsonObject{
            {QStringLiteral("callModalities"), QJsonArray{QStringLiteral("Audio")}},
            {QStringLiteral("sender"), selfParticipant()},
            {QStringLiteral("links"), QJsonObject{{QStringLiteral("mediaAcknowledgement"), link(QStringLiteral("call"), QStringLiteral("mediaAcknowledgement"))}}},
            {QStringLiteral("clientContentForMediaController"), clientContent},
            {QStringLiteral("mediaContent"), mediaContent}}},
            {QStringLiteral("debugContent"), QJsonObject{{QStringLiteral("endpointId"), m_id.endpointId}}}};
        postJson(QStringLiteral("mediaAnswer (renegotiation)"), "POST", m_renegotiationAnswerLink, body);
        m_renegotiationAnswerLink.clear();
    });
    connect(&m_engine, &CallEngine::failed, this, [this](const QString &m) { finish(tr("Audio error: ") + m); });
    connect(&m_engine, &CallEngine::reofferReady, this, [this](const QString &sdp) {
        // Teams web (vX): a renegotiation we start goes to the call's mediaRenegotiation
        // link; the answer comes back on our mediaAnswer link, a refusal on rejection.
        const QString url = m_mediaLinks.value(QLatin1String("mediaRenegotiation")).toString(m_callLinks.value(QLatin1String("mediaRenegotiation")).toString());
        if (url.isEmpty()) { qCWarning(lcNativeCall) << "renegotiation: no mediaRenegotiation link"; return; }
        QJsonObject mediaContent{{QStringLiteral("blob"), sdp}, {QStringLiteral("contentType"), QStringLiteral("application/sdp-ngc-1.0")}};
        if (!m_remoteLegId.isEmpty()) mediaContent.insert(QStringLiteral("mediaLegId"), m_remoteLegId);
        const QJsonObject body{{QStringLiteral("mediaNegotiation"), QJsonObject{
            {QStringLiteral("callModalities"), QJsonArray{QStringLiteral("Audio"), QStringLiteral("Video")}},
            {QStringLiteral("sender"), selfParticipant()},
            {QStringLiteral("links"), QJsonObject{{QStringLiteral("mediaAnswer"), link(QStringLiteral("call"), QStringLiteral("mediaAnswer"))},
                                                  {QStringLiteral("rejection"), link(QStringLiteral("call"), QStringLiteral("rejection"))}}},
            {QStringLiteral("mediaContent"), mediaContent}}}};
        postJson(QStringLiteral("mediaRenegotiation (ours)"), "POST", url, body, [this](const QJsonObject &response) {
            qCInfo(lcNativeCall).noquote() << "renegotiation response" << shape(response).left(400);
            rememberMediaLinks(response);
        });
    });
    connect(&m_engine, &CallEngine::remoteVideoChanged, this, &NativeCall::videoChanged);
    connect(&m_engine, &CallEngine::cameraChanged, this, &NativeCall::videoChanged);
    connect(&m_engine, &CallEngine::remoteShareChanged, this, &NativeCall::videoChanged);
    connect(&m_engine, &CallEngine::screenShareChanged, this, &NativeCall::videoChanged);
    connect(&m_engine, &CallEngine::connectionStateChanged, this, [this](const QString &s) {
        if (s == QLatin1String("connected")) {
            m_tone.stop();
            if (!m_connected) m_cue.play(QString::fromLatin1(kConnectedCue), false);
            m_connected = true;
            setState(tr("In a call"));
        }
        else if (s == QLatin1String("failed")) finish(tr("Media connection failed"));
    });
}

void NativeCall::setState(const QString &state)
{
    if (m_state == state) return;
    m_state = state;
    qCInfo(lcNativeCall) << "state:" << state;
    emit stateChanged();
}

void NativeCall::finish(const QString &state)
{
    m_tone.stop();
    // A call that had been connected ends with a cue (once: finish() can be reached twice).
    if (m_connected) m_cue.play(QString::fromLatin1(kEndedCue), false);
    m_connected = false;
    m_engine.stop();
    setState(state);
    if (m_incoming) { m_incoming = false; emit incomingChanged(); }
    if (m_active) { m_active = false; emit activeChanged(); }
}

QString NativeCall::link(const QString &kind, const QString &name) const
{
    // Same shape as Teams web's links: <surl>callAgent/<agent>/<8 hex>/<call|conversation>/<event>/
    return m_id.trouterUrl + QStringLiteral("callAgent/%1/%2/%3/%4/").arg(m_agentId, hex8(), kind, name);
}

QJsonObject NativeCall::selfParticipant() const
{
    return {{QStringLiteral("id"), m_id.selfMri}, {QStringLiteral("displayName"), m_id.displayName},
            {QStringLiteral("endpointId"), m_id.endpointId}, {QStringLiteral("participantId"), m_participantId},
            {QStringLiteral("languageId"), QStringLiteral("it-it")}};
}

void NativeCall::postJson(const QString &context, const QByteArray &verb, const QString &url, const QJsonObject &body,
                          std::function<void(const QJsonObject &)> onReply)
{
    if (url.isEmpty()) { qCWarning(lcNativeCall) << context << "has no link"; return; }
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
    req.setRawHeader("X-MS-Migration", "True");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, verb, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, context, onReply] {
        const QByteArray data = reply->readAll();
        qCInfo(lcNativeCall).noquote() << context << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                                       << (reply->error() != QNetworkReply::NoError ? QString::fromUtf8(data.left(300)) : QString());
        if (onReply && reply->error() == QNetworkReply::NoError) onReply(QJsonDocument::fromJson(data).object());
        reply->deleteLater();
    });
}

void NativeCall::rememberMediaLinks(const QJsonValue &v)
{
    // What is there, whether or not it is picked up below (the incoming call's links are not yet known).
    for (const char *name : {"applyChannelParameters", "controlVideoStreaming", "mediaRenegotiation"})
        for (const QString &what : describeKey(v, QString::fromLatin1(name)))
            qCInfo(lcNativeCall).noquote() << "media link candidate" << name << "->" << what << "ours:" << (what.contains(m_id.trouterUrl.section(QLatin1Char('/'), 2, 2)));
    // Our own callback links (clientContentForMediaController) point at our Trouter: skipped.
    for (const auto &[key, url] : CallJson::mediaControllerLinks(v, m_id.trouterUrl)) {
        if (m_mediaLinks.value(key).toString() == url) continue;
        qCInfo(lcNativeCall) << "media controller link" << key << url.section(QLatin1Char('/'), 0, 4);
        m_mediaLinks.insert(key, url);
        if (key == QLatin1String("applyChannelParameters")) {
            const QSet<int> pending = m_pendingSources;
            m_pendingSources.clear();
            for (int id : pending) requestRemoteVideo(id, m_screenSources.contains(id));
        }
    }
}

void NativeCall::sendMaxVideoSendCapabilities()
{
    // As captured from Teams web right after the acceptance: one message on the video mid.
    const QJsonObject caps{{QStringLiteral("max-width"), 640}, {QStringLiteral("max-height"), 640}, {QStringLiteral("max-fps"), 15},
                           {QStringLiteral("max-streams"), 1}, {QStringLiteral("max-layers"), 1}, {QStringLiteral("sequence-number"), ++m_videoSequence}};
    const QString parameter = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("maxVideoSendCapabilities"), QJsonObject{{QStringLiteral("caps"), caps}}}}).toJson(QJsonDocument::Compact));
    const QJsonObject body{{QStringLiteral("applyChannelParameters"), QJsonObject{{QStringLiteral("multiChannelParameter"), QJsonObject{
        {QStringLiteral("mids"), QJsonArray{m_engine.videoMid()}}, {QStringLiteral("mediaParameter"), parameter}}}}}};
    postJson(QStringLiteral("maxVideoSendCapabilities"), "POST", m_mediaLinks.value(QLatin1String("applyChannelParameters")).toString(), body);
}

void NativeCall::requestRemoteVideo(int sourceId, bool screen)
{
    if (m_requestedSources.contains(sourceId)) return;
    const QString url = m_mediaLinks.value(QLatin1String("applyChannelParameters")).toString();
    if (url.isEmpty()) { m_pendingSources.insert(sourceId); if (screen) m_screenSources.insert(sourceId); qCInfo(lcNativeCall) << "video source" << sourceId << "noted: no applyChannelParameters link yet"; return; }
    m_requestedSources.insert(sourceId);
    if (screen) m_screenSources.insert(sourceId);
    const QString mid = screen ? m_engine.shareMid() : m_engine.videoMid();
    // Teams web's "source request" (getSignalingSourceRequestMessage): which source we want
    // on which of our sections, and how big; sizes in Teams' units (max-fps 3000 = 30 fps).
    QString msid = m_engine.localVideoMsid();
    if (msid.isEmpty()) msid = m_engine.videoMid();
    const QJsonObject control{{QStringLiteral("sourceId"), sourceId}, {QStringLiteral("streamMsid"), msid},
                              {QStringLiteral("fmtParams"), QStringLiteral("max-fs=3600;max-mbps=108000;max-fps=3000")}};
    const QString parameter = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("controlVideoStreaming"), QJsonObject{
        {QStringLiteral("sequenceNumber"), ++m_videoSequence}, {QStringLiteral("controlInfo"), control}}}}).toJson(QJsonDocument::Compact));
    const QJsonObject body{{QStringLiteral("applyChannelParameters"), QJsonObject{{QStringLiteral("multiChannelParameter"), QJsonObject{
        {QStringLiteral("mids"), QJsonArray{mid}}, {QStringLiteral("mediaParameter"), parameter}}}}}};
    qCInfo(lcNativeCall) << "requesting" << (screen ? "screen" : "video") << "source" << sourceId << "on mid" << mid << "msid" << msid;
    postJson(QStringLiteral("source request"), "POST", url, body);
}

void NativeCall::parseRosterStreams(const QJsonObject &body)
{
    // participants[mri].endpoints[...].mediaStreams[{type, label, sourceId, direction}] (as
    // Teams web's participant manager reads them); logged whole while the shape is learned.
    for (const CallJson::RosterStream &stream : CallJson::rosterStreams(body, m_id.selfMri)) {
        qCInfo(lcNativeCall) << "  stream of" << stream.who.section(QLatin1Char(':'), -1).left(8) << stream.type << stream.label
                             << stream.direction << "source" << stream.sourceId;
        // One source per section: the first camera seen (meetings list many) and the screen.
        if (stream.type == QLatin1String("video") && stream.direction.contains(QLatin1String("send")) && stream.sourceId > 0) {
            const bool screen = stream.label == QLatin1String("applicationsharing-video");
            if (screen || m_requestedSources.isEmpty() || m_requestedSources == m_screenSources) requestRemoteVideo(stream.sourceId, screen);
        }
    }
}

void NativeCall::addTestBot()
{
    // As Teams web does after the conversation exists: publish our endpoint metadata, then
    // add the test bot; the one-to-one thread id is "<self guid>_<bot guid>".
    postJson(QStringLiteral("updateEndpointMetadata"), "PUT", m_links.value(QLatin1String("updateEndpointMetadata")).toString(),
             {{QStringLiteral("participants"), QJsonObject{{QStringLiteral("from"), selfParticipant()}}},
              {QStringLiteral("endpointMetadata"), QJsonObject{{QStringLiteral("holographicCapabilities"), 3}}}});
    const QString selfGuid = m_id.selfMri.section(QLatin1Char(':'), -1);
    const QString botGuid = QString::fromLatin1(kTestBotMri).section(QLatin1Char(':'), -1);
    const QJsonObject body{
        {QStringLiteral("disableUnmute"), false},
        {QStringLiteral("participants"), QJsonObject{{QStringLiteral("from"), selfParticipant()},
            {QStringLiteral("to"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString::fromLatin1(kTestBotMri)},
                                                          {QStringLiteral("participantId"), QUuid::createUuid().toString(QUuid::WithoutBraces)}}}}}},
        {QStringLiteral("participantInvitationData"), QJsonObject{}},
        {QStringLiteral("replacementDetails"), QJsonValue::Null},
        {QStringLiteral("groupContext"), QJsonValue::Null},
        {QStringLiteral("groupChat"), QJsonObject{{QStringLiteral("threadId"), QStringLiteral("19:%1_%2@unq.gbl.spaces").arg(selfGuid, botGuid)},
                                                  {QStringLiteral("messageId"), QJsonValue::Null}}},
        {QStringLiteral("links"), QJsonObject{{QStringLiteral("addParticipantSuccess"), link(QStringLiteral("conversation"), QStringLiteral("addParticipantSuccess"))},
                                              {QStringLiteral("addParticipantFailure"), link(QStringLiteral("conversation"), QStringLiteral("addParticipantFailure"))}}}};
    postJson(QStringLiteral("add (test bot)"), "POST", m_links.value(QLatin1String("addParticipantAndModality")).toString(), body);
}

void NativeCall::joinMeeting(const Identity &identity, const QString &threadId, const QString &tenantId, const QString &organizerId, const QString &subject)
{
    m_callees.clear();
    m_calleeMri = threadId; // not the test call
    m_threadId = threadId;
    m_calleeName = subject.isEmpty() ? tr("meeting") : subject;
    m_meeting = true;
    m_tenantId = tenantId;
    m_organizerId = organizerId;
    begin(identity);
}

void NativeCall::startTestCall(const Identity &identity)
{
    m_meeting = false;
    m_calleeMri.clear();
    m_callees.clear();
    m_threadId.clear();
    m_calleeName.clear();
    begin(identity);
}

void NativeCall::startCall(const Identity &identity, const QStringList &calleeMris, const QString &threadId, const QString &calleeName)
{
    m_meeting = false;
    m_callees = calleeMris;
    m_calleeMri = calleeMris.value(0);
    m_threadId = threadId;
    m_calleeName = calleeName;
    begin(identity);
    // The caller hears it ring from the moment of pressing Call until the other side answers
    // (Teams sends no "progress" for every call); stopped on acceptance, end or failure.
    if (m_active) m_tone.play(QString::fromLatin1(kRingback));
}

void NativeCall::begin(const Identity &identity)
{
    if (m_active) return;
    if (identity.trouterUrl.isEmpty()) { setState(tr("Trouter not connected: the answer could not be received")); return; }
    m_id = identity;
    m_agentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_participantId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_links = {};
    m_mediaLinks = {};
    m_requestedSources.clear();
    m_pendingSources.clear();
    m_screenSources.clear();
    m_videoSequence = 0;
    m_active = true;
    emit activeChanged();
    setState(tr("Preparing…"));

    fetchTurn([this](const QStringList &turn) {
        if (turn.isEmpty()) return;
        setState(tr("Starting audio…"));
        if (!m_engine.start(turn)) finish(tr("Audio not started"));
    });
}

void NativeCall::fetchTurn(std::function<void(const QStringList &)> done)
{
    // TURN credentials (as Teams web's legacy token fetcher: skypetoken + api-version 2).
    QNetworkRequest req{QUrl(QString::fromLatin1(kTrap))};
    req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
    req.setRawHeader("api-version", "2");
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        const QJsonArray tokens = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("tokens")).toArray();
        if (reply->error() != QNetworkReply::NoError || tokens.isEmpty()) {
            finish(tr("TURN credentials not obtained: %1").arg(reply->errorString()));
            done({});
            return;
        }
        const QJsonObject t = tokens.first().toObject();
        const QString user = QString::fromLatin1(QUrl::toPercentEncoding(t.value(QLatin1String("username")).toString()));
        const QString pass = QString::fromLatin1(QUrl::toPercentEncoding(t.value(QLatin1String("password")).toString()));
        QStringList turn;
        for (const QString &relay : kRelays) turn << relay.arg(user, pass);
        done(turn);
    });
}

QJsonObject NativeCall::acceptanceLinks() const
{
    QJsonObject links;
    for (const char *n : {"mediaRenegotiation", "transfer", "replacement", "balanceUpdate", "retargetCompletion",
                          "controlVideoStreaming", "updateMediaDescriptions"})
        links.insert(QString::fromLatin1(n), link(QStringLiteral("call"), QString::fromLatin1(n)));
    return links;
}

void NativeCall::incoming(const Identity &identity, const QJsonObject &notification)
{
    if (m_active) { qCInfo(lcNativeCall) << "incoming call ignored: a call is active"; return; }
    m_id = identity;
    m_agentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_participantId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_links = {};
    m_invitationLinks = {};
    m_mediaLinks = {};
    m_requestedSources.clear();
    m_pendingSources.clear();
    m_screenSources.clear();
    m_videoSequence = 0;
    m_threadId.clear(); // not the thread of an earlier call
    m_callees.clear();
    const IncomingCall invite = parseIncomingCall(notification);
    qCInfo(lcNativeCall) << "incoming call: from" << invite.callerMri.section(QLatin1Char(':'), -1).left(8)
                         << "fields" << notification.keys() << "groupChat" << notification.value(QLatin1String("groupChat")).toObject().keys()
                         << "attach" << !invite.attachUrl.isEmpty() << "offer" << !invite.offer.isEmpty();
    m_calleeMri = invite.callerMri;
    m_calleeName = invite.callerName;
    if (m_calleeName.isEmpty()) m_calleeName = QStringLiteral("Sconosciuto");
    m_remoteOffer = invite.offer;
    m_remoteLegId = invite.legId;
    // The service already names us as a participant of this call: answering with another
    // participantId makes it a second participant of the same user, which "removes" the
    // invited one (call/end subCode 5300).
    if (!invite.participantId.isEmpty()) m_participantId = invite.participantId;
    m_threadId = invite.threadId; // a group call or meeting: its chat, for the Chat button
    const QString attach = invite.attachUrl;
    const QString joinUrl = invite.joinUrl;
    if (attach.isEmpty()) { qCWarning(lcNativeCall) << "incoming call without attach link"; return; }

    m_active = true;
    m_incoming = true;
    emit activeChanged();
    emit incomingChanged();
    setState(tr("Incoming call from %1").arg(m_calleeName));
    m_tone.play(QString::fromLatin1(kRingtone));

    // Teams web: GX() -> attach, with the conversation "join" as an additional action.
    QJsonObject conversationLinks;
    for (const char *n : {"conversationEnd", "conversationUpdate", "localParticipantUpdate", "addParticipantSuccess", "addParticipantFailure", "receiveMessage"})
        conversationLinks.insert(QString::fromLatin1(n), link(QStringLiteral("conversation"), QString::fromLatin1(n)));
    const QJsonObject join{{QStringLiteral("input"), QJsonObject{
                                {QStringLiteral("capabilities"), QJsonValue::Null},
                                {QStringLiteral("endpointCapabilities"), kEndpointCapabilities},
                                {QStringLiteral("conversationRequest"), QJsonObject{
                                    {QStringLiteral("roster"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Delta")},
                                                                           {QStringLiteral("rosterUpdate"), link(QStringLiteral("conversation"), QStringLiteral("rosterUpdate"))}}},
                                    {QStringLiteral("links"), conversationLinks}}},
                                {QStringLiteral("endpointMetadata"), QJsonObject{{QStringLiteral("holographicCapabilities"), 3}}},
                                {QStringLiteral("participants"), QJsonObject{{QStringLiteral("from"), selfParticipant()}}}}},
                           {QStringLiteral("name"), QStringLiteral("join")},
                           {QStringLiteral("url"), joinUrl},
                           {QStringLiteral("waitForResponse"), true}};
    const QJsonObject body{
        {QStringLiteral("attach"), QJsonObject{{QStringLiteral("requireMediaContent"), m_remoteOffer.isEmpty()},
                                               {QStringLiteral("links"), QJsonObject{{QStringLiteral("end"), link(QStringLiteral("call"), QStringLiteral("end"))}}},
                                               {QStringLiteral("locationContent"), QJsonValue::Null},
                                               {QStringLiteral("networkContent"), QJsonValue::Null},
                                               {QStringLiteral("areaContent"), QJsonValue::Null}}},
        {QStringLiteral("capabilities"), QJsonValue::Null},
        {QStringLiteral("endpointCapabilities"), kEndpointCapabilities},
        {QStringLiteral("additionalActions"), QJsonArray{join}}};
    QNetworkRequest req{QUrl(attach)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
    req.setRawHeader("X-MS-Migration", "True");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        qCInfo(lcNativeCall).noquote() << "attach HTTP" << http << shape(response).left(900);
        if (reply->error() != QNetworkReply::NoError) { finish(tr("Incoming call lost (attach HTTP %1)").arg(http)); return; }
        const AttachResponse attached = parseAttachResponse(response);
        m_invitationLinks = attached.invitationLinks;
        qCInfo(lcNativeCall) << "invitation links:" << m_invitationLinks.keys();
        rememberMediaLinks(response);
        if (!attached.participantId.isEmpty() && attached.participantId != m_participantId) {
            qCInfo(lcNativeCall) << "participantId from attach response replaces ours";
            m_participantId = attached.participantId;
        }
        if (m_remoteOffer.isEmpty()) {
            m_remoteOffer = attached.offer;
            m_remoteLegId = attached.legId;
        }
        // Conversation links (leave...) come back with the "join" action's response.
        if (attached.hasConversationLinks) m_links = attached.conversationLinks;
        if (m_remoteOffer.isEmpty()) { finish(tr("Incoming call without a media offer")); return; }
        // Ringing, as Teams web reports it.
        postJson(QStringLiteral("progress"), "POST", m_invitationLinks.value(QLatin1String("progress")).toString(),
                 {{QStringLiteral("callProgress"), QJsonObject{{QStringLiteral("sender"), selfParticipant()},
                                                               {QStringLiteral("status"), QStringLiteral("ringing")},
                                                               {QStringLiteral("phrase"), QStringLiteral("ringing")}}}});
    });
}

void NativeCall::accept()
{
    if (!m_incoming) return;
    m_incoming = false;
    emit incomingChanged();
    m_tone.stop();
    setState(tr("Connecting…"));
    fetchTurn([this](const QStringList &turn) {
        if (turn.isEmpty()) return;
        // Answer with the Opus payload type the caller offered (webrtcbin matches codecs by it).
        static const QRegularExpression opus(QStringLiteral("a=rtpmap:(\\d+) opus/48000"), QRegularExpression::CaseInsensitiveOption);
        const auto m = opus.match(m_remoteOffer);
        const int pt = m.hasMatch() ? m.captured(1).toInt() : 111;
        // Video: the first H.264 payload type with packetization-mode 1, and its profile
        // (Teams clients offer several; the answer must name one of theirs, string for string).
        int videoPt = 107, sharePt = 107;
        QString videoProfile = QStringLiteral("42e01f");
        const QStringList sections = m_remoteOffer.split(QLatin1String("\r\nm="));
        int videoIndex = 0;
        for (const QString &section : sections) {
            if (!section.startsWith(QLatin1String("video"))) continue;
            ++videoIndex;
            static const QRegularExpression h264(QStringLiteral("a=rtpmap:(\\d+) H264/90000"), QRegularExpression::CaseInsensitiveOption);
            for (auto it = h264.globalMatch(section); it.hasNext();) {
                const QString candidatePt = it.next().captured(1);
                const QRegularExpression fmtp(QStringLiteral("a=fmtp:%1 ([^\\r\\n]*)").arg(candidatePt));
                const QString params = fmtp.match(section).captured(1);
                if (!params.contains(QLatin1String("packetization-mode=1"))) continue;
                static const QRegularExpression profile(QStringLiteral("profile-level-id=([0-9a-fA-F]+)"));
                if (videoIndex == 1) {
                    videoPt = candidatePt.toInt();
                    if (const auto p = profile.match(params); p.hasMatch()) videoProfile = p.captured(1);
                } else if (videoIndex == 2) {
                    sharePt = candidatePt.toInt();
                }
                break;
            }
        }
        if (!m_engine.start(turn, true, pt, videoPt, videoProfile, sharePt)) { finish(tr("Audio not started")); return; }
        m_renegotiationAnswerLink.clear();
        m_engine.setRemoteOffer(m_remoteOffer);
    });
}

void NativeCall::reject()
{
    if (!m_incoming) return;
    // Teams web: DELETE on the call leg with a 603.
    const QString leg = m_invitationLinks.value(QLatin1String("callLeg")).toString();
    const QJsonObject body{{QStringLiteral("callEnd"), QJsonObject{{QStringLiteral("code"), 603}, {QStringLiteral("subCode"), 0},
                                                                    {QStringLiteral("phrase"), QStringLiteral("CallEndReasonLocalUserInitiated")},
                                                                    {QStringLiteral("resultCategories"), QJsonArray{QStringLiteral("Success")}}}}};
    if (!leg.isEmpty()) postJson(QStringLiteral("reject"), "DELETE", leg, body);
    finish(tr("Call declined"));
}

void NativeCall::onOffer(const QString &sdp)
{
    if (!m_active) return;
    setState(tr("Calling…"));
    qCInfo(lcNativeCall).noquote() << "offer SDP:\n" << sdp;
    const QJsonObject self{{QStringLiteral("id"), m_id.selfMri},
                           {QStringLiteral("displayName"), m_id.displayName},
                           {QStringLiteral("endpointId"), m_id.endpointId},
                           {QStringLiteral("participantId"), m_participantId},
                           {QStringLiteral("languageId"), QStringLiteral("it-it")}};
    QJsonObject conversationLinks;
    for (const char *n : {"conversationEnd", "conversationUpdate", "localParticipantUpdate", "addParticipantSuccess", "addParticipantFailure",
                          "addModalitySuccess", "addModalityFailure", "confirmUnmute", "receiveMessage"})
        conversationLinks.insert(QString::fromLatin1(n), link(QStringLiteral("conversation"), QString::fromLatin1(n)));
    QJsonObject callLinks;
    for (const char *n : {"progress", "mediaAnswer", "acceptance", "redirection", "end"})
        callLinks.insert(QString::fromLatin1(n), link(QStringLiteral("call"), QString::fromLatin1(n)));

    // Body as Teams web sends it (captured test call); a call to a person names the callee in
    // "to" and has no scenario (Teams sets it only for the bot). groupChat stays null for a call to
    // one person, so the callee receives an ordinary one-to-one call; see groupChatFor().
    const bool test = m_calleeMri.isEmpty();
    QJsonArray to;
    for (const QString &mri : std::as_const(m_callees)) to.append(QJsonObject{{QStringLiteral("id"), mri}});
    QJsonObject body{
        {QStringLiteral("conversationRequest"), QJsonObject{
            {QStringLiteral("conversationType"), QJsonValue::Null},
            {QStringLiteral("subject"), QJsonValue::Null},
            {QStringLiteral("suppressDialout"), test},
            {QStringLiteral("roster"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Delta")},
                                                   {QStringLiteral("rosterUpdate"), link(QStringLiteral("conversation"), QStringLiteral("rosterUpdate"))}}},
            {QStringLiteral("properties"), QJsonObject{{QStringLiteral("allowConversationWithoutHost"), true},
                                                       {QStringLiteral("enableGroupCallEventMessages"), true},
                                                       {QStringLiteral("enableGroupCallUpgradeMessage"), false},
                                                       {QStringLiteral("enableGroupCallMeetupGeneration"), false}}},
            {QStringLiteral("links"), conversationLinks}}},
        {QStringLiteral("contentSharing"), QJsonValue::Null},
        {QStringLiteral("participants"), QJsonObject{{QStringLiteral("from"), self}, {QStringLiteral("to"), to}}},
        {QStringLiteral("capabilities"), QJsonValue::Null},
        {QStringLiteral("endpointCapabilities"), 73463},
        {QStringLiteral("clientEndpointCapabilities"), 248477418},
        {QStringLiteral("endpointMetadata"), QJsonObject{{QStringLiteral("holographicCapabilities"), 3}}},
        {QStringLiteral("groupContext"), QJsonValue::Null},
        {QStringLiteral("groupChat"), groupChatFor(test, m_meeting, m_callees, m_threadId)},
        {QStringLiteral("meetingInfo"), m_meeting ? QJsonValue(QJsonObject{{QStringLiteral("tenantId"), m_tenantId}, {QStringLiteral("organizerId"), m_organizerId}})
                                                  : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("meetingData"), QJsonValue::Null},
        {QStringLiteral("endpointState"), QJsonObject{{QStringLiteral("endpointStateSequenceNumber"), 1},
            {QStringLiteral("endpointProperties"), QJsonObject{{QStringLiteral("additionalEndpointProperties"),
                QJsonObject{{QStringLiteral("infoShownInReportMode"), QStringLiteral("FullInformation")}}}}}}},
        {QStringLiteral("callInvitation"), QJsonObject{
            {QStringLiteral("callModalities"), QJsonArray{QStringLiteral("Audio")}},
            {QStringLiteral("replaces"), QJsonValue::Null},
            {QStringLiteral("transferor"), QJsonValue::Null},
            {QStringLiteral("clientTransferContext"), QJsonValue::Null},
            {QStringLiteral("customContext"), QJsonValue::Null},
            {QStringLiteral("links"), callLinks},
            {QStringLiteral("clientContentForMediaController"), QJsonObject{
                {QStringLiteral("controlVideoStreaming"), link(QStringLiteral("call"), QStringLiteral("controlVideoStreaming"))},
                {QStringLiteral("csrcInfo"), link(QStringLiteral("call"), QStringLiteral("csrcInfo"))}}},
            {QStringLiteral("pstnContent"), QJsonObject{{QStringLiteral("emergencyCallCountry"), QString()},
                                                        {QStringLiteral("platformName"), QStringLiteral("Kollega/0.1; os=linux; deviceType=computer")},
                                                        {QStringLiteral("publicApiCall"), false}}},
            {QStringLiteral("emergencyContent"), QJsonValue::Null},
            {QStringLiteral("mediaContent"), QJsonObject{
                {QStringLiteral("blob"), sdp},
                {QStringLiteral("contentType"), QStringLiteral("application/sdp-ngc-1.0")},
                {QStringLiteral("clientLocation"), QStringLiteral("IT")},
                {QStringLiteral("mediaLegId"), QUuid::createUuid().toString(QUuid::Id128).toUpper()}}},
            {QStringLiteral("voicemailSettings"), QJsonObject{}},
            {QStringLiteral("locationContent"), QJsonValue::Null},
            {QStringLiteral("networkContent"), QJsonValue::Null},
            {QStringLiteral("areaContent"), QJsonValue::Null}}}};

    if (test) {
        QJsonObject request = body.value(QLatin1String("conversationRequest")).toObject();
        request.insert(QStringLiteral("scenario"), QStringLiteral("UserInitiatedTestCall"));
        body.insert(QStringLiteral("conversationRequest"), request);
    }

    QNetworkRequest req{QUrl(QString::fromLatin1(kConversationService))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Teams web's calling code: token type "skype" -> X-Skypetoken (+ X-MS-Migration).
    req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
    req.setRawHeader("X-MS-Migration", "True");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, test] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        qCInfo(lcNativeCall) << "epconv HTTP" << http << "response" << shape(QJsonDocument::fromJson(data).object()).left(600);
        if (reply->error() != QNetworkReply::NoError) {
            finish(tr("Call refused by the server (HTTP %1)").arg(http));
            qCWarning(lcNativeCall) << "epconv error body:" << data.left(400);
            return;
        }
        m_links = QJsonDocument::fromJson(data).object().value(QLatin1String("links")).toObject();
        setState(test ? tr("Waiting for the answer…")
                      : m_meeting ? tr("Joining %1…").arg(m_calleeName) : tr("Calling %1…").arg(m_calleeName));
        if (test) addTestBot();
    });
}

bool NativeCall::handleCallback(const QString &url, const QJsonObject &body)
{
    if (m_agentId.isEmpty() || !url.contains(QStringLiteral("/callAgent/") + m_agentId + QLatin1Char('/'))) return false;
    // ".../call/mediaAnswer/" -> "call/mediaAnswer"
    const QString event = url.section(QLatin1Char('/'), -3, -2);
    qCInfo(lcNativeCall).noquote() << "callback" << event << shape(body).left(500);
    // End reasons, updates, rosters and video control are codes, phrases and stream ids, no
    // secrets: logged whole while the protocol is being learned.
    if (event.endsWith(QLatin1String("/end")) || event.endsWith(QLatin1String("/conversationEnd")) || event.endsWith(QLatin1String("/conversationUpdate"))
        || event.endsWith(QLatin1String("/rosterUpdate")) || event.endsWith(QLatin1String("/controlVideoStreaming")) || event.endsWith(QLatin1String("/mediaAcknowledgement")))
        qCInfo(lcNativeCall).noquote() << "  body:" << QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact)).left(4000);
    rememberMediaLinks(body);
    if (event == QLatin1String("call/mediaAnswer") || event == QLatin1String("call/acceptance")) {
        const QString sdp = findSdp(body);
        if (!sdp.isEmpty()) m_engine.setRemoteAnswer(sdp);
        if (event == QLatin1String("call/acceptance")) {
            m_tone.stop();
            setState(tr("Connecting audio…"));
            // Teams web acknowledges the acceptance through the link it carries; keep the
            // call-leg links for later (hold, renegotiation, keep-alive).
            const QJsonObject acceptance = body.value(QLatin1String("callAcceptance")).toObject();
            m_callLinks = acceptance.value(QLatin1String("links")).toObject();
            qCInfo(lcNativeCall) << "call links:" << m_callLinks.keys();
            const QString ack = m_callLinks.value(QLatin1String("acknowledgement")).toString();
            if (!ack.isEmpty()) {
                QNetworkRequest req{QUrl(ack)};
                req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
                req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
                req.setRawHeader("X-MS-Migration", "True");
                // Body as Teams web builds it (XA() in its calling code): where to send
                // the later call events, all on our Trouter.
                const QJsonObject ackBody{{QStringLiteral("callAcceptanceAcknowledgement"), QJsonObject{{QStringLiteral("links"), acceptanceLinks()}}}};
                QNetworkReply *reply = m_nam.post(req, QJsonDocument(ackBody).toJson(QJsonDocument::Compact));
                connect(reply, &QNetworkReply::finished, this, [this, reply] {
                    const QByteArray data = reply->readAll();
                    qCInfo(lcNativeCall) << "acknowledgement HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << data.left(200);
                    rememberMediaLinks(QJsonDocument::fromJson(data).object());
                    if (m_mediaLinks.contains(QLatin1String("applyChannelParameters"))) sendMaxVideoSendCapabilities();
                    reply->deleteLater();
                });
            }
        }
    } else if (event == QLatin1String("call/controlVideoStreaming")) {
        // The media controller's instructions about our video stream (start/stop, sizes):
        // we send whenever the camera is on; logged above to learn the shape.
    } else if (event == QLatin1String("call/mediaRenegotiation")) {
        // The media controller re-offers (e.g. when the callee joins): answer through its link.
        const QJsonObject negotiation = body.value(QLatin1String("mediaNegotiation")).toObject();
        m_renegotiationAnswerLink = negotiation.value(QLatin1String("links")).toObject().value(QLatin1String("mediaAnswer")).toString();
        m_renegotiationLegId = negotiation.value(QLatin1String("mediaContent")).toObject().value(QLatin1String("mediaLegId")).toString();
        const QString sdp = findSdp(negotiation);
        if (!sdp.isEmpty()) m_engine.setRemoteOffer(sdp);
        else qCWarning(lcNativeCall) << "renegotiation without SDP";
    } else if (event == QLatin1String("call/rejection")) {
        qCWarning(lcNativeCall).noquote() << "our renegotiation was rejected:" << QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact)).left(800);
    } else if (event == QLatin1String("call/mediaAcknowledgement")) {
        qCInfo(lcNativeCall) << "renegotiation acknowledged";
    } else if (event == QLatin1String("conversation/rosterUpdate")) {
        // Who is in the call and in what state (ringing, connected...), for the log.
        const QJsonObject participants = body.value(QLatin1String("participants")).toObject();
        for (auto it = participants.begin(); it != participants.end(); ++it)
            qCInfo(lcNativeCall) << "  roster" << it.key().section(QLatin1Char(':'), -1).left(8) << it.value().toObject().value(QLatin1String("state")).toString();
        parseRosterStreams(body);
    } else if (event == QLatin1String("call/progress")) {
        setState(tr("Ringing…"));
        m_tone.play(QString::fromLatin1(kRingback));
    } else if (event == QLatin1String("call/end") || event == QLatin1String("conversation/conversationEnd")) {
        finish(callEndText(body));
    }
    return true;
}

void NativeCall::hangUp()
{
    if (!m_active) return;
    if (m_incoming) { reject(); return; }
    const QString leave = m_links.value(QLatin1String("leave")).toString();
    if (!leave.isEmpty()) {
        const QJsonObject body{
            {QStringLiteral("participants"), QJsonObject{{QStringLiteral("from"), QJsonObject{
                {QStringLiteral("id"), m_id.selfMri}, {QStringLiteral("displayName"), m_id.displayName},
                {QStringLiteral("endpointId"), m_id.endpointId}, {QStringLiteral("participantId"), m_participantId},
                {QStringLiteral("languageId"), QStringLiteral("it-it")}}}}},
            {QStringLiteral("conversationTransactionEnd"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("Success")},
                                                                       {QStringLiteral("code"), 0}, {QStringLiteral("phrase"), QStringLiteral("User ended the call")}}}};
        QNetworkRequest req{QUrl(leave)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("X-Skypetoken", m_id.skypeToken.toUtf8());
        req.setRawHeader("X-MS-Migration", "True");
        QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, reply, [reply] {
            qCInfo(lcNativeCall) << "leave HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
        });
    }
    finish(tr("Call ended"));
}
