#!/usr/bin/env bash

set -e

SCRIPT_PATH="$(dirname "${0}")"
cd "${SCRIPT_PATH}"

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

# This will export $CC and $CXX.
find_compiler="../find_compiler.py --clang-only"

if ! eval "${find_compiler}" ; then
    die "Unable to determine clang compiler"
fi

cmake -GNinja --preset=Distribution -B Build/tools \
    -DLAGOM_TOOLS_ONLY=ON \
    -DINSTALL_LAGOM_TOOLS=ON \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DCMAKE_INSTALL_PREFIX=Build/tool-install \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -Dpackage=LagomTools
ninja -C Build/tools install

# Restore flags for oss-fuzz
export CFLAGS="${CFLAGS_SAVE}"
export CXXFLAGS="${CXXFLAGS_SAVE}"
unset AFL_NOOPT

echo "Building Lagom Fuzzers..."

if [ "$#" -gt "0" ] && [ "--oss-fuzz" = "$1" ] ; then
    echo "Building for oss-fuzz configuration..."
    cmake -GNinja -B Build/fuzzers \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_FUZZERS_OSSFUZZ=ON \
        -DFUZZER_DICTIONARY_DIRECTORY="$OUT" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS -DOSS_FUZZ=ON" \
        -DLINKER_FLAGS="$LIB_FUZZING_ENGINE" \
        -DCMAKE_PREFIX_PATH=Build/tool-install
    ninja -C Build/fuzzers
    cp Build/fuzzers/bin/Fuzz* "$OUT"/
elif [ "$#" -gt "0" ] && [ "--standalone" = "$1" ] ; then
    echo "Building for standalone fuzz configuration..."
    cmake -GNinja -B Build/lagom-fuzzers-standalone \
        -DENABLE_FUZZERS=ON \
        -DCMAKE_PREFIX_PATH=Build/tool-install
    ninja -C Build/lagom-fuzzers-standalone
else
    echo "Building for local fuzz configuration..."
    cmake -GNinja --preset Fuzzers -B Build/lagom-fuzzers \
        -DCMAKE_PREFIX_PATH=Build/tool-install \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_OSX_SYSROOT=macosx
    ninja -C Build/lagom-fuzzers
fi
