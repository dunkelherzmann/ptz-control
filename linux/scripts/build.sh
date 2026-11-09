#!/bin/sh
# Builds the Linux app as a Flatpak (linux/org.ptzcontrol.app.yml) for this machine's CPU, on Linux, and
# writes a single-file bundle to dist/. One-time setup on Ubuntu or Debian:
#   sudo apt install flatpak
#   flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
#   flatpak install --user -y flathub org.flatpak.Builder
# The KDE runtime and SDK it needs are installed from Flathub on the first build.
# Usage: linux/scripts/build.sh [--install]   (--install also installs the result for this user)
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ID=org.ptzcontrol.app
[ "$(uname -s)" = Linux ] || { echo "linux/scripts/build.sh runs on Linux; build the macOS app with macos/scripts/build.sh" >&2; exit 1; }
flatpak info org.flatpak.Builder >/dev/null 2>&1 \
    || { echo "org.flatpak.Builder not found; see the setup lines at the top of this script" >&2; exit 1; }
VERSION="$(sh "$ROOT/scripts/version.sh" --header)"
ARCH="$(uname -m)"
printf 'PTZ Control %s (%s)\n' "$VERSION" "$ARCH"

WORK="$ROOT/.build/flatpak"
mkdir -p "$WORK" "$ROOT/dist"
flatpak run org.flatpak.Builder --user --install-deps-from=flathub --force-clean \
    --state-dir="$WORK/state" --repo="$WORK/repo" "$WORK/build" "$ROOT/linux/$ID.yml"

BUNDLE="$ROOT/dist/PTZ-Control-$VERSION-$ARCH.flatpak"
# The bundle names Flathub, so installing it fetches the KDE runtime when the system lacks it.
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo "$WORK/repo" "$BUNDLE" "$ID"
printf 'Built dist/%s\n' "$(basename "$BUNDLE")"

if [ "${1:-}" = "--install" ]; then
    flatpak install --user -y --noninteractive --reinstall "$BUNDLE"
    printf 'Installed %s %s for this user\n' "$ID" "$VERSION"
fi
