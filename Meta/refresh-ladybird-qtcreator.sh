#!/bin/sh

set -e

if [ -z "$LADYBIRD_SOURCE_DIR" ]
then
    LADYBIRD_SOURCE_DIR="$(git rev-parse --show-toplevel)"
    echo "Ladybird root not set. This is fine! Other scripts may require you to set the environment variable first, e.g.:"
    echo "    export LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}"
fi

cd "${LADYBIRD_SOURCE_DIR}"

find . \( \
        -name Base \
        -o -name Patches \
        -o -name Ports \
        -o -name Root \
        -o -name Toolchain \
        -o -name Build \
    \) -prune \
    -o \( \
        -name '*.ipc' \
        -o -name '*.cpp' \
        -o -name '*.idl' \
        -o -name '*.c' \
        -o -name '*.h' \
        -o -name '*.in' \
        -o -name '*.css' \
        -o -name '*.cmake' \
        -o -name '*.json' \
        -o -name 'CMakeLists.txt' \
    \) \
    -print > ladybird.files
find Build/release/ \( \
        -name '*.cpp' \
        -o -name '*.idl' \
        -o -name '*.h' \
    \) \
    -print >> ladybird.files  # Append
