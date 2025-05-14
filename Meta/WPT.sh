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

: "${TRY_SHOW_LOGFILES_IN_TMUX:=false}"
: "${SHOW_LOGFILES:=true}"
: "${SHOW_PROGRESS:=true}"
: "${PARALLEL_INSTANCES:=1}"

if "$SHOW_PROGRESS"; then
    SHOW_LOGFILES=true
    TRY_SHOW_LOGFILES_IN_TMUX=false
fi

sudo_and_ask() {
    local prompt
    prompt="$1"; shift
    if [ -z "$prompt" ]; then
        prompt="Running '${*}' as root, please enter password for %p: "
    else
        prompt="$prompt; please enter password for %p: "
    fi

    sudo --prompt="$prompt" "${@}"
}

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

run_dir_path() {
    i="$1"; shift
    local runpath="${BUILD_DIR}/wpt/run.$i"
    echo "$runpath"
}

ensure_run_dir() {
    i="$1"; shift
    local runpath

    runpath="$(run_dir_path "$i")"
    if [ ! -d "$runpath" ]; then
        mkdir -p "$runpath/upper" "$runpath/work" "$runpath/merged"
        # shellcheck disable=SC2140
        sudo_and_ask "Mounting overlayfs on $runpath" mount -t overlay overlay -o lowerdir="${WPT_SOURCE_DIR}",upperdir="$runpath/upper",workdir="$runpath/work" "$runpath/merged"
    fi
    echo "$runpath/merged"
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
    "--webdriver-arg=--force-cpu-painting"
    "--no-pause-after-test"
    "${EXTRA_WPT_ARGS[@]}"
)
WPT_LOG_ARGS=()

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
      clean:      $NAME clean
                      Clean up the extra resources and directories (if any leftover) created by this script.

    Env vars:
      EXTRA_WPT_ARGS:             Extra arguments for the wpt command, placed at the end; array, default empty
      TRY_SHOW_LOGFILES_IN_TMUX:  Whether to show split logs in tmux; true or false, default false
      SHOW_LOGFILES:              Whether to show logs at all; true or false, default true
      SHOW_PROGRESS:              Whether to show the progress of the tests, default true
                                    implies SHOW_LOGFILES=true and TRY_SHOW_LOGFILES_IN_TMUX=false

    Options for this script:
      --show-window
          Disable headless mode
      --debug-process PROC_NAME
          Enable debugging for the PROC_NAME ladybird process
      --parallel-instances N
          Enable running in chunked mode with N parallel instances
              N=0 to auto-enable if possible
              N=1 to disable chunked mode (default)
              N>1 to enable chunked mode with explicit process count
      --log PATH
          Alias for --log-raw PATH
      --log-(raw|unittest|xunit|html|mach|tbpl|grouped|chromium|wptreport|wptscreenshot) PATH
          Enable the given wpt log option with the given PATH


    Examples:
      $NAME update
          Updates the Web Platform Tests repository.
      $NAME run
          Run all of the Web Platform Tests.
      $NAME run --log expectations.log css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories and save the output to expectations.log.
      $NAME run --log-wptreport expectations.json --log-wptscreenshot expectations.db css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories; save the output in wptreport format to expectations.json and save screenshots to expectations.db.
      $NAME run --parallel-instances 0 --log-wptreport expectations.json --log-wptscreenshot expectations.db css dom
          Run the Web Platform Tests in the 'css' and 'dom' directories in chunked mode; save the output in wptreport format to expectations.json and save screenshots to expectations.db.
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
    log_name="${2}"

    WPT_LOG_ARGS+=("${log_type}" "${log_name}")
}

headless=1
ARG=$1
while [[ "$ARG" =~ ^(--show-window|--debug-process|--parallel-instances|(--log(-(raw|unittest|xunit|html|mach|tbpl|grouped|chromium|wptreport|wptscreenshot))?))$ ]]; do
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
        --parallel-instances)
            PARALLEL_INSTANCES="${2}"
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
            ./wpt make-hosts-file | sudo_and_ask "Appending wpt hosts to /etc/hosts" tee -a /etc/hosts
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

cleanup_run_infra() {
    readarray -t pids < <(jobs -p)
    for pid in "${pids[@]}"; do
        if ps -p "$pid" > /dev/null; then
            echo "Killing background process $pid"
            kill -HUP "$pid" 2>/dev/null || true
        fi
    done

    readarray -t NSS < <(ip netns list 2>/dev/null | grep -E '^wptns[0-9]+' | awk '{print $1}')
    if [ "${#NSS}" = 0 ]; then
        return
    fi
    echo "Cleaning up namespaces: ${NSS[*]}"
    for i in "${!NSS[@]}"; do
        ns="${NSS[$i]}"

        echo "Cleaning up namespace: $ns"

        # Delete namespace
        if sudo_and_ask "" ip netns list | grep -qw "$ns"; then
            sudo_and_ask "Removing netns $ns" ip netns delete "$ns" || echo "  failed to delete netns $ns"
        fi

        # Remove hosts override
        sudo_and_ask "Removing support files for netns $ns" rm -rf "/etc/netns/$ns" || echo "  failed to delete /etc/netns/$ns"
    done
}

cleanup_run_dirs() {
    readarray -t dirs < <(ls "${BUILD_DIR}/wpt" 2>/dev/null)
    if [ "${#dirs}" = 0 ]; then
        return
    fi

    echo "Cleaning run dirs: ${dirs[*]}"
    for dir in "${dirs[@]}"; do
        mount_path="${BUILD_DIR}/wpt/$dir/merged"
        for _ in $(seq 1 5); do
            readarray -t pids_in_use < <(sudo_and_ask "" lsof "$mount_path" 2>/dev/null | cut -f2 -d' ')
            [ "${#pids_in_use[@]}" = 0 ] && break
                echo Trying to kill procs: "${pids_in_use[@]}"
                kill -INT "${pids_in_use[@]}" 2>/dev/null || true
            done
            sudo_and_ask "" umount "$mount_path" || true
        done
        rm -fr "${BUILD_DIR}/wpt"
    }
cleanup_merge_dirs_and_infra() {
    cleanup_run_dirs
    cleanup_run_infra
}
trap cleanup_merge_dirs_and_infra EXIT INT TERM

make_instances() {
    if [ "${PARALLEL_INSTANCES}" = 1 ]; then
        echo 1
        return
    fi

    if ! command -v ip &>/dev/null; then
        echo "the 'ip' command is required to run WPT in chunked mode" >&2
        echo 1
        return
    fi

    if ! sudo_and_ask "Making test netns 'testns'" ip netns add testns; then
        echo "ip netns failed, chunked mode not available" >&2
        echo 1
        return
    fi
    sudo_and_ask "Cleaning up test netns 'testns'" ip netns delete testns

    explicit_count="${PARALLEL_INSTANCES}"

    local total_cores count ns
    total_cores=$(nproc)
    count=$(( total_cores / 2 ))
    (( count < 1 )) && count=1

    if (( explicit_count > 0 )); then
        count="$explicit_count"
    fi

    for i in $(seq 0 $((count - 1))); do
        ns="wptns$i"

        # Create namespace
        sudo_and_ask "" ip netns add "$ns"
        sudo_and_ask "" ip netns exec "$ns" ip link set lo up

        # Setup DNS and hosts (we've messed with it before getting here)
        sudo_and_ask "" mkdir -p "/etc/netns/$ns"
        sudo_and_ask "" cp /etc/hosts "/etc/netns/$ns/hosts"
    done

    echo "$count"
}

instance_run() {
    local idx="$1"; shift
    local rundir="$1"; shift
    local ns="wptns$idx"
    if sudo_and_ask "" ip netns list | grep -qw "$ns"; then
        (
            cd "$rundir"
            sudo_and_ask "" ip netns exec "$ns" sudo -u "$USER" -- env "PATH=$PATH" "$@"
        )
    else
        echo "  netns $ns not found, running in the host namespace"
        (
            cd "${WPT_SOURCE_DIR}"
            "$@"
        )
    fi
}

show_files() {
    if ! "$SHOW_LOGFILES"; then
        return
    fi
    local files=("$@")
    if ! command -v tmux &>/dev/null || ! "$TRY_SHOW_LOGFILES_IN_TMUX"; then
        if "$TRY_SHOW_LOGFILES_IN_TMUX"; then
            echo "tmux is not available, falling back to tail"
        fi
        if "$SHOW_PROGRESS"; then
            bash "${DIR}/watch_wpt_progress.sh" "${files[@]}" &
        else
            tail -f "${files[@]}" &
        fi
        PID=$!
        for pid in $(jobs -p | grep -v $PID); do
            # shellcheck disable=SC2009
            ps | grep -q "$pid" && wait "$pid"
        done
        kill -HUP $PID
    else
        tmux new-session -d

        tmux send-keys "less +F ${files[0]}" C-m

        for ((i = 1; i < ${#files[@]}; i++)); do
            if (( i % 2 == 1 )); then
                tmux split-window -h "less +F ${files[i]}"
            else
                tmux split-window -v "less +F ${files[i]}"
            fi
            tmux select-layout tiled > /dev/null
        done

        tmux attach
    fi
}

copy_results_to() {
    local target="$1"; shift
    local runcount="$1"; shift
    mkdir -p "$target"
    for i in $(seq 0 $((runcount - 1))); do
        for f in "$(run_dir_path "$i")/upper"/*; do
            cp -r "$f" "$target/$(basename "$f").run_$i"
        done
    done
}

show_summary() {
    local logs=("$@")
    local total_tests=0
    local expected=0
    local skipped=0
    local errored=0
    local subtest_issues=0
    local max_time=0

    for out_file in "${logs[@]}"; do
        # wpt puts random garbage in the output, strip those (as they're all nonprint)
        mapfile -t lines < <(grep -A4 -aE 'Ran [0-9]+ tests finished in' "$out_file" \
                             | iconv -f utf-8 -t ascii//TRANSLIT 2>/dev/null \
                             | sed 's/[^[:print:]]//g')

        for line in "${lines[@]}"; do
            if [[ $line =~ Ran[[:space:]]([0-9]+)[[:space:]]tests[[:space:]]finished[[:space:]]in[[:space:]]([0-9.]+) ]]; then
                (( total_tests += BASH_REMATCH[1] ))
                time=${BASH_REMATCH[2]}
                [[ $(echo "$time > $max_time" | bc -l) == 1 ]] && max_time=$time
            elif [[ $line =~ ([0-9]+)[[:space:]]ran[[:space:]]as[[:space:]]expected ]]; then
                (( expected += BASH_REMATCH[1] ))
            elif [[ $line =~ ([0-9]+)[[:space:]]tests[[:space:]]skipped ]]; then
                (( skipped += BASH_REMATCH[1] ))
            elif [[ $line =~ ([0-9]+)[[:space:]]tests[[:space:]](crashed|timed[[:space:]]out|had[[:space:]]errors)[[:space:]]unexpectedly ]]; then
                (( errored += BASH_REMATCH[1] ))
            elif [[ $line =~ ([0-9]+)[[:space:]]tests[[:space:]]had[[:space:]]unexpected[[:space:]]subtest[[:space:]]results ]]; then
                (( subtest_issues += BASH_REMATCH[1] ))
            fi
        done
    done

    echo "Total tests run: $total_tests"
    echo "Ran as expected: $expected"
    echo "Skipped: $skipped"
    echo "Errored unexpectedly: $errored"
    echo "Unexpected subtest results: $subtest_issues"
    echo "Longest run time: ${max_time}s"
}

# run_wpt_chunked <#processes> <wpt args...>
run_wpt_chunked() {
    local procs concurrency
    procs="$1"; shift

    # Ensure open files limit is at least 1024, so the WPT runner does not run out of descriptors
    if [ "$(ulimit -n)" -lt $((1024 * procs)) ]; then
        ulimit -S -n $((1024 * procs))
    fi

    if [ "$procs" -le 1 ]; then
        command=(./wpt run -f --browser-version="1.0-$(ladybird_git_hash)" --processes="${WPT_PROCESSES}" "$@")
        echo "${command[@]}"
        "${command[@]}"
        return
    fi

    concurrency=$(( $(nproc) * 2 / procs ))

    echo "Preparing the venv setup..."
    base_venv="${BUILD_DIR}/wpt-prep/_venv"
    ./wpt --venv "$base_venv" run "${WPT_ARGS[@]}" ladybird THIS_TEST_CANNOT_POSSIBLY_EXIST || true

    echo "Launching $procs chunked instances (concurrency=$concurrency each)"
    local logs=()

    for i in $(seq 0 $((procs - 1))); do
        local rundir runpath logpath
        rundir="$(ensure_run_dir "$i")"
        runpath="$(run_dir_path "$i")"
        logpath="$runpath/upper/run.logs"
        echo "rundir at $rundir, logs in $logpath"
        touch "$logpath"
        logs+=("$logpath")

        cp -r "$base_venv" "${runpath}/_venv"

        command=(./wpt --venv "${runpath}/_venv" \
            run \
            --this-chunk="$((i + 1))" \
            --total-chunks="$procs" \
            --chunk-type=hash \
            -f \
            --browser-version="1.0-$(ladybird_git_hash)"
            --processes="$concurrency" \
            "$@")
        echo "[INSTANCE $i / ns wptns$i] ${command[*]}"
        instance_run "$i" "$rundir" script -q "$logpath" -c "$(printf "%q " "${command[@]}")" &>/dev/null &
    done

    show_files "${logs[@]}"
    wait

    copy_results_to "${BUILD_DIR}/wpt-run-$(date +%s)" "$procs"
    show_summary "${logs[@]}"
}

absolutize_log_args() {
    for ((i=0; i<${#WPT_LOG_ARGS[@]}; i += 2)); do
        WPT_LOG_ARGS[i + 1]="$(absolutize_path "${WPT_LOG_ARGS[i + 1]}")"
    done
}

execute_wpt() {
    local procs

    procs=$(make_instances)
    if [[ "$procs" -le 1 ]]; then
        absolutize_log_args
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
        run_wpt_chunked "$procs" "${WPT_ARGS[@]}" "${WPT_LOG_ARGS[@]}" ladybird "${TEST_LIST[@]}"
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

if [[ "$CMD" =~ ^(update|clean|run|serve|compare|import|list-tests)$ ]]; then
    case "$CMD" in
        update)
            update_wpt
            ;;
        run)
            run_wpt "${@}"
            ;;
        clean)
            cleanup_run_infra
            cleanup_run_dirs true
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
