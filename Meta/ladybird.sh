#!/usr/bin/env bash

set -e

ARG0=$0
print_help() {
    NAME=$(basename "$ARG0")
    cat <<EOF
Usage: $NAME COMMAND [ARGS...]
  Supported COMMANDs:
    build:      Compiles the target binaries, [ARGS...] are passed through to ninja
    install:    Installs the target binary
    run:        $NAME run EXECUTABLE [ARGS...]
                    Runs the EXECUTABLE on the build host, e.g.
                    'shell' or 'js', [ARGS...] are passed through to the executable
    gdb:        Same as run, but also starts a gdb remote session.
                $NAME gdb EXECUTABLE [-ex 'any gdb command']...
                    Passes through '-ex' commands to gdb
    vcpkg:      Ensure that dependencies are available
    test:       $NAME test [TEST_NAME_PATTERN]
                    Runs the unit tests on the build host, or if TEST_NAME_PATTERN
                    is specified tests matching it.
    delete:     Removes the build environment
    rebuild:    Deletes and re-creates the build environment, and compiles the project
    addr2line:  $NAME addr2line BINARY_FILE ADDRESS
                    Resolves the ADDRESS in BINARY_FILE to a file:line. It will
                    attempt to find the BINARY_FILE in the appropriate build directory

  Examples:
    $NAME run ladybird
        Runs the Ladybird browser
    $NAME run js -A
        Runs the js(1) REPL
    $NAME test
        Runs the unit tests on the build host
    $NAME addr2line RequestServer 0x12345678
        Resolves the address 0x12345678 in the RequestServer binary
EOF
}

usage() {
    >&2 print_help
    exit 1
}

CMD=$1
[ -n "$CMD" ] || usage
shift
if [ "$CMD" = "help" ]; then
    print_help
    exit 0
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/shell_include.sh"

exit_if_running_as_root "Do not run ladybird.sh as root, your Build directory will become root-owned"

# shellcheck source=/dev/null
. "${DIR}/find_compiler.sh"

CMAKE_ARGS=()
CMD_ARGS=( "$@" )

get_top_dir() {
    git rev-parse --show-toplevel
}

create_build_dir() {
    cmake --preset default "${CMAKE_ARGS[@]}" -S "$LADYBIRD_SOURCE_DIR" -B "$BUILD_DIR"
}

cmd_with_target() {
    pick_host_compiler
    CMAKE_ARGS+=("-DCMAKE_C_COMPILER=${CC}")
    CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=${CXX}")

    # Export CC and CXX for the vcpkg toolchain, so it will pick up the same version as the ladybird build
    export CC="$CC"
    export CXX="$CXX"

    if [ ! -d "$LADYBIRD_SOURCE_DIR" ]; then
        LADYBIRD_SOURCE_DIR="$(get_top_dir)"
        export LADYBIRD_SOURCE_DIR
    fi
    BUILD_DIR="$LADYBIRD_SOURCE_DIR/Build/ladybird"
    CMAKE_ARGS+=("-DCMAKE_INSTALL_PREFIX=$LADYBIRD_SOURCE_DIR/Build/ladybird-install")

    export PATH="$LADYBIRD_SOURCE_DIR/Toolchain/Local/cmake/bin:$LADYBIRD_SOURCE_DIR/Toolchain/Local/vcpkg/bin:$PATH"
    export VCPKG_ROOT="$LADYBIRD_SOURCE_DIR/Toolchain/Tarballs/vcpkg"
}

ensure_target() {
    [ -f "$BUILD_DIR/build.ninja" ] || create_build_dir
}

run_tests() {
    local TEST_NAME="$1"
    local CTEST_ARGS=("--preset" "default" "--output-on-failure" "--test-dir" "$BUILD_DIR")
    if [ -n "$TEST_NAME" ]; then
        if [ "$TEST_NAME" = "WPT" ]; then
            CTEST_ARGS+=("-C" "Integration")
        fi
        CTEST_ARGS+=("-R" "$TEST_NAME")
    fi
    ctest "${CTEST_ARGS[@]}"
}

build_target() {
    # Get either the environment MAKEJOBS or all processors via CMake
    [ -z "$MAKEJOBS" ] && MAKEJOBS=$(cmake -P "$LADYBIRD_SOURCE_DIR/Meta/CMake/processor-count.cmake")

    # With zero args, we are doing a standard "build"
    # With multiple args, we are doing an install/run
    if [ $# -eq 0 ]; then
        CMAKE_BUILD_PARALLEL_LEVEL="$MAKEJOBS" cmake --build "$BUILD_DIR"
    else
        ninja -j "$MAKEJOBS" -C "$BUILD_DIR" -- "$@"
    fi
}

delete_target() {
    [ ! -d "$BUILD_DIR" ] || rm -rf "$BUILD_DIR"
}

build_cmake() {
    echo "CMake version too old: build_cmake"
    ( cd "$LADYBIRD_SOURCE_DIR/Toolchain" && ./BuildCMake.sh )
}

build_vcpkg() {
    echo "Building vcpkg"
    ( cd "$LADYBIRD_SOURCE_DIR/Toolchain" && ./BuildVcpkg.sh )
}

ensure_toolchain() {
    if [ "$(cmake -P "$LADYBIRD_SOURCE_DIR"/Meta/CMake/cmake-version.cmake)" -ne 1 ]; then
        build_cmake
    fi

    # FIXME: Add a version check if needed.
    if [ ! -x "${LADYBIRD_SOURCE_DIR}/Toolchain/Local/vcpkg/bin/vcpkg" ]; then
        build_vcpkg
    fi
}

run_gdb() {
    local GDB_ARGS=()
    local PASS_ARG_TO_GDB=""
    local LAGOM_EXECUTABLE=""
    for arg in "${CMD_ARGS[@]}"; do
        if [ "$PASS_ARG_TO_GDB" != "" ]; then
            GDB_ARGS+=( "$PASS_ARG_TO_GDB" "$arg" )

            PASS_ARG_TO_GDB=""
        elif [ "$arg" = "-ex" ]; then
            PASS_ARG_TO_GDB="$arg"
        elif [[ "$arg" =~ ^-.*$ ]]; then
            die "Don't know how to handle argument: $arg"
        else
            if [ "$LAGOM_EXECUTABLE" != "" ]; then
                die "Lagom executable can't be specified more than once"
            fi
            LAGOM_EXECUTABLE="$arg"
        fi
    done
    if [ "$PASS_ARG_TO_GDB" != "" ]; then
        GDB_ARGS+=( "$PASS_ARG_TO_GDB" )
    fi
    gdb "$BUILD_DIR/bin/$LAGOM_EXECUTABLE" "${GDB_ARGS[@]}"
}

build_and_run_lagom_target() {
    local lagom_target="${CMD_ARGS[0]}"
    local lagom_args=("${CMD_ARGS[@]:1}")

    if [ -z "$lagom_target" ]; then
        lagom_target="ladybird"
    fi

    build_target "${lagom_target}"

    if [ "$lagom_target" = "ladybird" ] && [ "$(uname -s)" = "Darwin" ]; then
        open --wait-apps --stdout "$(tty)" --stderr "$(tty)" "$BUILD_DIR/bin/Ladybird.app" --args "${lagom_args[@]}"
    else
        local lagom_bin="$lagom_target"
        if [ "$lagom_bin" = "ladybird" ]; then
            lagom_bin="Ladybird"
        fi
        "$BUILD_DIR/bin/$lagom_bin" "${lagom_args[@]}"
    fi
}

if [[ "$CMD" =~ ^(build|install|run|gdb|test|rebuild|recreate|addr2line)$ ]]; then
    cmd_with_target
    [[ "$CMD" != "recreate" && "$CMD" != "rebuild" ]] || delete_target
    ensure_toolchain
    ensure_target
    case "$CMD" in
        build)
            build_target "${CMD_ARGS[@]}"
            ;;
        install)
            build_target
            build_target install
            ;;
        run)
            build_and_run_lagom_target
            ;;
        gdb)
          [ $# -ge 1 ] || usage
          build_target "${CMD_ARGS[@]}"
          run_gdb "${CMD_ARGS[@]}"
          ;;
        test)
            build_target
            run_tests "${CMD_ARGS[0]}"
            ;;
        rebuild)
            build_target "${CMD_ARGS[@]}"
            ;;
        recreate)
            ;;
        addr2line)
            build_target
            [ $# -ge 2 ] || usage
            BINARY_FILE="$1"; shift
            BINARY_FILE_PATH="$BUILD_DIR/$BINARY_FILE"
            command -v addr2line >/dev/null 2>&1 || die "Please install addr2line!"
            ADDR2LINE=addr2line
            if [ -x "$BINARY_FILE_PATH" ]; then
                "$ADDR2LINE" -e "$BINARY_FILE_PATH" "$@"
            else
                find "$BUILD_DIR" -name "$BINARY_FILE" -executable -type f -exec "$ADDR2LINE" -e {} "$@" \;
            fi
            ;;
        *)
            build_target "$CMD" "${CMD_ARGS[@]}"
            ;;
    esac
elif [ "$CMD" = "delete" ]; then
    cmd_with_target
    delete_target
elif [ "$CMD" = "vcpkg" ]; then
    cmd_with_target
    ensure_toolchain
else
    >&2 echo "Unknown command: $CMD"
    usage
fi
