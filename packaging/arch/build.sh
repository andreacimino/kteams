#!/bin/sh
# Builds the Arch package from the current commit: packaging/arch/kteams-<ver>-1-x86_64.pkg.tar.zst
# Install it with: sudo pacman -U <file>
set -e
cd "$(dirname "$0")"
ver=$(grep '^pkgver=' PKGBUILD | cut -d= -f2)
git -C ../.. archive --format=tar.gz --prefix="kteams-$ver/" -o "$PWD/kteams-$ver.tar.gz" HEAD
makepkg -f --skipchecksums "$@"
