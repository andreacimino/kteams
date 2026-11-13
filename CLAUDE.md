# KTeams: native Microsoft Teams client for KDE (work accounts)

Goal: a **native Qt/Kirigami client** for a work/school Teams account, with the normal
Microsoft sign-in and every function the official client has, calls included, without any
embedded browser (the only WebEngine code left is the optional protocol recorder). Memory and speed are the point: the app runs at ~120 MB where Teams web
uses ~1 GB.

Chat in Italian with the developer; code, comments, commit messages, documentation and the
interface's source strings in English. User-visible text goes through `qsTr()` (QML) or
`tr()` (C++). Translations live in `native/translations/kteams_<lang>.ts` (Qt Linguist):
Italian, German, French, Spanish, Brazilian Portuguese, Simplified Chinese, Japanese,
Russian, Korean, Dutch, Polish. After changing strings: `cmake --build native/build --target
update_translations` (lupdate; the QML files are passed explicitly, it does not read the
.qrc), then fill the new `type="unfinished"` entries in every language (an unfinished entry
shows English), keeping placeholders (`%1`) and using Microsoft Teams' official terms for
Teams concepts; `lrelease` runs at build time. A new language: add its `.ts` to `TS_FILES`
in `native/CMakeLists.txt`, run `update_translations` to create it, translate.

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
  - `src/backend.*`: state, notifications, everything QML sees as `backend`.
  - `src/content.*`: Teams message HTML to text/rich text, files (pure functions);
    `src/chatparser.*`: chat service JSON to conversations, messages, reactions, team channels,
    calendar events and the activity feed (a `SelfContext` carries who we are and the known people); `src/models.*`: the list models;
    `src/statecache.*`: the local cache's JSON format; `src/trouterframes.*`: Trouter's frames,
    gzip bodies and routing; `src/calljson.*`: SDP, media links and roster streams of a call.
    Everything but `main.cpp` is the static library `kteams_core`, linked by the app and the tests.
  - `qml/`: Kirigami UI (chat list with tabs Chat/Team/Calendario, chat, channel threads).
  - `call/`: `kteams-capture`, Teams web in a QtWebEngine window, a developer tool only
    (`-DBUILD_CALL_HELPER=ON`): `--capture` records the calling protocol. The app never
    starts it.

## Tests: the regression suite

Qt Test executables in `native/tests/` (`tst_content`, `tst_chatparser`, `tst_models`,
`tst_statecache`, `tst_trouterframes`, `tst_calljson`, `tst_caches`, `tst_teamsclient`, `tst_oauth`, `tst_backendlogic`, `tst_demo`; see `native/tests/README.md`), run by ctest; built by default:

    cmake -S native -B native/build && cmake --build native/build
    ctest --test-dir native/build --output-on-failure

Coverage (gcovr), in its own build directory because it changes the compiler flags:
`cmake -S native -B native/build-coverage -DBUILD_TESTING=ON -DKTEAMS_COVERAGE=ON` then
`cmake --build native/build-coverage --target coverage`; the report is
`native/build-coverage/coverage/index.html`, the summary `coverage/summary.txt`.

- Run the tests before saying a change is done; a red test is never weakened, skipped or
  deleted to make it pass.
- Every bug fix comes with a test that fails without the fix (the unclosed `<b>` after a
  mention was found this way).
- New logic that does not need the network goes into a pure function or module (like
  `content.cpp`) and gets its `tst_<module>.cpp`; one slot per behaviour, named after it.
  Two things moc misreads in a test file, and then silently drops the whole test class (the
  executable fails to link on its vtable): a `\"` inside a raw string literal, and a `//`
  inside one (it is taken for a comment and the rest of the line disappears, braces
  included). Build such JSON with `QJsonObject`, or keep the raw string alone on its line.
  Network clients are tested against `FakeServer` (`native/tests/fakeserver.h`, loopback; no
  real network in tests): `teamsclient` takes `setTokens` (chat service) and `setHosts` (the
  other Microsoft hosts), `oauth` takes `setAuthority` and `setTokenStore` (the tests never
  touch the system keychain). A new service client gets the same injection points.
  `callengine` and the HTTP parts of `nativecall` are only tested through the pure modules
  they use (`calljson`, `trouterframes`).
- CI: `.github/workflows/tests.yml` runs the suite with coverage in an Arch container on
  every push and pull request (the HTML report is an artifact); `flatpak.yml` builds the
  Flatpak. A red workflow means the branch is not ready.

## Packaging

- Arch and derivatives: `packaging/arch/kteams-git/` (PKGBUILD + .SRCINFO, the AUR package,
  builds `develop` from GitHub) and `packaging/arch/PKGBUILD` with `build.sh` (the current
  checkout). A new dependency goes into both PKGBUILDs, the Flatpak manifest and
  `.github/workflows/tests.yml`.
- Flatpak: `packaging/flatpak/org.kteams.kteams.yml` on the KDE 6.11 runtime; QtKeychain
  and the x264 GStreamer element are built as modules. Sandbox permissions are listed there
  with the reason for each. `native/data/org.kteams.kteams.metainfo.xml` is the AppStream
  file. See `packaging/README.md`.

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
