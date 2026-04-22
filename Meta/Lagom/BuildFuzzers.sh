#!/usr/bin/env bash

set -e

LADYBIRD_SOURCE_DIR="$(dirname "${0}")"/../..
cd "${LADYBIRD_SOURCE_DIR}"

die() {
    >&2 echo "die: $*"
    exit 1
}

# Save flags for oss-fuzz to avoid fuzzing Tools/
# https://google.github.io/oss-fuzz/getting-started/new-project-guide/#temporarily-disabling-code-instrumentation-during-builds
CFLAGS_SAVE="$CFLAGS"
CXXFLAGS_SAVE="$CXXFLAGS"
unset CFLAGS
unset CXXFLAGS
export AFL_NOOPT=1

if [ "$#" -gt "0" ] && [ "--oss-fuzz" = "$1" ] ; then
    CXXFLAGS="$CXXFLAGS -DOSS_FUZZ=ON"
fi

# FIXME: Replace these CMake invocations with a CMake superbuild?
echo "Building Lagom Tools..."

. "Meta/Utils/find_compiler.sh"
pick_host_compiler --clang-only

cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja --preset=Host_Tools \
    -B "$LADYBIRD_SOURCE_DIR"/Build/host-tools-build \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}"
ninja -C "$LADYBIRD_SOURCE_DIR"/Build/host-tools-build install

# Restore flags for oss-fuzz
export CFLAGS="${CFLAGS_SAVE}"
export CXXFLAGS="${CXXFLAGS_SAVE}"
unset AFL_NOOPT

echo "Building Lagom Fuzzers..."

if [ "$#" -gt "0" ] && [ "--oss-fuzz" = "$1" ] ; then
    echo "Building for oss-fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$LADYBIRD_SOURCE_DIR"/Build/fuzzers \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_FUZZERS_OSSFUZZ=ON \
        -DFUZZER_DICTIONARY_DIRECTORY="$OUT" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS -DOSS_FUZZ=ON" \
        -DLINKER_FLAGS="$LIB_FUZZING_ENGINE" \
        -DLagomTools_DIR="$LADYBIRD_SOURCE_DIR"/Build/host-tools/share/LagomTools
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/fuzzers
    cp "$LADYBIRD_SOURCE_DIR"/Build/fuzzers/bin/Fuzz* "$OUT"/
elif [ "$#" -gt "0" ] && [ "--standalone" = "$1" ] ; then
    echo "Building for standalone fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone \
        -DENABLE_FUZZERS=ON \
        -DLagomTools_DIR="$LADYBIRD_SOURCE_DIR"/Build/host-tools/share/LagomTools
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone
else
    echo "Building for local fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja --preset Fuzzers -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers \
        -DLagomTools_DIR="$LADYBIRD_SOURCE_DIR"/Build/host-tools/share/LagomTools \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}"
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers
fi
