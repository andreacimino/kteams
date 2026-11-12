# KTeams

A **native** Microsoft Teams client for KDE Plasma (work / school accounts), written in
Qt 6 / Kirigami. No embedded browser: it talks to the Teams services directly, as the
official client does, and uses about 120 MB of memory against ~1 GB for Teams web.

Status: in daily use for chat and audio calls; video, screen sharing and meetings are
implemented and being verified on real calls. The interface is in English, with an Italian
translation picked automatically from the system language.

## What it does

- **Sign-in** with the normal Microsoft login (device code: the browser opens, you enter the
  code, you come back). The token stays in the system wallet (KWallet).
- **Chat**, one-to-one and groups, **teams and channels** with threads, formatting, pictures
  and attachments (sending, also by drag and drop, and downloading), reactions, mentions,
  edit and delete, quotes, "seen by", "is typing". Pinned, muted and hidden chats; groups can
  be renamed, have people added, and be left.
- **Native calls** (WebRTC through GStreamer): audio with echo cancellation, one-to-one and
  group, outgoing and incoming with a ringtone; **camera** and **screen sharing** (Plasma's
  system picker); **meetings** joined from the calendar.
- **Presence**: your own status and everybody else's, a status message, "Do not disturb"
  that silences notifications.
- **Calendar**: the week, reminders, accept/decline, join, new meeting.
- **Activity**: mentions, replies, reactions, with filters.
- **Plasma notifications** with sounds and pictures, a tray icon with the unread count,
  start at login, shortcuts (Ctrl+K search, Ctrl+N new chat, Ctrl+1..4 tabs).
- **Local cache**: chats, channels, people and recent messages are back at once on start-up.

- **Search**: chats by name, and messages (the recent ones of every chat; not Teams'
  server-side search yet).

Not yet: a grid with several videos at once, meeting lobby, Adaptive Cards, settings
(audio/video devices).

## Install

Arch / CachyOS / Manjaro: `packaging/arch/build.sh` makes a package, then
`sudo pacman -U packaging/arch/kteams-*.pkg.tar.zst`. Dependencies come from pacman.
Details in `packaging/README.md`.

From source (any distribution with Qt 6.5+, KF6, GStreamer 1.22+ with webrtcbin):

    cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
    cmake --build native/build
    cmake --install native/build --prefix ~/.local     # menu entry, icon, notification sounds

Then "KTeams" from the menu, or `kteams`.

## When something goes wrong

- Logs are in `~/.cache/kteams/kteams/kteams.log` (no tokens or secrets in
  them). Live: `QT_FORCE_STDERR_LOGGING=1 kteams`.
- During calls the log prints a `stats:` line every 5 seconds with the packets sent and
  received: "received" stuck at 0 means a network (TURN) or negotiation problem.
- "Sign out" in the tray menu really quits; closing the window keeps the app in the tray.
- Signing out of the account clears the cache and the token.

## For developers

- `native/`: the client. `src/` C++ (backend, Teams HTTP client, Trouter push channel, calls:
  `nativecall.*` signaling, `callengine.*` media), `qml/` the Kirigami interface,
  `translations/` the Qt Linguist files (source strings are English; after changing strings:
  `cmake --build native/build --target update_translations`, then fill the `.ts`).
- `native/docs/calling-protocol.md`: the Teams calling protocol as we understand it
  (Microsoft's SDP dialect, video source requests, meetings).
- `native/tests/offer_dump.cpp` → `kteams-offer-test`: prints the SDP offer, turns the
  camera on, and with `loop` runs a call between two engines in one process, without calling
  anyone. `cmake --build native/build --target kteams-offer-test`.
- `native/call/`: `kteams-capture`, Teams web in a QtWebEngine window, a developer tool only
  (`-DBUILD_CALL_HELPER=ON`): with `--capture` it records the protocol traffic, secrets
  masked. The app does not use it.
- The chat protocol comes from purple-teams; the calling one from captures and Teams web's
  code. Nothing is invented: every field was seen in a capture or in the code.

## License and disclaimer

KTeams is free software, released under the **GNU General Public License, version 3 or
later** (see `LICENSE`). It is written and maintained by Andrea Cimino with the help of
AI assistants.

KTeams is an **unofficial** client. It is not affiliated with, endorsed by or supported by
Microsoft. "Microsoft Teams" and "Microsoft" are trademarks of Microsoft Corporation. The
program talks to Teams through the same services the official clients use; those services
are not publicly documented and may change at any time, which can stop the program from
working without notice. Use it at your own risk and in accordance with the terms of service
of your organization's Microsoft 365 subscription: the authors accept no liability for any
damage, data loss or policy breach resulting from its use. The program is provided "as is",
without warranty of any kind, as stated in the license.

No usage data is collected: the program exchanges data only with Microsoft's services, on
your behalf, and keeps its cache and your session on your computer.
