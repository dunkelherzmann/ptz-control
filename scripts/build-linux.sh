#!/bin/sh
# Builds the Linux AppImage for this machine's CPU (x86_64 or aarch64) on Linux. The build runs in a
# rootless Podman (or Docker) container from Ubuntu 22.04 (linux/Containerfile), so the AppImage runs on
# glibc 2.35 and newer whatever distribution builds it. One-time setup on Ubuntu: sudo apt install podman
# Usage: scripts/build-linux.sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIMES="$ROOT/.build/appimage"
mkdir -p "$RUNTIMES" "$ROOT/dist"
[ "$(uname -s)" = Linux ] || { echo "build-linux.sh runs on Linux; build the macOS app with scripts/build-macos.sh" >&2; exit 1; }
VERSION="$(sh "$ROOT/scripts/version.sh" --header)"
printf 'PTZ Control %s\n' "$VERSION"
if command -v podman >/dev/null; then ENGINE=podman
elif command -v docker >/dev/null; then ENGINE=docker
else echo "podman not found; sudo apt install podman" >&2; exit 1
fi

# Official AppImage launchers (github.com/AppImage/type2-runtime), pinned by checksum.
# These static launchers run without libfuse2 on the user's system.
arch="$(uname -m)"
case "$arch" in
    x86_64) runtime_sha=617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46 ;;
    aarch64) runtime_sha=9a72a744d221576c9347ec074243acc5c3c7568896b12714e0e82e9f0bf47900 ;;
    *) echo "unsupported architecture $arch" >&2; exit 1 ;;
esac
runtime="$RUNTIMES/runtime-$arch"
if [ ! -f "$runtime" ]; then
    curl -fsSL --retry 3 --retry-all-errors -o "$runtime" "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$arch" \
        || { rm -f "$runtime"; exit 1; }
fi
if [ "$(sha256sum "$runtime" | cut -d' ' -f1)" != "$runtime_sha" ]; then
    echo "runtime-$arch does not match its pinned checksum; delete it to re-download, then update the checksum" >&2
    exit 1
fi

image="localhost/ptz-linux-build:$arch"
if ! "$ENGINE" image inspect "$image" >/dev/null 2>&1; then
    "$ENGINE" build -t "$image" -f "$ROOT/linux/Containerfile" "$ROOT/linux"
fi
printf 'Building %s AppImage\n' "$arch"
"$ENGINE" run --rm \
    -v "$ROOT:/src:ro" -v "$RUNTIMES:/runtimes:ro" -v "$ROOT/dist:/out" \
    "$image" sh /src/linux/package.sh /src "/runtimes/runtime-$arch" /out
