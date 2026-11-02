#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
OUT="${OUT:-build/tests}"
mkdir -p "$OUT"

CXX="${CXX:-clang++}"
INC="-I shared"

echo "== driver loopback"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_loopback" tests/driver_loopback_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_loopback"

echo "== driver property sizes"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_property" tests/driver_property_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_property"

echo "== driver timestamps"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_timestamp" tests/driver_timestamp_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_timestamp"

echo "== drift bridge: long run"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/drift_longrun" \
    tests/drift_longrun_test.cpp shared/DriftBridge.cpp shared/Resampler.cpp
"$OUT/drift_longrun"

echo "== drift bridge: block sizes and rate ratios"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/drift_blocksize" \
    tests/drift_blocksize_test.cpp shared/DriftBridge.cpp shared/Resampler.cpp
"$OUT/drift_blocksize"

echo "all tests passed"
