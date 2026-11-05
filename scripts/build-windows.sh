#!/bin/sh
# Cross-compiles dist/PTZControl.exe (Windows 10/11 x64) on macOS or Linux with
# mingw-w64 (brew install mingw-w64 / apt install mingw-w64). Fully static:
# one portable .exe, no DLLs to ship, no installer, no administrator rights.
# Usage: scripts/build-windows.sh [--zip]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.0.0"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
OBJ="$ROOT/.build/windows"
mkdir -p "$OBJ" "$ROOT/dist"
command -v "$CXX" >/dev/null || { printf '%s not found; install mingw-w64\n' "$CXX" >&2; exit 1; }

FLAGS="-std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
       -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
       -I$ROOT/core/include -ffunction-sections -fdata-sections"

for src in "$ROOT"/core/src/*.cpp "$ROOT"/windows/src/*.cpp; do
    printf 'Compiling %s\n' "${src#$ROOT/}"
    # shellcheck disable=SC2086
    "$CXX" $FLAGS -c "$src" -o "$OBJ/$(basename "$src" .cpp).o"
done

# Icon: the same artwork as the macOS app.
if [ ! -f "$ROOT/windows/res/app.ico" ]; then
    swift "$ROOT/scripts/make-icon.swift" "$OBJ/AppIcon.iconset"
    python3 "$ROOT/scripts/make-ico.py" "$OBJ/AppIcon.iconset" "$ROOT/windows/res/app.ico"
fi
"$WINDRES" -I"$ROOT/windows/res" "$ROOT/windows/res/app.rc" -O coff -o "$OBJ/app.res.o"

# shellcheck disable=SC2046
"$CXX" -o "$ROOT/dist/PTZControl.exe" $(ls "$OBJ"/*.o) \
    -municode -mwindows -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s \
    -lwinhttp -lws2_32 -lbcrypt -lgdiplus -lcomctl32 -ldwmapi -luxtheme -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32
printf 'Built dist/PTZControl.exe\n'

if [ "${1:-}" = "--zip" ]; then
    (cd "$ROOT/dist" && rm -f "PTZ-Control-$VERSION-Windows-x64.zip" && zip -q "PTZ-Control-$VERSION-Windows-x64.zip" PTZControl.exe)
    printf 'Zipped dist/PTZ-Control-%s-Windows-x64.zip\n' "$VERSION"
fi
