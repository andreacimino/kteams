# Installing KTeams

## Arch / CachyOS / Manjaro

    packaging/arch/build.sh
    sudo pacman -U packaging/arch/kteams-*.pkg.tar.zst

The package pulls in every dependency (Qt 6, Kirigami, GStreamer with webrtcbin, PipeWire).
"KTeams" then shows up in the menu; the first start asks for the Microsoft sign-in with a
code to enter in the browser.

## Other distributions

Build from source for now (see `native/CMakeLists.txt` for the libraries: Qt 6.5+, KF6
Kirigami/Notifications/StatusNotifierItem, QtKeychain, GStreamer 1.22+ with webrtcbin,
libnice, gst-plugin-pipewire). Flatpak and AppImage: planned.
