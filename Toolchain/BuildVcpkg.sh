#!/usr/bin/env bash

# This script builds the vcpkg dependency management system
set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/../Meta/shell_include.sh"

# FIXME: Make the test262-runner CI use a non-root user
ci=0
if [[ "$1" == "--ci" ]]; then
    echo "Running in CI mode, will not check for root user"
    ci=1
    shift
fi

if [ "$ci" -eq 0 ]; then
    exit_if_running_as_root "Do not run BuildVcpkg.sh as root, parts of your Toolchain directory will become root-owned"
fi

GIT_REPO="https://github.com/microsoft/vcpkg.git"
GIT_REV="01f602195983451bc83e72f4214af2cbc495aa94" # 2024.05.24
PREFIX_DIR="$DIR/Local/vcpkg"

mkdir -p "$DIR/Tarballs"
pushd "$DIR/Tarballs"
    if [[ ! -d vcpkg ]]; then
        git clone "${GIT_REPO}"
    else
        bootstrapped_vcpkg_version=$(git -C vcpkg rev-parse HEAD)

        if [[ "${bootstrapped_vcpkg_version}" == "${GIT_REV}" ]]; then
            exit 0
        fi
    fi

    echo "Building vcpkg"

    cd vcpkg
    git fetch origin
    git checkout $GIT_REV

    ./bootstrap-vcpkg.sh -disableMetrics

    mkdir -p "$PREFIX_DIR/bin"
    cp vcpkg "$PREFIX_DIR/bin"
popd
