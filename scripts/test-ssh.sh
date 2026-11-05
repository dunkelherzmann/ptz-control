#!/bin/sh
# Exercises the SSH route without touching system settings: a user-level sshd on
# 127.0.0.1:2222 with throwaway keys, the mock camera, and tests/ssh-smoke.c.
# ~/.ssh/known_hosts is backed up first and restored afterwards.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/.build/tests"
WORK="$(mktemp -d /tmp/ptz-sshtest-XXXXXX)"
PORT=2222
CC="${CC:-clang}"
CXX="${CXX:-clang++}"
KNOWN="$HOME/.ssh/known_hosts"
mkdir -p "$OUT"

if [ -f "$KNOWN" ]; then cp -p "$KNOWN" "$WORK/known_hosts.backup"; fi
cleanup() {
    [ -f "$WORK/sshd.pid" ] && kill "$(cat "$WORK/sshd.pid")" 2>/dev/null || true
    [ -n "${MOCK:-}" ] && kill "$MOCK" 2>/dev/null || true
    if [ -f "$WORK/known_hosts.backup" ]; then cp -p "$WORK/known_hosts.backup" "$KNOWN"; else rm -f "$KNOWN"; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -f "$WORK/host_key"
ssh-keygen -q -t ed25519 -N 'test-passphrase' -f "$WORK/client_key"
cp "$WORK/client_key.pub" "$WORK/authorized_keys"
cat > "$WORK/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $WORK/host_key
AuthorizedKeysFile $WORK/authorized_keys
PidFile $WORK/sshd.pid
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
AllowTcpForwarding yes
EOF
/usr/sbin/sshd -f "$WORK/sshd_config" -E "$WORK/sshd.log"
sleep 1

python3 "$ROOT/tools/mock-camera.py" --port 8080 >"$WORK/mock.log" 2>&1 &
MOCK=$!
sleep 1

"$CXX" -std=c++20 -O1 -I"$ROOT/core/include" -c "$ROOT"/core/src/*.cpp
mv ./*.o "$WORK/"
"$CC" -std=c11 -I"$ROOT/core/include" -c "$ROOT/tests/ssh-smoke.c" -o "$WORK/ssh-smoke.o"
"$CXX" "$WORK"/*.o -lcurl -o "$OUT/ssh-smoke"

PTZ_TEST_SSH_PORT=$PORT PTZ_TEST_KEY="$WORK/client_key" PTZ_TEST_KEY_PASSPHRASE='test-passphrase' \
    "$OUT/ssh-smoke" || { echo "--- sshd log"; tail -20 "$WORK/sshd.log"; exit 1; }
