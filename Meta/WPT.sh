#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

LADYBIRD_SOURCE_DIR="$(realpath "${DIR}"/..)"
WPT_SOURCE_DIR=${WPT_SOURCE_DIR:-"${LADYBIRD_SOURCE_DIR}/Tests/LibWeb/WPT/wpt"}
WPT_REPOSITORY_URL=${WPT_REPOSITORY_URL:-"https://github.com/web-platform-tests/wpt.git"}

LADYBIRD_BINARY=${LADYBIRD_BINARY:-"${LADYBIRD_SOURCE_DIR}/Build/ladybird/bin/Ladybird"}
WEBDRIVER_BINARY=${WEBDRIVER_BINARY:-"${LADYBIRD_SOURCE_DIR}/Build/ladybird/bin/WebDriver"}

WPT_PROCESSES=${WPT_PROCESSES:-$(nproc)}
WPT_CERTIFICATES=(
  "tools/certs/cacert.pem"
  "${LADYBIRD_SOURCE_DIR}/Build/ladybird/Lagom/cacert.pem"
)
WPT_ARGS=( "--binary=${LADYBIRD_BINARY}"
           "--webdriver-binary=${WEBDRIVER_BINARY}"
           "--install-webdriver"
            "--processes=${WPT_PROCESSES}"
           "--webdriver-arg=--force-cpu-painting"
            "--no-pause-after-test"
           "-f"
           "${EXTRA_WPT_ARGS[@]}"
)

ARG0=$0
print_help() {
  NAME=$(basename "$ARG0")
  cat <<EOF
  Usage: $NAME COMMAND [OPTIONS..] [TESTS...]
    Supported COMMANDs:
      update:     Update the Web Platform Tests repository.
      run:        $NAME run [OPTIONS...] [TESTS...]
                      Run the Web Platform Tests.
      compare:    $NAME compare [OPTIONS...] LOG_FILE [TESTS...]
                      Run the Web Platform Tests comparing the results to the expectations in LOG_FILE.

    Examples:
      $NAME update
          Updates the Web Platform Tests repository.
      $NAME run
          Run all of the Web Platform Tests.
      $NAME run --log expectations.log css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories and save the output to expectations.log.
      $NAME compare expectations.log
          Run all of the Web Platform Tests comparing the results to the expectations in before.log.
      $NAME compare --log results.log expectations.log css/CSS2
          Run the Web Platform Tests in the 'css/CSS2' directory, comparing the results to the expectations in expectations.log; output the results to results.log.
EOF
}

usage() {
    >&2 print_help
    exit 1
}

CMD=$1
[ -n "$CMD" ] || usage
shift
if [ "$CMD" = "--help" ] || [ "$CMD" = "help" ]; then
    print_help
    exit 0
fi

ARG=$1
if [ "$ARG" = "--log" ]; then
    shift
    LOG_NAME="$(pwd -P)/$1"
    [ -n "$LOG_NAME" ] || usage;
    shift
    WPT_ARGS+=( "--log-raw=${LOG_NAME}" )
fi
TEST_LIST=( "$@" )

# shellcheck source=/dev/null
. "${DIR}/shell_include.sh"

exit_if_running_as_root "Do not run WPT.sh as root"

ensure_wpt_repository() {
    mkdir -p "${WPT_SOURCE_DIR}"
    pushd "${WPT_SOURCE_DIR}" > /dev/null
        if [ ! -d .git ]; then
            git clone --depth 1 "${WPT_REPOSITORY_URL}" "${WPT_SOURCE_DIR}"
        fi
    popd > /dev/null
}

build_ladybird_and_webdriver() {
    "${DIR}"/ladybird.sh build WebDriver
}

update_wpt() {
    ensure_wpt_repository
    pushd "${WPT_SOURCE_DIR}" > /dev/null
        git pull
    popd > /dev/null
}

execute_wpt() {
    pushd "${WPT_SOURCE_DIR}" > /dev/null
        for certificate_path in "${WPT_CERTIFICATES[@]}"; do
            if [ ! -f "${certificate_path}" ]; then
                echo "Certificate not found: \"${certificate_path}\""
                exit 1
            fi
            WPT_ARGS+=( "--webdriver-arg=--certificate=${certificate_path}" )
        done
        QT_QPA_PLATFORM="minimal" ./wpt run "${WPT_ARGS[@]}" ladybird "${TEST_LIST[@]}"
    popd > /dev/null
}

run_wpt() {
    ensure_wpt_repository
    build_ladybird_and_webdriver
    execute_wpt
}

compare_wpt() {
    ensure_wpt_repository
    METADATA_DIR=$(mktemp -d)
    pushd "${WPT_SOURCE_DIR}" > /dev/null
      ./wpt update-expectations --product ladybird --full --metadata="${METADATA_DIR}" "${INPUT_LOG_NAME}"
    popd > /dev/null
    WPT_ARGS+=( "--metadata=${METADATA_DIR}" )
    build_ladybird_and_webdriver
    execute_wpt
    rm -rf "${METADATA_DIR}"
}

if [[ "$CMD" =~ ^(update|run|compare)$ ]]; then
    case "$CMD" in
        update)
            update_wpt
            ;;
        run)
            run_wpt
            ;;
        compare)
            INPUT_LOG_NAME="$(pwd -P)/$1"
            if [ ! -f "$INPUT_LOG_NAME" ]; then
                echo "Log file not found: \"${INPUT_LOG_NAME}\""
                usage;
            fi
            shift
            compare_wpt
            ;;
    esac
else
    >&2 echo "Unknown command: $CMD"
    usage
fi
