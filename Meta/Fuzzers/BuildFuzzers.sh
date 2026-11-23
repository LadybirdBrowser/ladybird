#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
LADYBIRD_SOURCE_DIR="${DIR}/../.."
cd "${LADYBIRD_SOURCE_DIR}"

. "Meta/Utils/find_compiler.sh"
pick_host_compiler --clang-only

echo "Building Fuzzers..."

if [ "$#" -gt "0" ] && [ "--oss-fuzz" = "$1" ] ; then
    echo "Building for oss-fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$LADYBIRD_SOURCE_DIR"/Build/fuzzers \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_FUZZERS_OSSFUZZ=ON \
        -DFUZZER_DICTIONARY_DIRECTORY="$OUT" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS -DOSS_FUZZ=ON" \
        -DLINKER_FLAGS="$LIB_FUZZING_ENGINE"
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/fuzzers
    cp "$LADYBIRD_SOURCE_DIR"/Build/fuzzers/bin/Fuzz* "$OUT"/
elif [ "$#" -gt "0" ] && [ "--standalone" = "$1" ] ; then
    echo "Building for standalone fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone \
        -DENABLE_FUZZERS=ON
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers-standalone
else
    echo "Building for local fuzz configuration..."
    cmake -S "$LADYBIRD_SOURCE_DIR" -GNinja --preset Fuzzers -B "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}"
    ninja -C "$LADYBIRD_SOURCE_DIR"/Build/lagom-fuzzers
fi
