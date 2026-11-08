#!/bin/sh
# Builds a self-contained "PTZ Control.app" (universal arm64 + x86_64) in dist/.
# Needs only the Xcode Command Line Tools. Usage: scripts/build-macos.sh [--zip]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sh "$ROOT/scripts/version.sh" --header)"
APP="$ROOT/dist/PTZ Control.app"
BUILD="$ROOT/.build"

cd "$ROOT"
for arch in arm64 x86_64; do
    printf 'Building %s…\n' "$arch"
    # Check swift's own status: a failed build leaves the previous binary in place.
    mkdir -p "$BUILD"
    STATUS=0
    swift build -c release --triple "$arch-apple-macosx14.0" --scratch-path "$BUILD/$arch" >"$BUILD/build-$arch.log" 2>&1 \
        || STATUS=$?
    grep -v "search path .* not found" "$BUILD/build-$arch.log" || true
    [ "$STATUS" = 0 ] || { printf 'Build failed for %s\n' "$arch" >&2; exit 1; }
done

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create "$BUILD/arm64/release/PTZControl" "$BUILD/x86_64/release/PTZControl" \
    -output "$APP/Contents/MacOS/PTZControl"
strip -x "$APP/Contents/MacOS/PTZControl"

ICONSET="$BUILD/AppIcon.iconset"
rm -rf "$ICONSET"
swift "$ROOT/scripts/make-icon.swift" "$ICONSET"
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>PTZ Control</string>
    <key>CFBundleDisplayName</key><string>PTZ Control</string>
    <key>CFBundleIdentifier</key><string>com.herbertcisneros.PTZControl</string>
    <key>CFBundleExecutable</key><string>PTZControl</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>PTZ Control connects to PTZ cameras and SSH hosts on your network to read and set camera coordinates.</string>
</dict>
</plist>
PLIST

# Ad-hoc signature: runs on this Mac. Use a Developer ID + notarization to distribute.
codesign --force --sign - --options runtime --timestamp=none "$APP"
codesign --verify --strict "$APP"
printf 'Built %s\n' "$APP"

if [ "${1:-}" = "--zip" ]; then
    (cd "$ROOT/dist" && ditto -c -k --keepParent "PTZ Control.app" "PTZ-Control-$VERSION-macOS.zip")
    printf 'Zipped dist/PTZ-Control-%s-macOS.zip\n' "$VERSION"
fi
