#!/bin/sh
# Drives the installed Flatpak (org.ptzcontrol.app) against the mock camera, off screen, and saves a
# screenshot. It uses its own settings folder and never touches the user's PTZ Control settings or windows.
# Usage: scripts/test-linux.sh [screenshot.png]   (default: .build/test/flatpak.png)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID=org.ptzcontrol.app
SHOT="$(realpath -m "${1:-$ROOT/.build/test/flatpak.png}")"
WORK="$(mktemp -d "$ROOT/.build/test-XXXXXX")"
mkdir -p "$(dirname "$SHOT")" "$WORK/config/PTZControl"
trap 'kill "$MOCK" 2>/dev/null || true; rm -rf "$WORK"' EXIT

PORT=18080
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
printf '[General]\nRoute=0\nVendor=0\nCameraHost=127.0.0.1\nCameraPort=%s\nCameraUser=admin\n' "$PORT" \
    > "$WORK/config/PTZControl/PTZControl.conf"
sleep 1
rm -f "$SHOT"
# Flatpak sets XDG_CONFIG_HOME itself, so the test settings folder is chosen inside the sandbox.
timeout 60 flatpak run --filesystem="$WORK" --filesystem="$(dirname "$SHOT")" --command=sh \
    --env=QT_QPA_PLATFORM=offscreen --env=PTZ_PASSWORD=secret --env=PTZ_SNAPSHOT_PATH="$SHOT" --env=PTZ_SNAPSHOT_RETURN=1 \
    "$ID" -c 'XDG_CONFIG_HOME="$1" exec ptz-control' sh "$WORK/config" >"$WORK/app.log" 2>&1 || true
# The app connected and drove the camera: the mock saw its ONVIF requests.
REQUESTS="$(grep -c '"POST ' "$WORK/mock.log" || true)"
if [ -s "$SHOT" ] && [ "$REQUESTS" -gt 3 ]; then
    printf 'PASS %s %s: %s camera requests → %s\n' "$ID" "$(flatpak info "$ID" | sed -n 's/^ *Version: //p')" "$REQUESTS" "$SHOT"
else
    printf 'FAIL %s: screenshot %s, %s camera requests\n' "$ID" "$([ -s "$SHOT" ] && echo saved || echo missing)" "$REQUESTS"
    cat "$WORK/app.log" "$WORK/mock.log"
    exit 1
fi
