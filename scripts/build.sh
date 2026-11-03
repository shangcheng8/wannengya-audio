#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

CMAKE="${CMAKE:-third_party/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
BUILD="${BUILD:-build}"
CONFIG="${CONFIG:-Release}"

if [ ! -x "$CMAKE" ]; then
    # Fall back to a system cmake if the bundled one is absent.
    CMAKE="$(command -v cmake || true)"
fi

if [ -z "${CMAKE:-}" ]; then
    echo "cmake not found" >&2
    exit 1
fi

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-MacLoopback Dev}" \
"$CMAKE" -S . -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-MacLoopback Dev}" \
"$CMAKE" --build "$BUILD" -j "$JOBS"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-MacLoopback Dev}" \
sh cmake/postprocess_agent.sh "$BUILD/agent/wannengya Agent.app"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-MacLoopback Dev}" sh scripts/build_driver.sh

echo
echo "built:"
echo "  $BUILD/agent/wannengya Agent.app"
echo "  $BUILD/WannengyaCapture_artefacts/$CONFIG/VST3/wannengya Capture.vst3"
echo "  $BUILD/WannengyaCapture_artefacts/$CONFIG/AU/wannengya Capture.component"
echo "  $BUILD/WannengyaReturn_artefacts/$CONFIG/VST3/wannengya Return.vst3"
echo "  $BUILD/WannengyaReturn_artefacts/$CONFIG/AU/wannengya Return.component"
echo "  $BUILD/drivers/Wannengya.driver"
echo "  $BUILD/wngmon"
