#!/usr/bin/env bash
#
# Build, package, codesign (ad-hoc) and install isolated-record as a loadable
# OBS .plugin on this Mac.
#
# Key correctness point: we COMPILE against Qt headers matching the Qt that
# OBS bundles (6.8.3 here), and LINK against frameworks whose install names are
# @rpath/... so at runtime dyld resolves them to OBS's own bundled frameworks
# (OBS provides the rpath @executable_path/../Frameworks). This avoids the Qt
# version-tag / ABI mismatch that would stop the plugin from loading.
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

NAME=isolated-record
VERSION=0.1.0
ARCH="${ARCH:-arm64}"

OBS_APP="${OBS_APP:-/Applications/OBS.app}"
OBSF="$OBS_APP/Contents/Frameworks"
OBS_SRC="${OBS_SRC:-$ROOT/obs-studio-src}"
QT="${QT:-$ROOT/Qt/6.8.3/macos}"
MOC="$QT/libexec/moc"
PLUGINS_DIR="${PLUGINS_DIR:-$HOME/Library/Application Support/obs-studio/plugins}"

GEN="$HERE/.gen"
OUT="$HERE/build-macos"
mkdir -p "$GEN" "$OUT"

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
#define PROJECT_VERSION_PATCH 0
EOF

cflags=(
  -std=c++20 -fPIC -Wall -arch "$ARCH" -mmacosx-version-min=12.0
  -I"$GEN" -I"$OBS_SRC/libobs" -I"$OBS_SRC/frontend/api" -I"$HERE/src" -I/opt/homebrew/include
  -I"$QT/lib/QtCore.framework/Headers"
  -I"$QT/lib/QtGui.framework/Headers"
  -I"$QT/lib/QtWidgets.framework/Headers"
  -F"$QT/lib"
)

echo "==> moc"
"$MOC" "$HERE/src/dock.hpp" -o "$OUT/moc_dock.cpp"

echo "==> compile ($ARCH)"
objs=()
for f in plugin-main isolated-record recorder-api audio-recorder dock; do
  clang++ "${cflags[@]}" -c "$HERE/src/$f.cpp" -o "$OUT/$f.o"
  objs+=("$OUT/$f.o")
done
clang++ "${cflags[@]}" -c "$OUT/moc_dock.cpp" -o "$OUT/moc_dock.o"
objs+=("$OUT/moc_dock.o")

echo "==> link"
# Link Qt frameworks by explicit binary path (clang drops a bare -framework
# QtCore). These carry @rpath install names -> resolve to OBS's bundled Qt.
clang++ "${objs[@]}" \
  -bundle -arch "$ARCH" -mmacosx-version-min=12.0 \
  -F"$OBSF" -framework libobs "$OBSF/obs-frontend-api.dylib" \
  "$QT/lib/QtWidgets.framework/QtWidgets" \
  "$QT/lib/QtGui.framework/QtGui" \
  "$QT/lib/QtCore.framework/QtCore" \
  -Wl,-rpath,"$OBSF" \
  -o "$OUT/$NAME"

echo "==> assemble .plugin bundle"
BUNDLE="$OUT/$NAME.plugin"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
cp "$OUT/$NAME" "$BUNDLE/Contents/MacOS/$NAME"
cp -R "$HERE/data/locale" "$BUNDLE/Contents/Resources/locale"

cat > "$BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>com.exeldro.$NAME</string>
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

echo "==> ad-hoc codesign"
codesign --force --sign - --timestamp=none "$BUNDLE/Contents/MacOS/$NAME"
codesign --force --sign - --timestamp=none "$BUNDLE"

echo "==> install to OBS plugins dir"
rm -rf "$PLUGINS_DIR/$NAME.plugin"
cp -R "$BUNDLE" "$PLUGINS_DIR/$NAME.plugin"

echo "==> done: $PLUGINS_DIR/$NAME.plugin"
echo "    linked Qt/libobs (otool -L):"
otool -L "$BUNDLE/Contents/MacOS/$NAME" | grep -E "Qt|libobs|frontend" | sed 's/^/      /'
