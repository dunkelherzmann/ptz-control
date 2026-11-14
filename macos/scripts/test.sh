#!/bin/sh
# Drives a private test copy of the Mac app against the mock camera, off screen, and checks the
# saved camera logins: run 1 types a login, ticks Save login and connects; run 2 starts with a
# wrong username and fills the fields from the saved login. The test copy has its own bundle id,
# settings and vault file, so it never touches the user's PTZ Control, settings or saved logins.
# Usage: macos/scripts/test.sh   (builds dist/PTZ Control.app first unless it exists)
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/dist/PTZ Control.app"
[ -d "$APP" ] || "$ROOT/macos/scripts/build.sh" >/dev/null
OUT="$ROOT/.build/test"
WORK="$(mktemp -d "$ROOT/.build/test-XXXXXX")"
TEST_ID=com.herbertcisneros.PTZControl.test
COPY="$WORK/PTZ Control Test.app"
mkdir -p "$OUT"
trap 'kill "$MOCK" "$MOCK2" "${STILL_MOCK:-}" 2>/dev/null || true; defaults delete "$TEST_ID" >/dev/null 2>&1 || true; rm -rf "$WORK"' EXIT

ditto "$APP" "$COPY"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $TEST_ID" "$COPY/Contents/Info.plist"
codesign --force --sign - "$COPY" 2>/dev/null
BIN="$COPY/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$COPY/Contents/Info.plist")"

PORT=18081
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
# A second camera without ONVIF's field-of-view moves: its view is measured from its pictures (run 4).
PORT2=18084
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT2" --no-fov >"$WORK/mock-no-fov.log" 2>&1 &
MOCK2=$!
sleep 1

settings() {  # camera username [port]
    printf '{"route":0,"vendor":0,"cameraHost":"127.0.0.1","cameraPort":%s,"cameraUser":"%s","httpsPort":443,"profile":0,"timeout":15,"sshHost":"","sshUser":"","sshPort":22,"sshIdentity":""}' "${2:-$PORT}" "$1" \
        | xxd -p | tr -d '\n'
}
FAIL=0
check() {  # description, condition result
    if [ "$2" = 0 ]; then printf 'ok   %s\n' "$1"; else printf 'FAIL %s\n' "$1"; FAIL=1; fi
}
moves() { grep -c '^mock: onvif AbsoluteMove' "$WORK/mock.log" || true; }

VAULT="$WORK/logins.vault"
printf 'PTZ Control logins 1\nplain\n' >"$VAULT"
chmod 600 "$VAULT"

# Run 1: typed login, Save login ticked, saved as "cam1".
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin)"
rm -f "$OUT/macos.png"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PATH="$OUT/macos.png" PTZ_SNAPSHOT_SAVE_LOGIN=1 \
    PTZ_MENU_REPORT="$WORK/menus.txt" PTZ_SNAPSHOT_FRAME_PATH="$OUT/macos-window.png" \
    PTZ_SNAPSHOT_LOGIN_NAME=cam1 \
    "$BIN" >"$WORK/app1.log" 2>&1 || true
check "run 1 connected and moved" "$([ "$(moves)" -ge 1 ] && echo 0 || echo 1)"
check "run 1 saved the camera login" "$(printf 'camera\tcam1\tadmin\tsecret\n' | grep -qxF -f - "$VAULT" && echo 0 || echo 1)"
check "the vault file is private (0600)" "$([ "$(stat -f %Lp "$VAULT")" = 600 ] && echo 0 || echo 1)"
check "menu bar holds only the app menu" "$(grep -qx 'menu bar: PTZ Control' "$WORK/menus.txt" && echo 0 || echo 1)"
check "Main Menu button at the right of the header" "$(grep -qx 'main menu button: yes' "$WORK/menus.txt" && echo 0 || echo 1)"
check "Main Menu holds every menu" "$(for m in 'PTZ Control' Edit Camera View Window Help; do grep -qx "$m" "$WORK/menus.txt" || exit 1; done && echo 0 || echo 1)"
check "a shortcut reaches the in-window menu" "$(grep -q 'shortcut routed: true theme system -> green' "$WORK/menus.txt" && echo 0 || echo 1)"
check "window picture saved" "$([ -s "$OUT/macos.png" ] && echo 0 || echo 1)"
BEFORE="$(moves)"

# Run 2: wrong username in the settings, no PTZ_PASSWORD: only the saved login can connect.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings nobody)"
rm -f "$OUT/macos-logins.png"
PTZ_VAULT_PATH="$VAULT" PTZ_SNAPSHOT_PATH="$OUT/macos-saved-login.png" PTZ_SNAPSHOT_USE_LOGIN=1 \
    PTZ_SNAPSHOT_LOGINS_PATH="$OUT/macos-logins.png" "$BIN" >"$WORK/app2.log" 2>&1 || true
check "run 2 connected with the saved login and moved" "$([ "$(moves)" -gt "$BEFORE" ] && echo 0 || echo 1)"
check "Manage Logins picture saved" "$([ -s "$OUT/macos-logins.png" ] && echo 0 || echo 1)"
check "still one camera login" "$([ "$(grep -c '^camera' "$VAULT")" = 1 ] && echo 0 || echo 1)"

# Run 3: still pictures, from a camera whose video can't be reached. A picture comes on connect, a point of it is
# aimed at, the camera moves toward it, and a new picture comes once it stops.
STILL_PORT=18083
python3 -u "$ROOT/tools/mock-camera.py" --port "$STILL_PORT" --video none >"$WORK/mock-still.log" 2>&1 &
STILL_MOCK=$!
sleep 1
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin "$STILL_PORT")"
rm -f "$OUT/macos-picture.png" "$OUT/macos-picture-aim.png"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PICTURE=1 PTZ_SNAPSHOT_PATH="$OUT/macos-picture.png" \
    PTZ_SNAPSHOT_AIM_PATH="$OUT/macos-picture-aim.png" PTZ_SNAPSHOT_MOVING_PATH="$OUT/macos-picture-moving.png" \
    PTZ_SNAPSHOT_LIVE_PATH="$WORK/live3.txt" "$BIN" >"$WORK/app3.log" 2>&1 || true
check "run 3 took a picture, then a new one after the move" \
    "$([ "$(grep -c '^mock: picture' "$WORK/mock-still.log" || true)" -ge 2 ] && echo 0 || echo 1)"
check "run 3 moved toward the aimed point (right and up)" \
    "$(grep -E '^mock: onvif view move to pan=0\.[0-9]+ tilt=0\.[0-9]+' "$WORK/mock-still.log" >/dev/null && echo 0 || echo 1)"
check "run 3 fell back to still pictures" "$(grep -qx 'no live view' "$WORK/live3.txt" && echo 0 || echo 1)"
check "camera-picture pictures saved" "$([ -s "$OUT/macos-picture.png" ] && [ -s "$OUT/macos-picture-aim.png" ] && echo 0 || echo 1)"

# Run 4: a camera that can't move within its view. Moving toward the same point first measures its view (it turns a
# little on each axis and back), then sends an absolute move to the point's coordinates: 0.45 × 0.25 pan and
# 0.3 × 0.140625 tilt from where the mock starts.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin "$PORT2")"
rm -f "$OUT/macos-measured.png"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PICTURE=1 PTZ_SNAPSHOT_PATH="$OUT/macos-measured.png" \
    PTZ_SNAPSHOT_WAIT=40 "$BIN" >"$WORK/app4.log" 2>&1 || true
LAST="$(grep '^mock: onvif absolute move to' "$WORK/mock-no-fov.log" | tail -1)"
check "run 4 measured the view and moved to the point's coordinates ($LAST)" "$(printf '%s\n' "$LAST" | awk -F'[= ]' \
    '{ d1 = $7 - 0.1125; d2 = $9 - 0.0422; exit !(d1 < 0.02 && d1 > -0.02 && d2 < 0.012 && d2 > -0.012) }' && echo 0 || echo 1)"
check "measured-view picture saved" "$([ -s "$OUT/macos-measured.png" ] && echo 0 || echo 1)"

# Run 5: the live view. The camera's H.264 video plays in the plane, and aiming in it still moves the camera.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin)"
rm -f "$OUT/macos-live.png"
VIEW_MOVES_BEFORE="$(grep -c '^mock: onvif view move' "$WORK/mock.log" || true)"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PICTURE=1 PTZ_SNAPSHOT_PATH="$OUT/macos-live.png" \
    PTZ_SNAPSHOT_LIVE_PATH="$WORK/live5.txt" "$BIN" >"$WORK/app5.log" 2>&1 || true
check "run 5 played the camera's video ($(cat "$WORK/live5.txt" 2>/dev/null))" \
    "$(grep -qx 'H.264 · 320 × 180' "$WORK/live5.txt" 2>/dev/null; echo $?)"
check "run 5 started the video over RTSP" "$(grep -q '^mock: rtsp play' "$WORK/mock.log" && echo 0 || echo 1)"
check "run 5 aimed in the live view" \
    "$([ "$(grep -c '^mock: onvif view move' "$WORK/mock.log" || true)" -gt "$VIEW_MOVES_BEFORE" ] && echo 0 || echo 1)"
check "live view picture saved" "$([ -s "$OUT/macos-live.png" ] && echo 0 || echo 1)"

if [ "$FAIL" = 0 ]; then
    printf 'PASS PTZ Control %s (test copy): pictures in %s\n' "$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)" "$OUT"
else
    cat "$WORK/app1.log" "$WORK/app2.log" "$WORK/app3.log" "$WORK/app4.log" "$WORK/app5.log" "$WORK/mock.log" \
        "$WORK/mock-still.log" "$WORK/mock-no-fov.log"
    exit 1
fi
