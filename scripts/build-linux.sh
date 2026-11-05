#!/bin/sh
# Builds the Linux AppImages (x86_64 and aarch64) on macOS with Apple's `container`
# tool (brew install container). Each architecture is built in an Ubuntu 22.04
# container (linux/Containerfile), so the AppImages run on glibc 2.35 and newer.
# Usage: scripts/build-linux.sh [x86_64|aarch64]...   (default: both)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIMES="$ROOT/.build/appimage"
ARCHES="${*:-x86_64 aarch64}"
mkdir -p "$RUNTIMES" "$ROOT/dist"
VERSION="$(sh "$ROOT/scripts/version.sh" --header)"
printf 'PTZ Control %s\n' "$VERSION"
command -v container >/dev/null || { echo "container not found; brew install container" >&2; exit 1; }
container system status >/dev/null 2>&1 || container system start --enable-kernel-install

# Official AppImage launchers (github.com/AppImage/type2-runtime), pinned by checksum.
# These static launchers run without libfuse2 on the user's system.
runtime_sha() {
    case "$1" in
        x86_64) echo 617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46 ;;
        aarch64) echo 9a72a744d221576c9347ec074243acc5c3c7568896b12714e0e82e9f0bf47900 ;;
    esac
}

for arch in $ARCHES; do
    case "$arch" in
        x86_64) platform=amd64 ;;
        aarch64) platform=arm64 ;;
        *) echo "unknown architecture $arch" >&2; exit 1 ;;
    esac
    runtime="$RUNTIMES/runtime-$arch"
    if [ ! -f "$runtime" ]; then
        curl -fsSL -o "$runtime" "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$arch"
    fi
    if [ "$(shasum -a 256 "$runtime" | cut -d' ' -f1)" != "$(runtime_sha "$arch")" ]; then
        echo "runtime-$arch does not match its pinned checksum; delete it to re-download, then update the checksum" >&2
        exit 1
    fi
    image="ptz-linux-build:$platform"
    if ! container image inspect "$image" >/dev/null 2>&1; then
        container build --arch "$platform" -t "$image" -f "$ROOT/linux/Containerfile" "$ROOT/linux"
    fi
    printf 'Building %s AppImage\n' "$arch"
    container run --rm --arch "$platform" --cpus 8 --memory 8G \
        -v "$ROOT:/src" -v "$RUNTIMES:/runtimes" -v "$ROOT/dist:/out" \
        "$image" sh /src/linux/package.sh /src "/runtimes/runtime-$arch" /out
done
