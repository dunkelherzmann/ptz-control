#!/bin/sh
# Builds PTZ Control for Linux and installs it under a prefix (/app inside the Flatpak build).
# Usage: sh linux/install.sh PREFIX   (run by linux/org.ptzcontrol.app.yml)
# The version comes from core/include/ptz_version.h, written by scripts/version.sh --header before the build
# (flatpak-builder copies the sources without .git).
set -eu
PREFIX="${1:-/app}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID=org.ptzcontrol.app
test -f "$ROOT/core/include/ptz_version.h" || { echo "core/include/ptz_version.h is missing; run scripts/version.sh --header" >&2; exit 1; }
VERSION="$(sed -n 's/^#define PTZ_VERSION "\(.*\)"$/\1/p' "$ROOT/core/include/ptz_version.h")"

cmake -S "$ROOT/linux" -B /tmp/ptz-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release >/dev/null
make -C /tmp/ptz-build -j"$(nproc)"
install -Dm755 -s /tmp/ptz-build/ptz-control "$PREFIX/bin/ptz-control"

install -Dm644 "$ROOT/linux/res/$ID.desktop" "$PREFIX/share/applications/$ID.desktop"
mkdir -p "$PREFIX/share/metainfo"
sed "s|<release version=\"[^\"]*\" date=\"[^\"]*\"/>|<release version=\"$VERSION\" date=\"$(date -u +%Y-%m-%d)\"/>|" \
    "$ROOT/linux/res/$ID.metainfo.xml" > "$PREFIX/share/metainfo/$ID.metainfo.xml"
for size in 16 32 64 128 256 512; do
    install -Dm644 "$ROOT/linux/res/icons/$size.png" "$PREFIX/share/icons/hicolor/${size}x${size}/apps/$ID.png"
done
install -Dm644 "$ROOT/LICENSE" "$PREFIX/share/licenses/$ID/LICENSE"
install -Dm644 "$ROOT/NOTICES" "$PREFIX/share/licenses/$ID/NOTICES"
