#!/bin/sh
# Drives the installed Flatpak (org.ptzcontrol.app) against the mock camera, off screen, and saves screenshots.
# Run 1 types a login, ticks Save login and connects; run 2 starts with a wrong username and fills the camera
# fields from the saved login. It uses its own settings folder and vault file and never touches the user's PTZ
# Control settings, saved logins or windows.
# Usage: scripts/test-linux.sh [screenshot.png]   (default: .build/test/flatpak.png)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID=org.ptzcontrol.app
SHOT="$(realpath -m "${1:-$ROOT/.build/test/flatpak.png}")"
SHOTS="$(dirname "$SHOT")"
WORK="$(mktemp -d "$ROOT/.build/test-XXXXXX")"
mkdir -p "$SHOTS" "$WORK/config/PTZControl"
trap 'kill "$MOCK" 2>/dev/null || true; rm -rf "$WORK"' EXIT

PORT=18080
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
settings() {  # camera username
    printf '[General]\nRoute=0\nVendor=0\nCameraHost=127.0.0.1\nCameraPort=%s\nCameraUser=%s\n' "$PORT" "$1" \
        > "$WORK/config/PTZControl/PTZControl.conf"
}
VAULT="$WORK/logins.vault"
printf 'PTZ Control logins 1\nplain\n' >"$VAULT"
chmod 600 "$VAULT"
sleep 1

run() {  # extra --env options...
    # Flatpak sets XDG_CONFIG_HOME itself, so the test settings folder is chosen inside the sandbox.
    timeout 60 flatpak run --filesystem="$WORK" --filesystem="$SHOTS" --command=sh \
        --env=QT_QPA_PLATFORM=offscreen --env=PTZ_VAULT_PATH="$VAULT" "$@" \
        "$ID" -c 'XDG_CONFIG_HOME="$1" exec ptz-control' sh "$WORK/config" >>"$WORK/app.log" 2>&1 || true
}
FAIL=0
check() {  # description, condition result
    if [ "$2" = 0 ]; then printf 'ok   %s\n' "$1"; else printf 'FAIL %s\n' "$1"; FAIL=1; fi
}
moves() { grep -c '^mock: onvif AbsoluteMove' "$WORK/mock.log" || true; }

# Run 1: typed login, Save login ticked; connect, move, return to the start.
settings admin
rm -f "$SHOT"
run --env=PTZ_PASSWORD=secret --env=PTZ_SNAPSHOT_PATH="$SHOT" --env=PTZ_SNAPSHOT_RETURN=1 --env=PTZ_SNAPSHOT_SAVE_LOGIN=1
REQUESTS="$(grep -c '"POST ' "$WORK/mock.log" || true)"
check "run 1 connected and moved ($REQUESTS camera requests)" "$([ "$(moves)" -ge 1 ] && echo 0 || echo 1)"
check "run 1 saved the camera login" \
    "$(printf 'camera\tadmin\tsecret\n' | grep -qxF -f - "$VAULT" && echo 0 || echo 1)"
check "the vault file is private (0600)" "$([ "$(stat -c %a "$VAULT")" = 600 ] && echo 0 || echo 1)"
check "window picture saved" "$([ -s "$SHOT" ] && echo 0 || echo 1)"
BEFORE="$(moves)"

# Run 2: wrong username in the settings, no PTZ_PASSWORD: only the saved login can connect.
settings nobody
LOGINS="$SHOTS/flatpak-logins.png"
rm -f "$LOGINS" "$SHOTS/flatpak-logins-form.png"
run --env=PTZ_SNAPSHOT_PATH="$SHOTS/flatpak-saved-login.png" --env=PTZ_SNAPSHOT_USE_LOGIN=1 \
    --env=PTZ_SNAPSHOT_LOGINS_PATH="$LOGINS"
check "run 2 connected with the saved login and moved" "$([ "$(moves)" -gt "$BEFORE" ] && echo 0 || echo 1)"
check "Manage Logins and form pictures saved" \
    "$([ -s "$LOGINS" ] && [ -s "$SHOTS/flatpak-logins-form.png" ] && echo 0 || echo 1)"
check "still one camera login" "$([ "$(grep -c '^camera' "$VAULT")" = 1 ] && echo 0 || echo 1)"

if [ "$FAIL" = 0 ]; then
    printf 'PASS %s %s: pictures in %s\n' "$ID" "$(flatpak info "$ID" | sed -n 's/^ *Version: //p')" "$SHOTS"
else
    cat "$WORK/app.log" "$WORK/mock.log"
    exit 1
fi
