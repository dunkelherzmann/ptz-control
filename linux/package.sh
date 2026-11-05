#!/bin/sh
# Runs inside the build container (linux/Containerfile): builds PTZ Control, bundles
# every library a desktop Linux cannot be assumed to have, and writes an AppImage.
# Usage (from scripts/build-linux.sh): package.sh <src-root> <runtime-file> <out-dir>
set -eu

SRC="$1"
RUNTIME="$2"
OUT="$3"
VERSION="1.0.0"
ARCH="$(uname -m)"
TRIPLE="$(gcc -dumpmachine)"
QT_PLUGINS="/usr/lib/$TRIPLE/qt6/plugins"
BUILD=/tmp/build
APPDIR=/tmp/AppDir

cmake -S "$SRC/linux" -B "$BUILD" -G "Unix Makefiles" >/dev/null
make -C "$BUILD" -j"$(nproc)" 2>&1 | grep -E "warning|error" || true
test -x "$BUILD/ptz-control"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/ptz-control/fonts" "$APPDIR/usr/share/doc"
install -m 755 -s "$BUILD/ptz-control" "$APPDIR/usr/bin/ptz-control"
printf '[Paths]\nPrefix = ..\nPlugins = plugins\n' > "$APPDIR/usr/bin/qt.conf"

# Qt plugins: X11 and Wayland windows, keyboard composition and input methods.
for plugin in platforms/libqxcb.so platforms/libqwayland-generic.so \
              wayland-shell-integration wayland-decoration-client \
              platforminputcontexts/libcomposeplatforminputcontextplugin.so \
              platforminputcontexts/libibusplatforminputcontextplugin.so; do
    mkdir -p "$APPDIR/usr/plugins/$(dirname "$plugin")"
    cp -r "$QT_PLUGINS/$plugin" "$APPDIR/usr/plugins/$(dirname "$plugin")/"
done

# Libraries every desktop distribution provides (and some that must match the
# system's drivers): the AppImage project's exclude list, abridged.
is_system_library() {
    case "$1" in
        ld-linux*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|\
        libanl.so.*|libmvec.so.*|libnss_*|libthread_db.so.*|libBrokenLocale.so.*|\
        libstdc++.so.*|libgcc_s.so.*|\
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libOpenGL.so.*|libEGL.so.*|libgbm.so.*|libdrm.so.*|libglapi.so.*|\
        libxcb.so.1|libX11.so.*|libX11-xcb.so.*|libXau.so.*|libXdmcp.so.*|libICE.so.*|libSM.so.*|\
        libfontconfig.so.*|libfreetype.so.*|libharfbuzz.so.*|libexpat.so.*|libz.so.*|libuuid.so.*|\
        libasound.so.*|libcom_err.so.*|libusb-1.0.so.*|libwayland-egl.so.*)
            return 0 ;;
    esac
    return 1
}

dependencies() {
    ldd "$@" 2>/dev/null | awk '/=> \// { print $3 }' | sort -u
}

ELF_FILES="$APPDIR/usr/bin/ptz-control $(find "$APPDIR/usr/plugins" -name '*.so')"
# shellcheck disable=SC2086
for lib in $(dependencies $ELF_FILES); do
    name="$(basename "$lib")"
    is_system_library "$name" && continue
    cp -L "$lib" "$APPDIR/usr/lib/$name"
    # Licence and copyright notices of every bundled package.
    pkg="$(dpkg -S "$lib" 2>/dev/null | head -1 | cut -d: -f1)" || pkg=""
    if [ -z "$pkg" ]; then
        real="$(readlink -f "$lib")"
        pkg="$(dpkg -S "$real" 2>/dev/null | head -1 | cut -d: -f1)" || pkg=""
    fi
    if [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ]; then
        mkdir -p "$APPDIR/usr/share/doc/$pkg"
        cp "/usr/share/doc/$pkg/copyright" "$APPDIR/usr/share/doc/$pkg/"
    fi
done
for qtdoc in libqt6core6 qt6-qpa-plugins qt6-wayland fonts-jetbrains-mono libglvnd0 libopengl0 libharfbuzz0b libice6 libsm6; do
    if [ -f "/usr/share/doc/$qtdoc/copyright" ]; then
        mkdir -p "$APPDIR/usr/share/doc/$qtdoc"
        cp "/usr/share/doc/$qtdoc/copyright" "$APPDIR/usr/share/doc/$qtdoc/"
    fi
done

# Fallbacks for system libraries a minimal install might lack; AppRun adds one to
# the search path only when the system has no copy of its own.
for name in libOpenGL.so.0 libGLX.so.0 libEGL.so.1 libGLdispatch.so.0 libharfbuzz.so.0 libICE.so.6 libSM.so.6; do
    mkdir -p "$APPDIR/usr/lib/fallback/$name"
    cp -L "/usr/lib/$TRIPLE/$name" "$APPDIR/usr/lib/fallback/$name/$name"
    patchelf --set-rpath '$ORIGIN/../..' "$APPDIR/usr/lib/fallback/$name/$name"
done

# Every bundled ELF finds its neighbours relative to itself.
for lib in "$APPDIR"/usr/lib/*.so*; do
    chmod 644 "$lib"
    patchelf --set-rpath '$ORIGIN' "$lib"
done
find "$APPDIR/usr/plugins" -name '*.so' | while read -r plugin; do
    patchelf --set-rpath '$ORIGIN/../../lib' "$plugin"
done
strip --strip-unneeded "$APPDIR"/usr/lib/*.so* $(find "$APPDIR/usr/plugins" -name '*.so') 2>/dev/null || true

# Anything still unresolved inside the bundle is a packaging bug: fail loudly.
missing="$(LD_LIBRARY_PATH="$(ls -d "$APPDIR"/usr/lib/fallback/* | tr '\n' ':')" ldd "$APPDIR/usr/bin/ptz-control" $(find "$APPDIR/usr" -name '*.so*') 2>/dev/null | grep 'not found' | sort -u || true)"
if [ -n "$missing" ]; then
    printf 'Unresolved libraries:\n%s\n' "$missing" >&2
    exit 1
fi

# Resources: monospace font, icons, desktop entry.
for style in Regular Bold; do
    cp "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-$style.ttf" "$APPDIR/usr/share/ptz-control/fonts/"
done
for size in 16 32 64 128 256 512; do
    mkdir -p "$APPDIR/usr/share/icons/hicolor/${size}x${size}/apps"
    cp "$SRC/linux/res/icons/$size.png" "$APPDIR/usr/share/icons/hicolor/${size}x${size}/apps/ptz-control.png"
done
cp "$SRC/linux/res/ptz-control.desktop" "$APPDIR/usr/share/applications/"
cp "$SRC/linux/res/ptz-control.desktop" "$APPDIR/"
cp "$SRC/linux/res/icons/256.png" "$APPDIR/ptz-control.png"
ln -s ptz-control.png "$APPDIR/.DirIcon"
install -m 755 "$SRC/linux/res/AppRun" "$APPDIR/AppRun"

case "$ARCH" in
    x86_64) NAME="PTZ-Control-$VERSION-x86_64.AppImage" ;;
    aarch64) NAME="PTZ-Control-$VERSION-aarch64.AppImage" ;;
    *) echo "unsupported architecture $ARCH" >&2; exit 1 ;;
esac
chmod -R u+rwX,go+rX,go-w "$APPDIR"
mksquashfs "$APPDIR" /tmp/app.squashfs -root-owned -noappend -comp zstd -Xcompression-level 19 -quiet >/dev/null
cat "$RUNTIME" /tmp/app.squashfs > "$OUT/$NAME"
chmod 755 "$OUT/$NAME"
printf 'Built %s (%s bundled libraries)\n' "$NAME" "$(find "$APPDIR/usr/lib" -name '*.so*' | wc -l)"
