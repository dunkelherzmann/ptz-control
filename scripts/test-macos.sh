#!/bin/sh
# Drives a private test copy of the Mac app against the mock camera, off screen, and checks the
# saved camera logins: run 1 types a login, ticks Save login and connects; run 2 starts with a
# wrong username and fills the fields from the saved login. The test copy has its own bundle id,
# settings and vault file, so it never touches the user's PTZ Control, settings or saved logins.
# Usage: scripts/test-macos.sh   (builds dist/PTZ Control.app first unless it exists)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/dist/PTZ Control.app"
[ -d "$APP" ] || "$ROOT/scripts/build-macos.sh" >/dev/null
OUT="$ROOT/.build/test"
WORK="$(mktemp -d "$ROOT/.build/test-XXXXXX")"
TEST_ID=com.herbertcisneros.PTZControl.test
COPY="$WORK/PTZ Control Test.app"
mkdir -p "$OUT"
trap 'kill "$MOCK" 2>/dev/null || true; defaults delete "$TEST_ID" >/dev/null 2>&1 || true; rm -rf "$WORK"' EXIT

ditto "$APP" "$COPY"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $TEST_ID" "$COPY/Contents/Info.plist"
codesign --force --sign - "$COPY" 2>/dev/null
BIN="$COPY/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$COPY/Contents/Info.plist")"

PORT=18081
python3 -u "$ROOT/tools/mock-camera.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
sleep 1

settings() {  # camera username
    printf '{"route":0,"vendor":0,"cameraHost":"127.0.0.1","cameraPort":%s,"cameraUser":"%s","httpsPort":443,"profile":0,"timeout":15,"sshHost":"","sshUser":"","sshPort":22,"sshIdentity":""}' "$PORT" "$1" \
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

# Run 1: typed login, Save login ticked.
defaults write "$TEST_ID" ConnectionSettings -data "$(settings admin)"
rm -f "$OUT/macos.png"
PTZ_VAULT_PATH="$VAULT" PTZ_PASSWORD=secret PTZ_SNAPSHOT_PATH="$OUT/macos.png" PTZ_SNAPSHOT_SAVE_LOGIN=1 \
    "$BIN" >"$WORK/app1.log" 2>&1 || true
check "run 1 connected and moved" "$([ "$(moves)" -ge 1 ] && echo 0 || echo 1)"
check "run 1 saved the camera login" "$(printf 'camera\t127.0.0.1\t%s\tadmin\tsecret\t\n' "$PORT" | grep -qxF -f - "$VAULT" && echo 0 || echo 1)"
check "the vault file is private (0600)" "$([ "$(stat -f %Lp "$VAULT")" = 600 ] && echo 0 || echo 1)"
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

if [ "$FAIL" = 0 ]; then
    printf 'PASS PTZ Control %s (test copy): pictures in %s\n' "$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)" "$OUT"
else
    cat "$WORK/app1.log" "$WORK/app2.log" "$WORK/mock.log"
    exit 1
fi
