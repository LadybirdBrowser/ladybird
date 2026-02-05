#!/usr/bin/env bash

# This script builds the GN meta-build system
set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/../Meta/shell_include.sh"

exit_if_running_as_root "Do not run BuildGN.sh as root, parts of your Toolchain directory will become root-owned"

NPROC=$(get_number_of_processing_units)
[ -z "$MAKEJOBS" ] && MAKEJOBS=${NPROC}

GIT_REPO=https://gn.googlesource.com/gn
GIT_REV=fae280eabe5d31accc53100137459ece19a7a295
BUILD_DIR="$DIR"/Build/gn
PREFIX_DIR="$DIR/Local/gn"

mkdir -p "$DIR"/Tarballs
pushd "$DIR"/Tarballs

[ ! -d gn ] && git clone $GIT_REPO

cd gn
git fetch origin
git checkout $GIT_REV

./build/gen.py --out-path="$BUILD_DIR" --allow-warnings
ninja -C "$BUILD_DIR"

mkdir -p "$PREFIX_DIR/bin"
cp "$BUILD_DIR/gn" "$PREFIX_DIR/bin"

popd
