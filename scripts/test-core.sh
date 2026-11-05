#!/bin/sh
# Builds tests/core-smoke.c against the core and runs it with a mock camera.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/.build/tests"
mkdir -p "$OUT" && rm -f "$OUT"/*.o
clang++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/core/include" -c "$ROOT"/core/src/*.cpp
mv ./*.o "$OUT/"
clang -std=c11 -Wall -I"$ROOT/core/include" -c "$ROOT/tests/core-smoke.c" -o "$OUT/core-smoke.o"
clang++ "$OUT"/*.o -lcurl -o "$OUT/core-smoke"

python3 "$ROOT/tools/mock-camera.py" --port 8080 >"$OUT/mock.log" 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
sleep 1
"$OUT/core-smoke"
