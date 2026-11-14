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
python3 "$ROOT/tools/mock-camera.py" --port 8081 --no-fov --plain-png >"$OUT/mock-no-fov.log" 2>&1 &
MOCK2=$!
python3 "$ROOT/tools/mock-camera.py" --port 8090 --video h265 >"$OUT/mock-h265.log" 2>&1 &
MOCK_H265=$!
trap 'kill $MOCK $MOCK2 $MOCK_H265 2>/dev/null' EXIT
# Wait until the mock cameras answer (up to 10 s): a fixed one-second pause was too short on a busy machine.
for port in 8080 8081 8090; do
    for _ in $(seq 1 50); do nc -z 127.0.0.1 $port 2>/dev/null && break; sleep 0.2; done
done
"$OUT/core-smoke"
