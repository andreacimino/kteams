# Installing Kollega

## Arch / CachyOS / Manjaro

From the AUR (`kollega-git`, the latest commit of the `develop` branch), with an AUR helper:

    yay -S kollega-git        # or paru -S kollega-git

or by hand, with the files in `arch/kollega-git/` (what the AUR repository contains):

    cd packaging/arch/kollega-git
    makepkg -si

To package the checkout you are working on instead of GitHub's `develop`:

    packaging/arch/build.sh
    sudo pacman -U packaging/arch/kollega-*.pkg.tar.zst

Either package pulls in every dependency (Qt 6, Kirigami, GStreamer with webrtcbin, PipeWire).
"Kollega" then shows up in the menu; the first start asks for the Microsoft sign-in with a
code to enter in the browser.

### Updating the AUR package

The AUR repository is a git repository holding only `PKGBUILD` and `.SRCINFO`
(`ssh://aur@aur.archlinux.org/kollega-git.git`). After changing `arch/kollega-git/PKGBUILD`
(new dependency, new `pkgrel`), regenerate `.SRCINFO` and push both files there:

    cd packaging/arch/kollega-git
    makepkg --printsrcinfo > .SRCINFO
    cp PKGBUILD .SRCINFO <aur-clone>/ && cd <aur-clone> && git add -A && git commit -m "..." && git push

A `-git` package needs no update for new commits: `makepkg` always builds the newest one
and computes `pkgver` itself.

## Flatpak (any distribution)

GitHub Actions (`.github/workflows/flatpak.yml`) builds the Flatpak on every push, with the
manifest in `flatpak/io.github.andreacimino.kollega.yml`, and keeps `kollega.flatpak` as an artifact of the
run (Actions tab, open the run, "Artifacts"); on a `v*` tag it is attached to the release.
Install it with:

    flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak install --user kollega.flatpak      # pulls org.kde.Platform 6.11 from Flathub
    flatpak run io.github.andreacimino.kollega

To build it locally (needs `flatpak-builder` and the `org.kde.Sdk//6.11` runtime):

    flatpak-builder --user --install --force-clean build-dir packaging/flatpak/io.github.andreacimino.kollega.yml

Inside the sandbox the app can read the home folder (files dragged into a chat), write
`~/Downloads` (received files), use the microphone, speakers and camera, and talk to the
notification, tray and wallet services. Not yet in the sandbox: the autostart entry (it
needs the Background portal).

## Other distributions

Build from source for now (see `native/CMakeLists.txt` for the libraries: Qt 6.5+, KF6
Kirigami/Notifications/StatusNotifierItem, QtKeychain, GStreamer 1.22+ with webrtcbin,
libnice, gst-plugin-pipewire). AppImage: planned.
