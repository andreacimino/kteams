# Teams calling protocol: what the native client does

Learned from `kollega-capture --capture` (Teams web test call, 2026-11-12), from Teams web's
calling code (service-worker cache) and by trial against the live service. Secrets were
masked in the captures; this file holds structure only. Implementation: `src/nativecall.cpp`
(signaling) and `src/callengine.cpp` (media, GStreamer webrtcbin).

## Media: standard WebRTC, with a dialect for the SDP text

- Unified Plan, BUNDLE, ICE, DTLS-SRTP, rtcp-mux. Audio: Opus (Microsoft answers with
  payload type **102**; offering Opus as 102 is what makes webrtcbin map the incoming
  stream). Video: one H.264 section, `a=label:main-video`, offered `sendrecv` from the
  start with the camera off, as Teams web does (its offer carries 18 video sections and
  an `x-data` one; the media controller answered Chrome with H.264 PT 107,
  `packetization-mode=1;profile-level-id=42C02A`). We offer PT 107 constrained baseline
  (`42e01f`, `level-asymmetry-allowed=1`); answering a Teams client we take its first
  H.264 PT with packetization-mode 1 and repeat its profile-level-id string (webrtcbin
  compares the strings). Screen sharing (`applicationsharing-video`): not done yet.
- TURN: `GET https://teams.microsoft.com/trap/tokens` (headers `X-Skypetoken`,
  `api-version: 2`) → `{tokens: [{username, password, realm}], expires}`; relays
  `turn(s):gateway-eu.az.relay.teams.cloud.microsoft:{3478,443}` (EU region).
- What we send (`toTeamsDialect`): Teams web rewrites libwebrtc's SDP before sending
  (`contentType: application/sdp-ngc-1.0`): real address and port in `c=`/`m=` (0.0.0.0 or
  port 9 means "disabled media" → `DisabledMediaNotSupported`), profile `RTP/SAVP`,
  `o=` with 127.0.0.1, `b=CT:4000`, `a=rtcp:<port>`, `a=label:main-audio`, no
  `a=rtcp-mux-only`, no empty lines (an empty ICE candidate ends gathering).
- What we receive (`fromTeamsDialect`): profile back to `UDP/TLS/RTP/SAVPF`; a direction
  attribute added when missing (webrtcbin does not apply the RFC 4566 default; its error
  message says "dtls setup" but it is the direction); candidates with `tcp-pass`/`tcp-act`
  dropped and the `MTURNID ...` extension removed (libnice); proprietary attributes
  (`a=x-source-streamid`, `a=x-mediabw`, `a=x-plaza-msi-range`, `a=x-signaling-fb`) are
  ignored by GStreamer.

## Signaling: REST + JSON on `api.flightproxy.teams.microsoft.com`, events on Trouter

All requests carry `X-Skypetoken` and `X-MS-Migration: True`. Every callback link we hand
out points at our Trouter address: `<surl>callAgent/<agent uuid>/<8 hex>/<call|conversation>/<event>/`;
Trouter delivers them as requests whose body may be gzip+base64 (`H4sI...`).

### Outgoing call

| Step | Request | Notes |
|---|---|---|
| create | `POST /api/v2/epconv` | `conversationRequest` (roster Delta + rosterUpdate link, properties, conversation links), `participants.from` (id, displayName, endpointId, participantId, languageId) and `.to: [{id}]`, `endpointCapabilities 73463`, `clientEndpointCapabilities 248477418`, `groupChat` = null for a call to one person (the test call's captured shape; with the chat's thread in it, the callee's Trouter push is `evt: 109` with `groupChat`, `groupContext` and `meetingProperties`, not `evt: 107`), the chat's `threadId` for a group call and for a meeting, `callInvitation` (callModalities, call links, clientContentForMediaController, pstnContent, `mediaContent.blob` = offer). `scenario: "UserInitiatedTestCall"` and `suppressDialout: true` only for the echo bot (`28:cf28171e-fcfd-47e4-a1d6-79460b0b3ca0`), which is then added with `POST links.addParticipantAndModality` (`/add`). Response: `links` (leave, addParticipantAndModality, updateEndpointMetadata, mute, subscribe...). |
| metadata | `PUT links.updateEndpointMetadata` | `{participants.from, endpointMetadata: {holographicCapabilities: 3}}` |
| accepted | callback `call/acceptance` | `callAcceptance.mediaContent.blob` = answer; `links.acknowledgement` → `POST {callAcceptanceAcknowledgement: {links: {mediaRenegotiation, transfer, replacement, balanceUpdate, retargetCompletion, controlVideoStreaming, updateMediaDescriptions}}}` |
| ringing | callback `call/progress` | callee's client is ringing |
| renegotiation | callback `call/mediaRenegotiation` | `mediaNegotiation.mediaContent.blob` = new offer (sent when the callee joins); answer with `POST mediaNegotiation.links.mediaAnswer` `{mediaAnswer: {callModalities, sender, links: {mediaAcknowledgement}, clientContentForMediaController, mediaContent}}`; then callback `call/mediaAcknowledgement`. Without this the callee hears nothing. |
| roster | callback `conversation/rosterUpdate` | `participants[mri].state` |
| end | `POST links.leave` / callback `call/end` | `callEnd.code` 603 declined, 486 busy, 480/408 no answer, 410 media/parsing errors with `phrase` |

### Incoming call

| Step | Request | Notes |
|---|---|---|
| notification | Trouter `NGCallManagerWin`, body `evt: 107` (a call) or `109` (a call in a group chat or meeting, which also carries `groupChat`), `cp` (gzip+base64) | `callNotification` (from, links.attach/redirection, mediaContent may hold the offer), `conversationInvitation` (conversationController, subject, isMultiParty) |
| attach | `POST callNotification.links.attach` | `{attach: {requireMediaContent, links: {end}, locationContent, networkContent, areaContent}, capabilities: null, endpointCapabilities, additionalActions: [{name: "join", url: conversationInvitation.conversationController, waitForResponse: true, input: {capabilities, endpointCapabilities, conversationRequest: {roster, links}, endpointMetadata, participants: {from}}}]}`. Response: `callInvitation` (links: acceptance, callLeg, progress, mediaAnswer, newOffer, redirection, subscribe; `mediaContent.blob` = offer), `additionalActionResponses` (the join: conversation links) |
| ringing | `POST callInvitation.links.progress` | `{callProgress: {sender, status: "ringing", phrase: "ringing"}}` |
| answer | `POST callInvitation.links.acceptance` | `{callAcceptance: {acceptedBy, acceptedCallModalities: ["Audio"], capabilities: null, endpointCapabilities, clientEndpointCapabilities, links (as the acknowledgement), clientContentForMediaController, mediaContent: {blob: answer, contentType, mediaLegId}, pstnContent: null, callKeepAliveInterval: null}}` |
| decline | `DELETE callInvitation.links.callLeg` | `{callEnd: {code: 603, subCode: 0, phrase, resultCategories: ["Success"]}}` |

## Video streams (from Teams web's calling code, `7213d919901961eb_0` in the sign-in cache)

Microsoft's media controller only forwards a video source to a client that asked for it,
and tells a client when to send its own. Labels: `main-audio`, `main-video`,
`applicationsharing-video`, `data`. Every section gets a server-side source id
(`a=x-source-streamid:` in the controller's answer: 201 audio, 202 video).

| What | How |
|---|---|
| media controller links | `applyChannelParameters` and `controlVideoStreaming` URLs on `api.flightproxy.../cc/v1/mcProxy/...`, delivered inside other messages (`saveMediaControllerLinksIfAny`: the acceptance's links); we pick them up wherever they appear. |
| what we can send | `POST applyChannelParameters` `{applyChannelParameters: {multiChannelParameter: {mids: ["<video mid>"], mediaParameter: "<JSON string>"}}}` with `{maxVideoSendCapabilities: {caps: {"max-width", "max-height", "max-fps", "max-streams", "max-layers", "sequence-number"}}}` (Teams web sends it right after the acceptance). |
| receive someone's video | the roster lists each endpoint's `mediaStreams: [{type, label, sourceId, direction}]`; a "source request" asks for one: same `applyChannelParameters` call with `{controlVideoStreaming: {sequenceNumber, controlInfo: {sourceId, streamMsid, fmtParams: "max-fs=…;max-mbps=…;max-fps=…", subStreamIndex?}}}`. Teams web prefers sending it in-band ("mcp", `x-signaling-fb:* x-message app send:src recv:src,vc`) and falls back to this signaling form. Still to confirm in a real call: the `mediaStreams` shape and what `streamMsid` must be (we send our section's msid). |
| send our video | callback `call/controlVideoStreaming` with `controlInfo: [{control: "start"/"stop", sourceId, fmtParams}]` tells us what to send; we send whenever the camera is on (the valve opens). |

## Screen sharing

Teams web's offer carries an `applicationsharing-video` section (mid 18 of 20) from the
start, `sendrecv`; a Teams desktop client offers it `inactive` and (it seems) renegotiates
when sharing starts. We offer it as our third section (`main-audio`, `main-video`,
`applicationsharing-video`), H.264 PT 108, the valve closed until the user shares; the
source is the desktop portal's PipeWire stream (`src/screencast.cpp`, ScreenCast portal
over D-Bus: CreateSession → SelectSources → Start → OpenPipeWireRemote). A remote share is
a video source whose roster label is `applicationsharing-video`: it is requested on our
screen section and shown on its own sink. Still to confirm in a real call, like the
camera: the roster shape and whether the controller needs a renegotiation for the
`inactive` section of a desktop client.

## Not done yet

Meetings (`meetingInfo`), hold/transfer, call keep-alive, device selection, in-band media
control plane, more than one remote video at a time (group calls show one).
