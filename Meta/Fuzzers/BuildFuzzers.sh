#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
LADYBIRD_SOURCE_DIR="${DIR}/../.."
cd "${LADYBIRD_SOURCE_DIR}"

if [ "$#" -gt "0" ] && [ "--oss-fuzz" = "$1" ] ; then
    : "${OUT:?OSS-Fuzz must provide OUT}"
    : "${CC:?OSS-Fuzz must provide CC}"
    : "${CXX:?OSS-Fuzz must provide CXX}"
    : "${LIB_FUZZING_ENGINE:?OSS-Fuzz must provide LIB_FUZZING_ENGINE}"

    echo "Building for oss-fuzz configuration..."

    oss_fuzz_cflags="${CFLAGS:-}"
    oss_fuzz_cxxflags="${CXXFLAGS:-}"
    oss_fuzz_vcpkg_cxxflags="${CXXFLAGS_EXTRA:-}"

    if [ "${SANITIZER:-}" = "undefined" ]; then
        # vptr checks require RTTI, but Ladybird's static Skia package is built without it.
        oss_fuzz_cxxflags="${oss_fuzz_cxxflags} -fno-sanitize=vptr"
    fi

    # Autoconf-based vcpkg ports cannot run configure probes linked with libFuzzer.
    # Keep those dependencies uninstrumented while preserving the target's libc++ ABI.
    unset CFLAGS CXXFLAGS

    VCPKG_ROOT="${VCPKG_ROOT:-${LADYBIRD_SOURCE_DIR}/Build/vcpkg-distribution}"
    if [ ! -x "${VCPKG_ROOT}/vcpkg" ]; then
        python3 Meta/ladybird.py vcpkg --preset Fuzzers --jobs "${JOBS:-$(nproc)}"
    fi
    export VCPKG_ROOT

    build_dir="${WORK:-${LADYBIRD_SOURCE_DIR}/Build}/ladybird-fuzzers-${SANITIZER:-default}"
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$build_dir" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_GUI_TARGETS=OFF \
        -DENABLE_FUZZERS_OSSFUZZ=ON \
        -DFUZZER_OUTPUT_DIRECTORY="$OUT" \
        -DFUZZING_ENGINE="$LIB_FUZZING_ENGINE" \
        -DLADYBIRD_CACHE_DIR="${WORK:-${LADYBIRD_SOURCE_DIR}/Build/caches}" \
        -DLADYBIRD_VCPKG_TYPE=distribution \
        -DLADYBIRD_VCPKG_C_FLAGS= \
        -DLADYBIRD_VCPKG_CXX_FLAGS="${oss_fuzz_vcpkg_cxxflags}" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_C_FLAGS="${oss_fuzz_cflags}" \
        -DCMAKE_CXX_FLAGS="${oss_fuzz_cxxflags}"
    cmake --build "$build_dir" --target fuzzers --parallel "${JOBS:-$(nproc)}"
elif [ "$#" -gt "0" ] && [ "--fuzzilli" = "$1" ] ; then
    echo "Building for local Fuzzilli configuration..."

    . "Meta/Utils/find_compiler.sh"
    pick_host_compiler --clang-only
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja --preset Fuzzers -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzilli \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_GUI_TARGETS=OFF \
        -DENABLE_FUZZERS_LIBFUZZER=OFF \
        -DENABLE_FUZZERS_FUZZILLI=ON \
        -DENABLE_UNDEFINED_SANITIZER=ON \
        -DUNDEFINED_BEHAVIOR_IS_FATAL=ON \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}"
    cmake --build "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzilli --target FuzzilliJs --parallel "${JOBS:-$(nproc)}"
elif [ "$#" -gt "0" ] && [ "--standalone" = "$1" ] ; then
    echo "Building for standalone fuzz configuration..."

    . "Meta/Utils/find_compiler.sh"
    pick_host_compiler --clang-only
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone \
        -DENABLE_FUZZERS=ON
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone
else
    echo "Building for local fuzz configuration..."

    . "Meta/Utils/find_compiler.sh"
    pick_host_compiler --clang-only
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja --preset Fuzzers -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}"
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers
fi
