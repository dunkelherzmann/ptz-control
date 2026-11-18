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
trap 'kill "$MOCK" "${STILL_MOCK:-}" 2>/dev/null || true; defaults delete "$TEST_ID" >/dev/null 2>&1 || true; rm -rf "$WORK"' EXIT

ditto "$APP" "$COPY"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $TEST_ID" "$COPY/Contents/Info.plist"
codesign --force --sign - "$COPY" 2>/dev/null
BIN="$COPY/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$COPY/Contents/Info.plist")"

PORT=18081
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
sleep 1

settings() {  # camera username [port] [vendor: 0 ONVIF, 1 Axis]
    printf '{"route":0,"vendor":%s,"cameraHost":"127.0.0.1","cameraPort":%s,"cameraUser":"%s","httpsPort":443,"profile":0,"timeout":15,"sshHost":"","sshUser":"","sshPort":22,"sshIdentity":""}' "${3:-0}" "${2:-$PORT}" "$1" \
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
check "run 3 moved to the pan and tilt aimed at on the range plane" \
    "$(grep -q '^mock: onvif absolute move to pan=0.4500 tilt=0.3000' "$WORK/mock-still.log" && echo 0 || echo 1)"
check "run 3 fell back to still pictures" "$(grep -qx 'no live view' "$WORK/live3.txt" && echo 0 || echo 1)"
check "camera-picture pictures saved" "$([ -s "$OUT/macos-picture.png" ] && [ -s "$OUT/macos-picture-aim.png" ] && echo 0 || echo 1)"

# Run 4: the live view. The camera's H.264 video plays under the range plane, and aiming on it moves the camera. Show
# Grid is turned off first (the window picture shows the plane without its grid), and stays off for the next launch.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin)"
rm -f "$OUT/macos-live.png"
MOVES_BEFORE="$(moves)"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PICTURE=1 PTZ_SNAPSHOT_PATH="$OUT/macos-live.png" \
    PTZ_SNAPSHOT_HIDE_GRID=1 PTZ_SNAPSHOT_LIVE_PATH="$WORK/live4.txt" "$BIN" >"$WORK/app4.log" 2>&1 || true
check "run 4 played the camera's video ($(cat "$WORK/live4.txt" 2>/dev/null))" \
    "$(grep -qx 'H.264 · 320 × 180' "$WORK/live4.txt" 2>/dev/null; echo $?)"
check "run 4 started the video over RTSP" "$(grep -q '^mock: rtsp play' "$WORK/mock.log" && echo 0 || echo 1)"
check "run 4 aimed on the range plane over the video" "$([ "$(moves)" -gt "$MOVES_BEFORE" ] && echo 0 || echo 1)"
check "Show Grid off is remembered" "$([ "$(defaults read "$TEST_ID" ShowGrid 2>/dev/null)" = 0 ] && echo 0 || echo 1)"
check "live view picture saved" "$([ -s "$OUT/macos-live.png" ] && echo 0 || echo 1)"

# Run 5: an Axis camera whose view isn't measured yet. Double-clicking a point of its picture (half way to the right
# edge, a quarter of the way to the bottom) measures the view, then moves so that point comes to the centre: the mock
# draws a 60 x 35 degree view through a lens, so that point is pan 16.10, tilt -4.46 from pan 0, tilt 0.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin "$STILL_PORT" 1)"
defaults write "$TEST_ID" ShowGrid -bool true
rm -f "$OUT/macos-measured.png"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PICTURE=1 PTZ_SNAPSHOT_PATH="$OUT/macos-measured.png" \
    PTZ_SNAPSHOT_MEASURE_AT="0.5,-0.25" PTZ_SNAPSHOT_WAIT=45 "$BIN" >"$WORK/app5.log" 2>&1 || true
LAST="$(grep '^mock: axis absolute move' "$WORK/mock-still.log" | tail -1)"
check "run 5 measured the view and moved to the point double-clicked ($LAST)" "$(printf '%s\n' "$LAST" | awk '{
    for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
    exit !(v["pan"] > 15.7 && v["pan"] < 16.5 && v["tilt"] < -4.3 && v["tilt"] > -4.65) }'; echo $?)"
check "run 5 remembered the measured view for this camera" \
    "$(defaults read "$TEST_ID" "Views axis 127.0.0.1:$STILL_PORT" >/dev/null 2>&1; echo $?)"
check "measured-view picture saved" "$([ -s "$OUT/macos-measured.png" ] && echo 0 || echo 1)"

if [ "$FAIL" = 0 ]; then
    printf 'PASS PTZ Control %s (test copy): pictures in %s\n' "$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)" "$OUT"
else
    cat "$WORK/app1.log" "$WORK/app2.log" "$WORK/app3.log" "$WORK/app4.log" "$WORK/app5.log" "$WORK/mock.log" "$WORK/mock-still.log"
    exit 1
fi
