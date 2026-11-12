# KTeams: native Microsoft Teams client for KDE (work accounts)

Goal: a **native Qt/Kirigami client** for a work/school Teams account, with the normal
Microsoft sign-in and every function the official client has, calls included, without any
embedded browser (the only WebEngine code left is the optional protocol recorder). Memory and speed are the point: the app runs at ~120 MB where Teams web
uses ~1 GB.

Chat in Italian with the developer; code, comments, commit messages, documentation and the
interface's source strings in English. User-visible text goes through `qsTr()` (QML) or
`tr()` (C++); Italian lives in `native/translations/kteams_it.ts` (after changing
strings: `cmake --build native/build --target update_translations`, then fill the new
entries; `lrelease` runs at build time).

## Layout

- `native/`: the client (CMake, Qt 6.12, Kirigami 6, KF6 Notifications / StatusNotifierItem,
  QtKeychain, GStreamer webrtcbin). Build: `cmake -S native -B native/build && cmake --build
  native/build`; install for the menu/badge: `cmake --install native/build --prefix ~/.local`.
  - `src/oauth.*`: device-code sign-in (purple-teams' client id), token refresh per resource,
    refresh token in the keychain.
  - `src/teamsclient.*`: chat service (`<region>.ng.msg.teams.microsoft.com`, skypetoken),
    CSA chats/teams summary, profiles, presence, calendar, search, files.
  - `src/trouter.*`: push channel (socket.io v1 websocket): messages, presence, call events
    (`NGCallManagerWin`), and the call callbacks (`callAgent/...`).
  - `src/nativecall.*` + `src/callengine.*`: native calls (audio, camera, screen sharing),
    see `native/docs/calling-protocol.md`; `src/screencast.*`: the ScreenCast portal.
  - `tests/offer_dump.cpp` (`kteams-offer-test`): prints the SDP offer and runs the
    camera without calling anyone; the way to check the media pipeline.
  - `src/backend.*`: state, models, notifications, everything QML sees as `backend`.
  - `qml/`: Kirigami UI (chat list with tabs Chat/Team/Calendario, chat, channel threads).
  - `call/`: `kteams-capture`, Teams web in a QtWebEngine window, a developer tool only
    (`-DBUILD_CALL_HELPER=ON`): `--capture` records the calling protocol. The app never
    starts it.

## How the protocol was learned

purple-teams (EionRobb) for chat/presence/login, Teams web captures (`kteams-capture
--capture`) and its minified code (service-worker cache) for calling. Never invent an
endpoint or a field: check a capture, the Teams code, or purple-teams, and log the shape of
unknown responses before using them.

## Working rules

- Never restart the running app while the developer may be in a call: ask first.
- No autonomous test calls (`KTEAMS_AUTOTEST`) without telling the developer.
- Secrets (tokens, TURN credentials) never go into logs or captures: mask them.
- Logs: `QT_FORCE_STDERR_LOGGING=1`, categories `kteams.*`; the call engine logs a `stats`
  line every 5 s (raw/encoded/sent/received packets, peer receiver reports).

## State (November 2026)

Working: sign-in, chats and channels with threads, formatting, images and files (upload and
download), reactions, mentions, presence and own status, calendar, notifications with
sounds and pictures, tray, native audio calls (outgoing one-to-one and group, incoming, test
call) with echo cancellation, activity feed, calendar replies, local cache. Camera and
screen sharing in native calls: implemented, being verified in real calls (the video
signaling is partly guessed from Teams web's code: read the logs). Not yet: global search,
chat management (mute/pin), meetings.
