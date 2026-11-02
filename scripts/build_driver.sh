#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

OUT="$PWD/build/drivers"
PLIST="$PWD/driver/Info.plist"
IDENTITY="${CODESIGN_IDENTITY:-MacLoopback Dev}"

rm -rf "$OUT"
mkdir -p "$OUT/Wannengya.driver/Contents/MacOS" "$OUT/Wannengya.driver/Contents/Resources"

clang++ -bundle -std=c++17 -O2 \
    -arch arm64 -arch x86_64 \
    -mmacosx-version-min=12.0 \
    -I "$PWD/shared" \
    driver/WannengyaDriver.cpp \
    -framework CoreAudio \
    -framework CoreFoundation \
    -o "$OUT/Wannengya.driver/Contents/MacOS/Wannengya"

cp "$PLIST" "$OUT/Wannengya.driver/Contents/Info.plist"
plutil -lint "$OUT/Wannengya.driver/Contents/Info.plist" >/dev/null

if security find-identity -v -p codesigning 2>/dev/null | grep -F "\"$IDENTITY\"" >/dev/null; then
    codesign --force --deep --sign "$IDENTITY" "$OUT/Wannengya.driver"
    echo "signed with '$IDENTITY'"
else
    echo "warning: signing identity '$IDENTITY' not found; leaving unsigned" >&2
fi

echo "driver bundle: $OUT/Wannengya.driver"
