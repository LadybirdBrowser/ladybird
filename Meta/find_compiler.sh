# shellcheck shell=bash

HOST_COMPILER=""

is_supported_compiler() {
    local COMPILER="$1"
    if [ -z "$COMPILER" ]; then
        return 1
    fi

    local VERSION=""
    VERSION="$($COMPILER -dumpversion 2> /dev/null)" || return 1
    local MAJOR_VERSION=""
    MAJOR_VERSION="${VERSION%%.*}"
    if $COMPILER --version 2>&1 | grep "Apple clang" >/dev/null; then
        # Apple Clang version check
        BUILD_VERSION=$(echo | $COMPILER -dM -E - | grep __apple_build_version__ | cut -d ' ' -f3)
        # Xcode 14.3, based on upstream LLVM 15
        [ "$BUILD_VERSION" -ge 14030022 ] && return 0
    elif $COMPILER --version 2>&1 | grep "clang" >/dev/null; then
        # Clang version check
        [ "$MAJOR_VERSION" -ge 17 ] && return 0
    else
        # GCC version check
        [ "$MAJOR_VERSION" -ge 13 ] && return 0
    fi
    return 1
}

find_newest_compiler() {
    local BEST_VERSION=0
    local BEST_CANDIDATE=""
    for CANDIDATE in "$@"; do
        if ! command -v "$CANDIDATE" >/dev/null 2>&1; then
            continue
        fi
        if ! $CANDIDATE -dumpversion >/dev/null 2>&1; then
            continue
        fi
        local VERSION=""
        VERSION="$($CANDIDATE -dumpversion)"
        local MAJOR_VERSION="${VERSION%%.*}"
        if [ "$MAJOR_VERSION" -gt "$BEST_VERSION" ]; then
            BEST_VERSION=$MAJOR_VERSION
            BEST_CANDIDATE="$CANDIDATE"
        fi
    done
    HOST_COMPILER=$BEST_CANDIDATE
}

pick_host_compiler() {
    CC=${CC:-"cc"}
    CXX=${CXX:-"c++"}

    if is_supported_compiler "$CC" && is_supported_compiler "$CXX"; then
        return
    fi

    find_newest_compiler clang clang-17 clang-18 clang-19 clang-20 /opt/homebrew/opt/llvm/bin/clang
    if is_supported_compiler "$HOST_COMPILER"; then
        export CC="${HOST_COMPILER}"
        export CXX="${HOST_COMPILER/clang/clang++}"
        return
    fi

    find_newest_compiler egcc gcc gcc-13 gcc-14 /usr/local/bin/gcc-{13,14} /opt/homebrew/bin/gcc-{13,14}
    if is_supported_compiler "$HOST_COMPILER"; then
        export CC="${HOST_COMPILER}"
        export CXX="${HOST_COMPILER/gcc/g++}"
        return
    fi

    if [ "$(uname -s)" = "Darwin" ]; then
        die "Please make sure that Xcode 14.3, Homebrew Clang 17, or higher is installed."
    else
        die "Please make sure that GCC version 13, Clang version 17, or higher is installed."
    fi
}
