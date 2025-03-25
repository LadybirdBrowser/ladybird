#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/shell_include.sh"

ensure_ladybird_source_dir

WPT_SOURCE_DIR=${WPT_SOURCE_DIR:-"${LADYBIRD_SOURCE_DIR}/Tests/LibWeb/WPT/wpt"}
WPT_REPOSITORY_URL=${WPT_REPOSITORY_URL:-"https://github.com/web-platform-tests/wpt.git"}

BUILD_PRESET=${BUILD_PRESET:-default}

BUILD_DIR=$(get_build_dir "$BUILD_PRESET")

default_binary_path() {
    if [ "$(uname -s)" = "Darwin" ]; then
        echo "${BUILD_DIR}/bin/Ladybird.app/Contents/MacOS"
    else
        echo "${BUILD_DIR}/bin"
    fi
}

ladybird_git_hash() {
    pushd "${LADYBIRD_SOURCE_DIR}" > /dev/null
        git rev-parse --short HEAD
    popd > /dev/null
}

LADYBIRD_BINARY=${LADYBIRD_BINARY:-"$(default_binary_path)/Ladybird"}
WEBDRIVER_BINARY=${WEBDRIVER_BINARY:-"$(default_binary_path)/WebDriver"}
HEADLESS_BROWSER_BINARY=${HEADLESS_BROWSER_BINARY:-"$(default_binary_path)/headless-browser"}
WPT_PROCESSES=${WPT_PROCESSES:-$(get_number_of_processing_units)}
WPT_CERTIFICATES=(
  "tools/certs/cacert.pem"
  "${BUILD_DIR}/Lagom/cacert.pem"
)
WPT_ARGS=( "--webdriver-binary=${WEBDRIVER_BINARY}"
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
      import:     $NAME import [PATHS...]
                      Fetch the given test file(s) from https://wpt.live/ and create an in-tree test and expectation files.
      list-tests: $NAME list-tests [PATHS..]
                      List the tests in the given PATHS.

    Examples:
      $NAME update
          Updates the Web Platform Tests repository.
      $NAME run
          Run all of the Web Platform Tests.
      $NAME run --log expectations.log css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories and save the output to expectations.log.
      $NAME run --log-wptreport expectations.json --log-wptscreenshot expectations.db css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories; save the output in wptreport format to expectations.json and save screenshots to expectations.db.
      $NAME run --debug-process WebContent http://wpt.live/dom/historical.html
          Run the 'dom/historical.html' test, attaching the debugger to the WebContent process when the browser is launched.
      $NAME compare expectations.log
          Run all of the Web Platform Tests comparing the results to the expectations in before.log.
      $NAME compare --log results.log expectations.log css/CSS2
          Run the Web Platform Tests in the 'css/CSS2' directory, comparing the results to the expectations in expectations.log; output the results to results.log.
      $NAME import html/dom/aria-attribute-reflection.html
          Import the test from https://wpt.live/html/dom/aria-attribute-reflection.html into the Ladybird test suite.
      $NAME list-tests css/CSS2 dom
          Show a list of all tests in the 'css/CSS2' and 'dom' directories.
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

set_logging_flags()
{
    [ -n "${1}" ] || usage;
    [ -n "${2}" ] || usage;

    log_type="${1}"
    log_name="$(absolutize_path "${2}")"

    WPT_ARGS+=( "${log_type}=${log_name}" )
}

headless=1
ARG=$1
while [[ "$ARG" =~ ^(--show-window|--debug-process|(--log(-(raw|unittest|xunit|html|mach|tbpl|grouped|chromium|wptreport|wptscreenshot))?))$ ]]; do
    case "$ARG" in
        --show-window)
            headless=0
            ;;
        --debug-process)
            process_name="${2}"
            shift
            WPT_ARGS+=( "--webdriver-arg=--debug-process=${process_name}" )
            ;;
        --log)
            set_logging_flags "--log-raw" "${2}"
            shift
            ;;
        *)
            set_logging_flags "${ARG}" "${2}"
            shift
            ;;
    esac

    shift
    ARG=$1
done

if [ $headless -eq 1 ]; then
    WPT_ARGS+=( "--binary=${HEADLESS_BROWSER_BINARY}" )
    WPT_ARGS+=( "--webdriver-arg=--headless" )
else
    WPT_ARGS+=( "--binary=${LADYBIRD_BINARY}" )
fi

exit_if_running_as_root "Do not run WPT.sh as root"

construct_test_list() {
  TEST_LIST=( "$@" )

  for i in "${!TEST_LIST[@]}"; do
      item="${TEST_LIST[i]}"
      item="${item#"$WPT_SOURCE_DIR"/}"
      item="${item#*Tests/LibWeb/WPT/wpt/}"
      item="${item#http://wpt.live/}"
      item="${item#https://wpt.live/}"
      TEST_LIST[i]="$item"
  done
}

ensure_wpt_repository() {
    mkdir -p "${WPT_SOURCE_DIR}"
    pushd "${WPT_SOURCE_DIR}" > /dev/null
        if [ ! -d .git ]; then
            git clone --depth 1 "${WPT_REPOSITORY_URL}" "${WPT_SOURCE_DIR}"
        fi

        # Update hosts file if needed
        if [ "$(comm -13 <(sort -u /etc/hosts) <(./wpt make-hosts-file | sort -u) | wc -l)" -gt 0 ]; then
            echo "Enter superuser password to append wpt hosts to /etc/hosts"
            ./wpt make-hosts-file | sudo tee -a /etc/hosts
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
    # Ensure open files limit is at least 1024, so the WPT runner does not run out of descriptors
    if [ "$(ulimit -n)" -lt 1024 ]; then
        ulimit -S -n 1024
    fi

    pushd "${WPT_SOURCE_DIR}" > /dev/null
        for certificate_path in "${WPT_CERTIFICATES[@]}"; do
            if [ ! -f "${certificate_path}" ]; then
                echo "Certificate not found: \"${certificate_path}\""
                exit 1
            fi
            WPT_ARGS+=( "--webdriver-arg=--certificate=${certificate_path}" )
        done
        construct_test_list "${@}"
        echo LADYBIRD_GIT_VERSION="$(ladybird_git_hash)" ./wpt run "${WPT_ARGS[@]}" ladybird "${TEST_LIST[@]}"
        LADYBIRD_GIT_VERSION="$(ladybird_git_hash)" ./wpt run "${WPT_ARGS[@]}" ladybird "${TEST_LIST[@]}"
    popd > /dev/null
}

run_wpt() {
    ensure_wpt_repository
    build_ladybird_and_webdriver
    execute_wpt "${@}"
}

serve_wpt()
{
    ensure_wpt_repository

    pushd "${WPT_SOURCE_DIR}" > /dev/null
        ./wpt serve
    popd > /dev/null
}

list_tests_wpt()
{
    ensure_wpt_repository

    construct_test_list "${@}"

    pushd "${WPT_SOURCE_DIR}" > /dev/null
        ./wpt run --list-tests ladybird "${TEST_LIST[@]}"
    popd > /dev/null
}

import_wpt()
{
    for i in "${!INPUT_PATHS[@]}"; do
        item="${INPUT_PATHS[i]}"
        item="${item#http://wpt.live/}"
        item="${item#https://wpt.live/}"
        INPUT_PATHS[i]="$item"
    done

    RAW_TESTS=()
    while IFS= read -r test_file; do
        RAW_TESTS+=("${test_file%%\?*}")
    done < <(
        "${ARG0}" list-tests "${INPUT_PATHS[@]}"
    )
    if [ "${#RAW_TESTS[@]}" -eq 0 ]; then
        echo "No tests found for the given paths"
        exit 1
    fi

    TESTS=()
    while IFS= read -r test_file; do
        TESTS+=("$test_file")
    done < <(printf "%s\n" "${RAW_TESTS[@]}" | sort -u)

    pushd "${LADYBIRD_SOURCE_DIR}" > /dev/null
        ./Meta/ladybird.sh build headless-browser
        set +e
        for path in "${TESTS[@]}"; do
            echo "Importing test from ${path}"
            if ! ./Meta/import-wpt-test.py https://wpt.live/"${path}"; then
                continue
            fi
            "${HEADLESS_BROWSER_BINARY}" --run-tests ./Tests/LibWeb --rebaseline -f "$path"
        done
        set -e
    popd > /dev/null
}

compare_wpt() {
    ensure_wpt_repository
    METADATA_DIR=$(mktemp -d)
    pushd "${WPT_SOURCE_DIR}" > /dev/null
      ./wpt update-expectations --product ladybird --full --metadata="${METADATA_DIR}" "${INPUT_LOG_NAME}"
    popd > /dev/null
    WPT_ARGS+=( "--metadata=${METADATA_DIR}" )
    build_ladybird_and_webdriver
    execute_wpt "${@}"
    rm -rf "${METADATA_DIR}"
}

if [[ "$CMD" =~ ^(update|run|serve|compare|import|list-tests)$ ]]; then
    case "$CMD" in
        update)
            update_wpt
            ;;
        run)
            run_wpt "${@}"
            ;;
        serve)
            serve_wpt
            ;;
        import)
            if [ $# -eq 0 ]; then
                usage
            fi
            INPUT_PATHS=( "$@" )
            import_wpt
            ;;

        compare)
            INPUT_LOG_NAME="$(pwd -P)/$1"
            if [ ! -f "$INPUT_LOG_NAME" ]; then
                echo "Log file not found: \"${INPUT_LOG_NAME}\""
                usage;
            fi
            shift
            compare_wpt "${@}"
            ;;
        list-tests)
            list_tests_wpt "${@}"
            ;;
    esac
else
    >&2 echo "Unknown command: $CMD"
    usage
fi
