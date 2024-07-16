#!/usr/bin/env bash

set -eo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

LADYBIRD_SOURCE_DIR="$(realpath "${DIR}"/../..)"

# shellcheck source=/dev/null
. "${LADYBIRD_SOURCE_DIR}/Meta/shell_include.sh"

# shellcheck source=/dev/null
. "${LADYBIRD_SOURCE_DIR}/Meta/find_compiler.sh"

pick_host_compiler

BUILD_DIR=${BUILD_DIR:-"${LADYBIRD_SOURCE_DIR}/Build"}
CACHE_DIR=${CACHE_DIR:-"${BUILD_DIR}/caches"}

# HACK: This export of XDG_CACHE_HOME is required to make vcpkg happy.
# This is because vcpkg tries to find a cache directory by:
#   1) checking $XDG_CACHE_HOME
#   2) appending "/.cache" to $HOME
# The problem is, in the Android build environment, neither of those environment variables are set.
# This causes vcpkg to fail; so, we set a dummy $XDG_CACHE_HOME, ensuring that vcpkg is happy.
# (Note that vcpkg appends "/vcpkg" to the cache directory we give it.)
# (And this also works on macOS, despite the fact that $XDG_CACHE_HOME is a Linux-ism.)
export XDG_CACHE_HOME="$CACHE_DIR"

"$LADYBIRD_SOURCE_DIR"/Meta/ladybird.sh vcpkg

cmake -S "${LADYBIRD_SOURCE_DIR}/Meta/Lagom" -B "$BUILD_DIR/lagom-tools" \
    -GNinja -Dpackage=LagomTools \
    -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/lagom-tools-install"  \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DSERENITY_CACHE_DIR="$CACHE_DIR" \
    -DLAGOM_TOOLS_ONLY=ON \
    -DINSTALL_LAGOM_TOOLS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$LADYBIRD_SOURCE_DIR/Toolchain/Tarballs/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_INSTALL_OPTIONS="--no-print-usage" \
    -DVCPKG_OVERLAY_TRIPLETS="$LADYBIRD_SOURCE_DIR/Meta/CMake/vcpkg/release-triplets" \
    -DVCPKG_ROOT="$LADYBIRD_SOURCE_DIR/Toolchain/Tarballs/vcpkg" \
    -DVCPKG_MANIFEST_DIR="$LADYBIRD_SOURCE_DIR"

ninja -C "$BUILD_DIR/lagom-tools" install
