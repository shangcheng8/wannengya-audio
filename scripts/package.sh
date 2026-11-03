#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

VERSION="${VERSION:-1.0.7}"
BUILD_DIR="${BUILD_DIR:-build-release}"
CMAKE="${CMAKE:-third_party/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

APP_IDENTITY="${APP_SIGN_IDENTITY:-${CODESIGN_IDENTITY:-MacLoopback Dev}}"
INSTALLER_IDENTITY="${INSTALLER_SIGN_IDENTITY:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
MAKE_DMG="${MAKE_DMG:-1}"

DIST="$PWD/dist"
STAGE="$PWD/$BUILD_DIR/package-root"
SCRIPTS="$PWD/$BUILD_DIR/package-scripts"
COMPONENT_PKG="$PWD/$BUILD_DIR/wannengya-component.pkg"
DIST_XML="$PWD/$BUILD_DIR/distribution.xml"
PKG="$DIST/wannengya-$VERSION.pkg"
DMG="$DIST/wannengya-$VERSION.dmg"

if [ -n "$NOTARY_PROFILE" ]; then
    case "$APP_IDENTITY" in
        "Developer ID Application:"*) ;;
        *)
            echo "NOTARY_PROFILE is set, so APP_SIGN_IDENTITY must be a Developer ID Application identity." >&2
            exit 1
            ;;
    esac
    if [ -z "$INSTALLER_IDENTITY" ]; then
        echo "NOTARY_PROFILE is set, so INSTALLER_SIGN_IDENTITY must be a Developer ID Installer identity." >&2
        exit 1
    fi
fi

if [ -z "${CMAKE:-}" ] || [ ! -x "$CMAKE" ]; then
    CMAKE="$(command -v cmake || true)"
fi

rm -rf "$STAGE" "$SCRIPTS" "$COMPONENT_PKG" "$DIST_XML" "$PKG" "$DMG"
mkdir -p "$DIST" "$STAGE" "$SCRIPTS"

echo "==> Release build"
CODESIGN_IDENTITY="$APP_IDENTITY" "$CMAKE" -S . -B "$BUILD_DIR" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DWNG_SKIP_PLUGIN_COPY=ON

CODESIGN_IDENTITY="$APP_IDENTITY" "$CMAKE" --build "$BUILD_DIR" -j "$JOBS"
CODESIGN_IDENTITY="$APP_IDENTITY" sh cmake/postprocess_agent.sh "$BUILD_DIR/agent/wannengya Agent.app"
CODESIGN_IDENTITY="$APP_IDENTITY" sh scripts/build_driver.sh

AGENT="$BUILD_DIR/agent/wannengya Agent.app"
CAPTURE="$BUILD_DIR/WannengyaCapture_artefacts/Release/VST3/wannengya Capture.vst3"
RETURN="$BUILD_DIR/WannengyaReturn_artefacts/Release/VST3/wannengya Return.vst3"
CAPTURE_AU="$BUILD_DIR/WannengyaCapture_artefacts/Release/AU/wannengya Capture.component"
RETURN_AU="$BUILD_DIR/WannengyaReturn_artefacts/Release/AU/wannengya Return.component"
DRIVER="$PWD/build/drivers/Wannengya.driver"

for artifact in "$AGENT" "$CAPTURE" "$RETURN" "$CAPTURE_AU" "$RETURN_AU" "$DRIVER"; do
    if [ ! -e "$artifact" ]; then
        echo "Missing build artifact: $artifact" >&2
        exit 1
    fi
done

sign() {
    path="$1"

    /usr/bin/xattr -cr "$path" 2>/dev/null || true

    case "$APP_IDENTITY" in
        "Developer ID Application:"*)
            /usr/bin/codesign --force --deep --options runtime --timestamp --sign "$APP_IDENTITY" "$path" ;;
        ""|-)
            /usr/bin/codesign --force --deep --sign - "$path" ;;
        *)
            /usr/bin/codesign --force --deep --sign "$APP_IDENTITY" "$path" ;;
    esac
}

echo "==> Staging"
mkdir -p "$STAGE/Applications" \
         "$STAGE/Library/Audio/Plug-Ins/HAL" \
         "$STAGE/Library/Audio/Plug-Ins/VST3" \
         "$STAGE/Library/Audio/Plug-Ins/Components"

/usr/bin/ditto "$DRIVER" "$STAGE/Library/Audio/Plug-Ins/HAL/Wannengya.driver"
/usr/bin/ditto "$AGENT" "$STAGE/Applications/wannengya Agent.app"
/usr/bin/ditto "$CAPTURE" "$STAGE/Library/Audio/Plug-Ins/VST3/wannengya Capture.vst3"
/usr/bin/ditto "$RETURN" "$STAGE/Library/Audio/Plug-Ins/VST3/wannengya Return.vst3"
# Logic Pro / GarageBand only see Audio Units, so the .component bundles are
# what makes the product usable there at all.
/usr/bin/ditto "$CAPTURE_AU" "$STAGE/Library/Audio/Plug-Ins/Components/wannengya Capture.component"
/usr/bin/ditto "$RETURN_AU" "$STAGE/Library/Audio/Plug-Ins/Components/wannengya Return.component"

# AGPL-3.0 requires the licence to travel with the binaries, and the third-party
# notices are required by the MIT / zlib / BSD components we link. They go into
# the app bundle before signing, so they are sealed by the signature.
STAGED_APP="$STAGE/Applications/wannengya Agent.app"
mkdir -p "$STAGED_APP/Contents/Resources"
for doc in LICENSE THIRD-PARTY-NOTICES.md; do
    if [ ! -f "$doc" ]; then
        echo "Missing $doc; refusing to build a package without it." >&2
        exit 1
    fi
    /usr/bin/ditto "$doc" "$STAGED_APP/Contents/Resources/$doc"
done

find "$STAGE" -name '._*' -delete
find "$STAGE" -name '.DS_Store' -delete

for bundle in \
    "$STAGE/Applications/wannengya Agent.app" \
    "$STAGE/Library/Audio/Plug-Ins/VST3/wannengya Capture.vst3" \
    "$STAGE/Library/Audio/Plug-Ins/VST3/wannengya Return.vst3" \
    "$STAGE/Library/Audio/Plug-Ins/Components/wannengya Capture.component" \
    "$STAGE/Library/Audio/Plug-Ins/Components/wannengya Return.component" \
    "$STAGE/Library/Audio/Plug-Ins/HAL/Wannengya.driver"
do
    sign "$bundle"
    /usr/bin/codesign --verify --deep --strict "$bundle"
done

# codesign --force rewrites the extended attributes, which drops AppleDouble
# copies of every file into the staging tree. They must go before pkgbuild, or
# they end up as duplicate payload entries.
find "$STAGE" -name '._*' -delete
find "$STAGE" -name '.DS_Store' -delete
/usr/bin/xattr -cr "$STAGE" 2>/dev/null || true

# PackageKit installs the payload as root:wheel; matching that here keeps the
# tree we sign identical to the tree that lands on disk. chown needs root, so
# it goes through sudo when available and is skipped otherwise - the installed
# files are root-owned either way because pkgbuild records the payload owner.
if [ "$(id -u)" = "0" ]; then
    /usr/sbin/chown -R root:wheel "$STAGE" || true
else
    sudo -n /usr/sbin/chown -R root:wheel "$STAGE" 2>/dev/null \
        || echo "note: staging tree stays owned by $(id -un); the installed payload is still root:wheel"
fi
/bin/chmod -R go-w "$STAGE" 2>/dev/null || true

cp scripts/pkg-preinstall "$SCRIPTS/preinstall"
cp scripts/pkg-postinstall "$SCRIPTS/postinstall"
chmod 755 "$SCRIPTS/preinstall" "$SCRIPTS/postinstall"

echo "==> Building component package"
# --ownership recommended is what makes the installed HAL bundle root:wheel.
# Without it pkgbuild records this account as the owner and coreaudiod will
# load the plug-in but publish no devices.
/usr/bin/pkgbuild --root "$STAGE" \
    --identifier com.wannengya.installer \
    --version "$VERSION" \
    --scripts "$SCRIPTS" \
    --ownership recommended \
    --install-location "/" \
    "$COMPONENT_PKG"

echo "==> Building distribution package"
sed "s/@VERSION@/$VERSION/g" packaging/distribution.xml.in > "$DIST_XML"

if [ -n "$INSTALLER_IDENTITY" ]; then
    /usr/bin/productbuild --distribution "$DIST_XML" \
        --package-path "$BUILD_DIR" \
        --resources packaging \
        --sign "$INSTALLER_IDENTITY" \
        "$PKG"
else
    /usr/bin/productbuild --distribution "$DIST_XML" \
        --package-path "$BUILD_DIR" \
        --resources packaging \
        "$PKG"
fi

if [ -n "$NOTARY_PROFILE" ]; then
    echo "==> Notarising"
    /usr/bin/xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    /usr/bin/xcrun stapler staple "$PKG"
fi

if [ "$MAKE_DMG" = "1" ]; then
    echo "==> Building disk image"
    DMG_ROOT="$BUILD_DIR/dmg-root"
    rm -rf "$DMG_ROOT"
    mkdir -p "$DMG_ROOT"
    /usr/bin/ditto "$PKG" "$DMG_ROOT/$(basename "$PKG")"
    ln -s /Applications "$DMG_ROOT/Applications"
    # Same two files next to the installer, so they are readable without
    # mounting anything first.
    /usr/bin/ditto LICENSE "$DMG_ROOT/LICENSE.txt"
    /usr/bin/ditto THIRD-PARTY-NOTICES.md "$DMG_ROOT/THIRD-PARTY-NOTICES.md"

    /usr/bin/hdiutil create -volname "万能鸭录音机架" -srcfolder "$DMG_ROOT" \
        -ov -format UDZO "$DMG"

    if [ -n "$NOTARY_PROFILE" ]; then
        /usr/bin/xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
        /usr/bin/xcrun stapler staple "$DMG"
    fi
fi

echo
echo "done:"
ls -lh "$PKG"
[ "$MAKE_DMG" = "1" ] && ls -lh "$DMG"
exit 0
