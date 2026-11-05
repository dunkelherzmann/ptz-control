#!/bin/sh
# Runs inside a stock distribution container (see scripts/test-linux.sh): installs
# only what a desktop already has, then drives the AppImage against the mock camera
# under Xvfb and saves a screenshot.
# Usage: test-appimage.sh <src-root> <appimage> <screenshot.png>
set -eu
SRC="$1"
APP="$2"
SHOT="$3"

. /etc/os-release
case "$ID" in
    ubuntu|debian)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq >/dev/null
        apt-get install -y -qq --no-install-recommends xvfb python3 libgl1 libegl1 libx11-6 libx11-xcb1 libxcb1 \
            libfontconfig1 fonts-dejavu-core >/dev/null ;;
    fedora)
        dnf install -y -q xorg-x11-server-Xvfb python3 mesa-libGL mesa-libEGL libX11 libX11-xcb libxcb fontconfig \
            dejavu-sans-fonts >/dev/null ;;
    arch)
        pacman -Sy --disable-sandbox --noconfirm --needed -q xorg-server-xvfb python mesa libx11 libxcb fontconfig ttf-dejavu >/dev/null ;;
    opensuse*)
        zypper -q -n install xvfb-run python3 Mesa-libGL1 Mesa-libEGL1 libX11-6 libX11-xcb1 libxcb1 fontconfig \
            dejavu-fonts >/dev/null ;;
    *) echo "unknown distribution $ID" >&2; exit 1 ;;
esac

WORK="$(mktemp -d)"
cp "$APP" "$WORK/app.AppImage"
cd "$WORK"
# Emulators (Rosetta, qemu-user) match the whole ELF identification block and
# reject the "AI\2" AppImage marker in its padding; real kernels ignore it.
# Clear it in this test copy only.
printf '\000\000\000' | dd of=app.AppImage bs=1 seek=8 conv=notrunc 2>/dev/null

# Every library the AppImage needs must come from the bundle or this system.
./app.AppImage --appimage-extract >/dev/null
export LD_LIBRARY_PATH="$(PTZ_APPRUN_PRINT_LIBRARY_PATH=1 squashfs-root/AppRun)"
missing="$(ldd squashfs-root/usr/bin/ptz-control $(find squashfs-root/usr -name '*.so*') 2>/dev/null | grep 'not found' | sort -u || true)"
if [ -n "$missing" ]; then
    printf 'FAIL %s: unresolved libraries\n%s\n' "$PRETTY_NAME" "$missing"
    exit 1
fi
[ -n "$LD_LIBRARY_PATH" ] && printf 'Using bundled fallbacks: %s\n' "$LD_LIBRARY_PATH"
unset LD_LIBRARY_PATH

python3 "$SRC/tools/mock-camera.py" --port 8080 >"$WORK/mock.log" 2>&1 &
mkdir -p "$HOME/.config/PTZControl"
printf '[General]\nRoute=0\nVendor=0\nCameraHost=127.0.0.1\nCameraPort=8080\nCameraUser=admin\n' \
    > "$HOME/.config/PTZControl/PTZControl.conf"
Xvfb :99 -screen 0 1400x1000x24 -nolisten tcp >/dev/null 2>&1 &
sleep 2
rm -f "$SHOT"
DISPLAY=:99 APPIMAGE_EXTRACT_AND_RUN=1 PTZ_PASSWORD=secret PTZ_SNAPSHOT_PATH="$SHOT" PTZ_SNAPSHOT_RETURN=1 \
    PTZ_THEME="${PTZ_THEME:-}" timeout 60 ./app.AppImage >"$WORK/app.log" 2>&1 || true
if [ -s "$SHOT" ]; then
    printf 'PASS %s %s\n' "$PRETTY_NAME" "$(uname -m)"
else
    printf 'FAIL %s %s: no screenshot\n' "$PRETTY_NAME" "$(uname -m)"
    cat "$WORK/app.log"
    exit 1
fi
