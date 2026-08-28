#!/bin/sh
# build_app_bundle.sh
#
# Distribution prep: assembles a real AkaizerS.app from the SwiftPM
# release binary -- `swift build` alone produces a bare Mach-O
# executable, not a macOS app bundle, which is why the app has needed
# the NSApplicationDelegate activation-policy workaround since stage 4
# (see README's "Gotchas fixed along the way"). A proper bundle with an
# Info.plist gets the right activation policy from LaunchServices
# automatically; the workaround stays in the code regardless, since
# `swift run`/the raw binary (still the normal dev workflow) still needs
# it.
#
# Deliberately UNSIGNED -- no Apple Developer ID certificate exists on
# this machine (checked via `security find-identity -v -p codesigning`).
# Running the built app is fine on this Mac; copying it to another one
# will trigger a Gatekeeper "unidentified developer" prompt, overridden
# with a right-click > Open. Real Developer ID signing + notarization
# needs an Apple Developer Program membership this project doesn't have.
#
# Usage: Scripts/build_app_bundle.sh

set -e

cd "$(dirname "$0")/.."

APP_NAME="AkaizerS"
BUNDLE_NAME="Akaizer S.app"
DIST_DIR="dist"
BUNDLE_PATH="$DIST_DIR/$BUNDLE_NAME"

echo "==> Building release binary..."
swift build -c release

echo "==> Assembling $BUNDLE_PATH..."
rm -rf "$BUNDLE_PATH"
mkdir -p "$BUNDLE_PATH/Contents/MacOS"
mkdir -p "$BUNDLE_PATH/Contents/Resources"

cp ".build/release/$APP_NAME" "$BUNDLE_PATH/Contents/MacOS/$APP_NAME"
cp "Resources/Info.plist" "$BUNDLE_PATH/Contents/Info.plist"
cp "Resources/AppIcon.icns" "$BUNDLE_PATH/Contents/Resources/AppIcon.icns"

# Deliberately unsigned, by choice -- not just "no certificate available."
# Gatekeeper will show an "unidentified developer" prompt on first launch
# (right-click > Open to override); that's expected for an unsigned app,
# not a bug in this script.

echo "==> Done: $BUNDLE_PATH"
echo "    Run it with: open \"$BUNDLE_PATH\""
