# Tests

Regression tests with Qt Test, run by ctest; `offer_dump.cpp` (`kteams-offer-test`) is a
separate developer tool that exercises the media pipeline without calling anyone.

| File | What it covers |
|---|---|
| `tst_content.cpp` | `src/content.cpp`: Teams HTML to text and rich text, links, pictures, emoji, files, times |
| `tst_chatparser.cpp` | `src/chatparser.cpp`: chat service JSON to conversations (unread, pinned, previews), messages (edited, deleted, mentions), reactions, titles |
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
the network (`teamsclient`, `trouter`, `oauth`) is not unit-tested yet: parsing that can be
separated from the request goes into a pure function (as `content.cpp` does) and gets tested there.
