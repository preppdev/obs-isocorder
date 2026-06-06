#!/usr/bin/env bash
#
# Build a DISTRIBUTABLE macOS package for Isolated Record:
#   - dist/isolated-record-<ver>-macos-<arch>.pkg   (double-click installer,
#     installs per-user to ~/Library/Application Support/obs-studio/plugins —
#     no admin required)
#   - dist/isolated-record-<ver>-macos-<arch>.zip   (drop-in .plugin bundle)
#
# This does NOT install into your own OBS (use pack-macos.sh for that). It only
# produces artifacts under dist/.
#
# Signing: the .plugin is ad-hoc signed so it loads locally. For distribution to
# OTHER machines without Gatekeeper warnings you must sign with a Developer ID
# and notarize — see the notes at the bottom of this script.
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

NAME=isolated-record
VERSION=0.1.2
ARCH="${ARCH:-arm64}"
BUNDLE_ID=com.exeldro.${NAME}

OBS_APP="${OBS_APP:-/Applications/OBS.app}"
OBSF="$OBS_APP/Contents/Frameworks"
OBS_SRC="${OBS_SRC:-$ROOT/obs-studio-src}"
QT="${QT:-$ROOT/Qt/6.8.3/macos}"
MOC="$QT/libexec/moc"

GEN="$HERE/.gen"
WORK="$HERE/build-macos"
DIST="$HERE/dist"
mkdir -p "$GEN" "$WORK" "$DIST"
rm -rf "$DIST"/*

cat > "$GEN/obsconfig.h" <<'EOF'
#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
EOF
cat > "$HERE/src/version.h" <<EOF
#pragma once
#define PROJECT_VERSION "$VERSION"
#define PROJECT_VERSION_MAJOR 0
#define PROJECT_VERSION_MINOR 1
#define PROJECT_VERSION_PATCH 2
EOF

cflags=(
  -std=c++20 -fPIC -Wall -arch "$ARCH" -mmacosx-version-min=12.0
  -I"$GEN" -I"$OBS_SRC/libobs" -I"$OBS_SRC/frontend/api" -I"$HERE/src" -I/opt/homebrew/include
  -I"$QT/lib/QtCore.framework/Headers" -I"$QT/lib/QtGui.framework/Headers"
  -I"$QT/lib/QtWidgets.framework/Headers" -F"$QT/lib"
)

echo "==> moc + compile ($ARCH)"
"$MOC" "$HERE/src/dock.hpp" -o "$WORK/moc_dock.cpp"
objs=()
for f in plugin-main isolated-record recorder-api audio-recorder dock; do
  clang++ "${cflags[@]}" -c "$HERE/src/$f.cpp" -o "$WORK/$f.o"
  objs+=("$WORK/$f.o")
done
clang++ "${cflags[@]}" -c "$WORK/moc_dock.cpp" -o "$WORK/moc_dock.o"
objs+=("$WORK/moc_dock.o")

echo "==> link"
clang++ "${objs[@]}" -bundle -arch "$ARCH" -mmacosx-version-min=12.0 \
  -F"$OBSF" -framework libobs "$OBSF/obs-frontend-api.dylib" \
  "$QT/lib/QtWidgets.framework/QtWidgets" "$QT/lib/QtGui.framework/QtGui" \
  "$QT/lib/QtCore.framework/QtCore" -Wl,-rpath,"$OBSF" -o "$WORK/$NAME"

echo "==> assemble .plugin bundle"
BUNDLE="$WORK/$NAME.plugin"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
cp "$WORK/$NAME" "$BUNDLE/Contents/MacOS/$NAME"
cp -R "$HERE/data/locale" "$BUNDLE/Contents/Resources/locale"
cat > "$BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleSupportedPlatforms</key><array><string>MacOSX</string></array>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
EOF

# Developer ID Application identity (set CODESIGN_IDENT to e.g.
# "Developer ID Application: Your Name (TEAMID)" to sign for distribution).
CODESIGN_IDENT="${CODESIGN_IDENT:--}"
echo "==> codesign .plugin ($CODESIGN_IDENT)"
# A real Developer ID identity needs hardened runtime (--options runtime) and a
# secure timestamp for notarization; ad-hoc ("-") ignores those.
TS_FLAG=(); [[ "$CODESIGN_IDENT" != "-" ]] && TS_FLAG=(--timestamp)
codesign --force --deep --options runtime ${TS_FLAG[@]+"${TS_FLAG[@]}"} --sign "$CODESIGN_IDENT" "$BUNDLE/Contents/MacOS/$NAME"
codesign --force --deep --options runtime ${TS_FLAG[@]+"${TS_FLAG[@]}"} --sign "$CODESIGN_IDENT" "$BUNDLE"

echo "==> zip"
ditto -c -k --keepParent "$BUNDLE" "$DIST/$NAME-$VERSION-macos-$ARCH.zip"

echo "==> build .pkg (per-user install)"
PKGROOT="$WORK/pkgroot"
rm -rf "$PKGROOT"
mkdir -p "$PKGROOT"
cp -R "$BUNDLE" "$PKGROOT/"
pkgbuild --root "$PKGROOT" \
  --install-location "Library/Application Support/obs-studio/plugins" \
  --identifier "$BUNDLE_ID" --version "$VERSION" \
  "$WORK/$NAME-component.pkg"

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Isolated Record</title>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default"><pkg-ref id="$BUNDLE_ID"/></choice>
    <pkg-ref id="$BUNDLE_ID" version="$VERSION">$NAME-component.pkg</pkg-ref>
</installer-gui-script>
EOF

PKG="$DIST/$NAME-$VERSION-macos-$ARCH.pkg"
# Set CODESIGN_IDENT_INSTALLER to a "Developer ID Installer: ..." identity to
# sign the installer; otherwise it's unsigned (fine for local use).
if [[ -n "${CODESIGN_IDENT_INSTALLER:-}" ]]; then
  productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" \
    --sign "$CODESIGN_IDENT_INSTALLER" "$PKG"
else
  productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$PKG"
fi
rm -f "$WORK/$NAME-component.pkg" "$WORK/distribution.xml"

echo
echo "==> artifacts:"
ls -la "$DIST"
echo
cat <<'NOTE'
To distribute to OTHER Macs without Gatekeeper warnings, sign + notarize:
  1. CODESIGN_IDENT="Developer ID Application: NAME (TEAMID)" \
     CODESIGN_IDENT_INSTALLER="Developer ID Installer: NAME (TEAMID)" ./package-macos.sh
  2. xcrun notarytool submit dist/isolated-record-<ver>-macos-arm64.pkg \
       --apple-id <you@apple> --team-id <TEAMID> --password <app-specific-pw> --wait
  3. xcrun stapler staple dist/isolated-record-<ver>-macos-arm64.pkg
NOTE
