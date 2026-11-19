#!/bin/sh
# Builds tests/core-smoke.c and tests/vault-smoke.c against the core and runs them:
# the vault test on its own, the core test against a mock camera.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/.build/tests"
mkdir -p "$OUT" && rm -f "$OUT"/*.o
clang++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/core/include" -c "$ROOT"/core/src/*.cpp
mv ./*.o "$OUT/"
clang -std=c11 -Wall -I"$ROOT/core/include" -c "$ROOT/tests/core-smoke.c" -o "$OUT/core-smoke.o"
clang -std=c11 -Wall -I"$ROOT/core/include" -c "$ROOT/tests/vault-smoke.c" -o "$OUT/vault-smoke.o"
CORE_OBJS=$(ls "$OUT"/*.o | grep -v -e core-smoke.o -e vault-smoke.o)
clang++ $CORE_OBJS "$OUT/core-smoke.o" -lcurl -o "$OUT/core-smoke"
clang++ $CORE_OBJS "$OUT/vault-smoke.o" -lcurl -o "$OUT/vault-smoke"
mkdir -p "$OUT/vault" && "$OUT/vault-smoke" "$OUT/vault"

python3 "$ROOT/tools/mock-camera.py" --port 8080 >"$OUT/mock.log" 2>&1 &
MOCK=$!
python3 "$ROOT/tools/mock-camera.py" --port 8090 --video h265 >"$OUT/mock-h265.log" 2>&1 &
MOCK_H265=$!
python3 "$ROOT/tools/mock-camera.py" --port 8081 --plain-png >"$OUT/mock-plain.log" 2>&1 &
MOCK_PLAIN=$!
python3 "$ROOT/tools/mock-camera.py" --port 8082 --plain-png --onvif-real >"$OUT/mock-real.log" 2>&1 &
MOCK_REAL=$!
python3 "$ROOT/tools/mock-camera.py" --port 8083 --plain-png --onvif-real --wet >"$OUT/mock-wet.log" 2>&1 &
MOCK_WET=$!
python3 "$ROOT/tools/mock-camera.py" --port 8084 --plain-png --onvif-real --picture-lag 1 >"$OUT/mock-lag.log" 2>&1 &
MOCK_LAG=$!
trap 'kill $MOCK $MOCK_H265 $MOCK_PLAIN $MOCK_REAL $MOCK_WET $MOCK_LAG 2>/dev/null' EXIT
# Wait until the mock cameras answer (up to 10 s): a fixed one-second pause was too short on a busy machine.
for port in 8080 8090 8081 8082 8083 8084; do
    for _ in $(seq 1 50); do nc -z 127.0.0.1 $port 2>/dev/null && break; sleep 0.2; done
done
MOCK_REAL_LOG="$OUT/mock-real.log" MOCK_WET_LOG="$OUT/mock-wet.log" MOCK_LAG_LOG="$OUT/mock-lag.log" "$OUT/core-smoke"
