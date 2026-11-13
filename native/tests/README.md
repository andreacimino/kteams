# Tests

Regression tests with Qt Test, run by ctest; `offer_dump.cpp` (`kteams-offer-test`) is a
separate developer tool that exercises the media pipeline without calling anyone.

| File | What it covers |
|---|---|
| `tst_content.cpp` | `src/content.cpp`: Teams HTML to text and rich text, links, pictures, emoji, files, times |
| `tst_chatparser.cpp` | `src/chatparser.cpp`: chat service JSON to conversations (unread, pinned, previews), messages (edited, deleted, mentions), reactions, titles |
| `tst_trouterframes.cpp` | `src/trouterframes.cpp`: socket.io frames, gzip/base64 push bodies, routing of pushes, message-loss repeats |
| `tst_calljson.cpp` | `src/calljson.cpp`: SDP lookup, media controller links, roster streams of a call |
| `tst_caches.cpp` | `src/mediacache.cpp`, `src/avatarcache.cpp`: which pictures get the session token, served from disk, stale and "no picture" entries (offline, in a temporary cache home) |
| `tst_backendlogic.cpp` | what used to sit inside Backend: team channels, calendar events, activity feed, reminder timing, download file names (no path escapes), reply quotes (escaping) |
| `tst_teamsclient.cpp` | `src/teamsclient.cpp` calls (chat service, sign-in, profiles, calendar, search, presence, pictures) against a fake HTTP server on loopback: method, path, query, headers and body of each request, the signals for answers, errors and bad JSON |
| `tst_oauth.cpp` | `src/oauth.cpp`: device code sign-in, token refresh, caching and sharing, session loss, with a fake login server and an in-memory token store (never the system keychain) |
| `tst_fileuploader.cpp` | `src/fileuploader.cpp`: sending a file through Microsoft Graph (OneDrive for chats, the channel folder for channels), 5 MiB chunks, the file entry Teams expects, every failure stage |
| `tst_models.cpp` | `src/models.cpp`: the list models (conversations, messages, channels, activity, calendar) |
| `tst_statecache.cpp` | `src/statecache.cpp`: the local cache's JSON format, old and new |
| `tst_demo.cpp` | `src/demo.h`: demo mode hides real names, titles and texts |

Run them:

    cmake -S native -B native/build -DBUILD_TESTING=ON
    cmake --build native/build
    ctest --test-dir native/build --output-on-failure

Coverage (needs `gcovr`: `pacman -S gcovr`, or `pip install gcovr` in a virtualenv and
`-DGCOVR_EXECUTABLE=<path>`), in a separate build directory because it changes the compiler
flags:

    cmake -S native -B native/build-coverage -DBUILD_TESTING=ON -DKTEAMS_COVERAGE=ON
    cmake --build native/build-coverage --target coverage
    xdg-open native/build-coverage/coverage/index.html

The summary is printed at the end and saved in `coverage/summary.txt`. The same runs in
GitHub Actions (`.github/workflows/tests.yml`), where the HTML report is an artifact.

Writing a test: one `tst_<module>.cpp` per source module, registered with `kteams_test()` in
`CMakeLists.txt`; each slot checks one behaviour and is named after it. Code that talks to
the network is tested against `FakeServer` (`fakeserver.h`), a server on loopback that records
every request and answers as configured: `teamsclient` through `setTokens` and `setHosts`,
`oauth` through `setAuthority` and `setTokenStore`, `fileuploader` through `setGraphBase`. A new service client gets the same two
injection points. `callengine` and the HTTP parts of `nativecall` are not unit-tested yet:
parsing that can be separated from the request goes into a pure function (as `content.cpp`
does) and gets tested there.
