#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/Utils/shell_include.sh"

ensure_ladybird_source_dir

WPT_SOURCE_DIR=${WPT_SOURCE_DIR:-"${LADYBIRD_SOURCE_DIR}/Tests/LibWeb/WPT/wpt"}
WPT_REPOSITORY_URL=${WPT_REPOSITORY_URL:-"https://github.com/web-platform-tests/wpt.git"}

BUILD_PRESET=${BUILD_PRESET:-Release}

BUILD_DIR=$(get_build_dir "$BUILD_PRESET")

TMPDIR=${TMPDIR:-/tmp}

: "${PARALLEL_INSTANCES:=1}"
: "${BUILD_LADYBIRD:=true}"
: "${WPT_DURATIONS_FILE:=}"
: "${WPT_PROCESSES_PER_INSTANCE:=4}"
: "${WPT_SCHEDULER_ARGS:=}"

sudo_and_ask() {
    local prompt
    prompt="$1"; shift
    # Running as root is only possible when the CI environment variable is set to true.
    if [ "$(id -u)" -eq 0 ]; then
        "${@}"
        return
    fi
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

# Chunked instances share $HOME, so each wptrunner installing Ahem into ~/.fonts makes them race: the first
# instance to finish removes the font while the others are still running. Install it once up front instead, and
# let the instances run with --no-install-fonts.
ensure_ahem_font() {
    local font_dir="${HOME}/.fonts"
    if [ -f "${font_dir}/Ahem.ttf" ]; then
        return
    fi

    echo "Installing Ahem into ${font_dir}"
    mkdir -p "${font_dir}"
    cp "${WPT_SOURCE_DIR}/fonts/Ahem.ttf" "${font_dir}"
    fc-cache
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
TEST_WEB_BINARY=${TEST_WEB_BINARY:-"${BUILD_DIR}/bin/test-web"}
WPT_PROCESSES=${WPT_PROCESSES:-$(get_number_of_processing_units)}
WPT_CERTIFICATES=(
    "tools/certs/cacert.pem"
)
WPT_TEST_TYPE_ARGS=(
    "--test-types"
    "testharness"
    "reftest"
    "wdspec"
    "crashtest"
    "test262"
)
WPT_ARGS=(
    "--binary=${LADYBIRD_BINARY}"
    "--webdriver-binary=${WEBDRIVER_BINARY}"
    "--install-webdriver"
    "--webdriver-arg=--force-cpu-painting"
    "--webdriver-arg=--default-time-zone=UTC"
    "--webdriver-arg=--expose-experimental-interfaces"
    "--no-pause-after-test"
    "--no-restart-on-new-group"
    # Every failure is "unexpected" since there's no expectation metadata, no need to restart for that.
    "--no-restart-on-unexpected"
    "${EXTRA_WPT_ARGS[@]}"
)
IMPORT_ARGS=()
WPT_LOG_ARGS=()
TESTS_FROM_FILE=()

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
                      Clean up the extra resources and directories (if any leftover) created by this script (Linux only).
      bisect:     $NAME bisect BAD_COMMIT GOOD_COMMIT [TESTS...]
                      Find the first commit where a given set of tests produce unexpected results.

    Env vars:
      BUILD_LADYBIRD:             Whether to build Ladybird and WebDriver before running tests; true or false, default true
      EXTRA_WPT_ARGS:             Extra arguments for the wpt command, placed at the end; array, default empty
      WPT_DURATIONS_FILE:         Per-test durations used to order and size batches in parallel mode; default the
                                    newest durations.json in \$BUILD_DIR/wpt-run-*.
      WPT_PROCESSES_PER_INSTANCE: Test runners per instance in parallel mode; default 4
      WPT_SCHEDULER_ARGS:         Extra arguments for Meta/wpt_scheduler.py in parallel mode, e.g.
                                    "--max-cpu-pressure 10"; default empty

    Options for this script:
      --show-window
          Disable headless mode
      --debug-process PROC_NAME
          Enable debugging for the PROC_NAME ladybird process
      --parallel-instances N
          Run tests in batches across up to N instances, each in its own network namespace. More instances are
          started while the machine has spare CPU, and fewer when it doesn't.
              N=0 to auto-enable if possible, with up to one instance per processor
              N=1 to disable parallel mode (default)
              N>1 to allow at most N instances
      --log PATH
          Alias for --log-raw PATH
      --test-list PATH
          Read tests to run from the given file, one test path per line
              Empty lines and lines starting with '#' are ignored
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
      $NAME import --force html/dom/aria-attribute-reflection.html
          Import the test from https://wpt.live/html/dom/aria-attribute-reflection.html into the Ladybird test suite, redownloading any files that already exist.
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
while [[ "$ARG" =~ ^(--show-window|--debug-process|--parallel-instances|--test-list|(--log(-(raw|unittest|xunit|html|mach|tbpl|grouped|chromium|wptreport|wptscreenshot))?))$ ]]; do
    case "$ARG" in
        --show-window)
            headless=0
            ;;
        --test-list)
            [ -f "${2}" ] || die "No such test list file: '${2}'"
            while IFS= read -r test_path; do
                TESTS_FROM_FILE+=("$test_path")
            done < <(grep -v -e '^#' -e '^[[:space:]]*$' "${2}")
            shift
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
    WPT_ARGS+=( "--webdriver-arg=--headless" )
fi

if [ "${CI:-false}" != "true" ]; then
    exit_if_running_as_root "Do not run WPT.sh as root"
fi

construct_test_list() {
    TEST_LIST=( "$@" "${TESTS_FROM_FILE[@]}" )

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
    popd > /dev/null
}

build_ladybird_and_webdriver() {
    if ! "$BUILD_LADYBIRD"; then
        return
    fi
    "${LADYBIRD_SOURCE_DIR}"/Meta/ladybird.py build WebDriver
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

clear_processes_using_path() {
    local path="$1"
    local signal="$2"
    local action="$3"
    local timeout_seconds="$4"
    local deadline=$((SECONDS + timeout_seconds))
    local pids_in_use

    readarray -t pids_in_use < <(sudo_and_ask "" lsof -t "$path" 2>/dev/null | sort -nu)
    if [ "${#pids_in_use[@]}" = 0 ]; then
        return 0
    fi

    echo "Trying to $action procs holding $path: ${pids_in_use[*]}"
    kill "-$signal" "${pids_in_use[@]}" 2>/dev/null || true
    while true; do
        readarray -t pids_in_use < <(sudo_and_ask "" lsof -t "$path" 2>/dev/null | sort -nu)
        if [ "${#pids_in_use[@]}" = 0 ]; then
            return 0
        fi

        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "Timed out waiting for procs holding $path to exit: ${pids_in_use[*]}"
            return 1
        fi

        sleep 0.1
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
        if ! mountpoint -q "$mount_path"; then
            continue
        fi

        clear_processes_using_path "$mount_path" TERM terminate 5 \
            || clear_processes_using_path "$mount_path" KILL kill 5 \
            || true

        sudo_and_ask "" umount "$mount_path" || echo "Failed to unmount $mount_path"
    done
    # Overlayfs can leave root-owned internal state in the workdir because we mount it via sudo.
    sudo_and_ask "" rm -fr "${BUILD_DIR}/wpt"
}

WPT_PROFILES_PARENT="${BUILD_DIR}/wpt-profiles"
WPT_PROFILES_ROOT="${WPT_PROFILES_PARENT}/run-$$"

# Each WebDriver instance creates its browser profile under this run's profile root. The root is
# removed when this script exits; roots leaked by previous unclean exits are swept before running.
ensure_profiles_root() {
    local stale pid
    for stale in "${WPT_PROFILES_PARENT}"/run-*; do
        [ -d "$stale" ] || continue
        pid="${stale##*/run-}"
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -rf "$stale"
        fi
    done
    mkdir -p "${WPT_PROFILES_ROOT}"
    WPT_ARGS+=( "--webdriver-arg=--profiles-directory=${WPT_PROFILES_ROOT}" )
}

cleanup_profiles_root() {
    rm -rf "${WPT_PROFILES_ROOT}"
}

SCHEDULER_PID=""

stop_scheduler() {
    if [ -z "$SCHEDULER_PID" ] || ! kill -0 "$SCHEDULER_PID" 2>/dev/null; then
        return
    fi
    # Let it shut its instances down (and not mistake them dying for crashes) before the rug is pulled out.
    kill -TERM "$SCHEDULER_PID" 2>/dev/null || true
    local deadline=$((SECONDS + 30))
    while kill -0 "$SCHEDULER_PID" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do
        sleep 0.2
    done
}

cleanup_merge_dirs_and_infra() {
    # Cleanup is only needed on Linux
    if [[ $OSTYPE == 'linux'* ]]; then
        stop_scheduler
        cleanup_run_dirs
        cleanup_run_infra
    fi
}
trap 'cleanup_merge_dirs_and_infra; cleanup_profiles_root' EXIT INT TERM

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
    count=$total_cores
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

run_wpt_single() {
    command=(./wpt run -f --browser-version="1.0-$(ladybird_git_hash)" --processes="${WPT_PROCESSES}" --install-fonts \
        "${WPT_ARGS[@]}" "${WPT_LOG_ARGS[@]}" ladybird "${TEST_LIST[@]}" "${WPT_TEST_TYPE_ARGS[@]}")
    echo "${command[@]}"
    "${command[@]}"
}

# run_wpt_scheduled <#instances>
# Hands the tests out in batches to up to N instances, one per network namespace; see Meta/wpt_scheduler.py.
run_wpt_scheduled() {
    local slots="$1"
    local base_venv="${BUILD_DIR}/wpt-prep/_venv"
    local durations_file="$WPT_DURATIONS_FILE"
    if [ -z "$durations_file" ]; then
        durations_file=$(find "${BUILD_DIR}" -maxdepth 2 -path "${BUILD_DIR}/wpt-run-*/durations.json" -printf '%T@ %p\n' 2>/dev/null \
            | sort -n | tail -n 1 | cut -d' ' -f2-)
    fi
    local run_dir
    run_dir="${BUILD_DIR}/wpt-run-$(date +"%Y%m%d%H%M%S")"
    mkdir -p "$run_dir"

    # Ensure open files limit is at least 1024 per instance, so the WPT runners do not run out of descriptors
    if [ "$(ulimit -n)" -lt $((1024 * slots)) ]; then
        ulimit -S -n $((1024 * slots))
    fi

    ensure_ahem_font

    # This also brings the manifest up to date once, so the instances can skip that.
    echo "Preparing the venv and listing tests..."
    local listing="${run_dir}/list-tests.out"
    if ! ./wpt --venv "$base_venv" run "${WPT_ARGS[@]}" --list-tests ladybird "${TEST_LIST[@]}" "${WPT_TEST_TYPE_ARGS[@]}" > "$listing"; then
        echo "Listing the tests to run failed" >&2
        return 1
    fi
    grep '^/' "$listing" > "${run_dir}/tests.txt" || true
    rm -f "$listing"
    if [ ! -s "${run_dir}/tests.txt" ]; then
        echo "No tests to run" >&2
        return 1
    fi

    local scheduler_args=()
    for i in $(seq 0 $((slots - 1))); do
        scheduler_args+=( "--slot=wptns$i:$(ensure_run_dir "$i")" )
    done
    for ((i = 0; i < ${#WPT_LOG_ARGS[@]}; i += 2)); do
        scheduler_args+=( "--log=${WPT_LOG_ARGS[i]}=${WPT_LOG_ARGS[i + 1]}" )
    done
    if [ -n "$durations_file" ]; then
        echo "Using test durations from ${durations_file}"
        scheduler_args+=( "--durations" "$durations_file" )
    fi
    local extra_scheduler_args=()
    read -ra extra_scheduler_args <<< "$WPT_SCHEDULER_ARGS"
    scheduler_args+=( "${extra_scheduler_args[@]}" )

    local webdriver_wrapper="${run_dir}/webdriver"
    cat > "$webdriver_wrapper" <<EOF2
#!/bin/sh
echo 1000 > /proc/self/oom_score_adj
exec "${WEBDRIVER_BINARY}" "\$@"
EOF2
    chmod +x "$webdriver_wrapper"

    # The scheduler starts its instances with sudo -n (and keeps the credentials fresh), so authenticate once up front.
    if [ "$(id -u)" -ne 0 ]; then
        sudo_and_ask "Authenticating for the parallel run" -v
    fi

    python3 "${DIR}/wpt_scheduler.py" run "${scheduler_args[@]}" \
        --venv "$base_venv" \
        --tests "${run_dir}/tests.txt" \
        --processes "$WPT_PROCESSES_PER_INSTANCE" \
        --out "$run_dir" \
        -- --browser-version="1.0-$(ladybird_git_hash)" "${WPT_ARGS[@]}" --webdriver-binary="$webdriver_wrapper" \
            ladybird "${WPT_TEST_TYPE_ARGS[@]}" &
    SCHEDULER_PID=$!
    wait "$SCHEDULER_PID"
}

absolutize_log_args() {
    for ((i=0; i<${#WPT_LOG_ARGS[@]}; i += 2)); do
        WPT_LOG_ARGS[i + 1]="$(absolutize_path "${WPT_LOG_ARGS[i + 1]}")"
    done
}

update_hosts_file_if_needed() {
    pushd "${WPT_SOURCE_DIR}" > /dev/null
        if [ "$(comm -13 <(sort -u /etc/hosts) <(./wpt make-hosts-file | sort -u) | wc -l)" -gt 0 ]; then
            ./wpt make-hosts-file | sudo_and_ask "Appending wpt hosts to /etc/hosts" tee -a /etc/hosts
        fi
    popd > /dev/null
}

execute_wpt() {
    local procs

    ensure_profiles_root

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
        if [[ "$procs" -le 1 ]]; then
            run_wpt_single
        else
            run_wpt_scheduled "$procs"
        fi
    popd > /dev/null
}

run_wpt() {
    ensure_wpt_repository
    build_ladybird_and_webdriver
    update_hosts_file_if_needed
    execute_wpt "${@}"
}

serve_wpt()
{
    ensure_wpt_repository
    update_hosts_file_if_needed

    pushd "${WPT_SOURCE_DIR}" > /dev/null
        ./wpt serve
    popd > /dev/null
}

cleanup_bisect()
{
    local temp_file_directory="$1"; shift
    if [ -d "${temp_file_directory}" ]; then
        echo "Removing temp file directory: ${temp_file_directory}"
        rm -rf "${temp_file_directory}"
    fi

    git bisect reset
}

bisect_wpt()
{
    if ! git diff-index --quiet HEAD --; then
        echo "You have uncommitted changes, please commit or stash them before bisecting."
        exit 1
    fi

    local bad="$1"; shift
    local good="$1"; shift
    # Commits from before ladybird.py was added don't currently work with this script
    OLDEST_COMMIT_ALLOWED="061a7f766ce"

    if ! git rev-parse --verify "${bad}" >/dev/null 2>&1; then
        echo "Bad commit '${bad}' not found."
        exit 1
    fi

    if ! git rev-parse --verify "${good}" >/dev/null 2>&1; then
        echo "Good commit '${good}' not found."
        exit 1
    fi

    if ! git merge-base --is-ancestor ${OLDEST_COMMIT_ALLOWED} "${good}"; then
        echo "Commits older than ${OLDEST_COMMIT_ALLOWED} aren't allowed (because ladybird.py is required)."
        exit 1
    fi

    if [ "${good}" = "${bad}" ]; then
        echo "The good commit and the bad commit must be different."
        exit 1
    fi

    if ! git merge-base --is-ancestor "${good}" "${bad}"  ; then
        echo "The good commit must be older than the bad commit."
        exit 1
    fi

    ensure_wpt_repository
    construct_test_list "${@}"

    pushd "${LADYBIRD_SOURCE_DIR}" > /dev/null
      local temp_file_directory_base
      temp_file_directory_base="$(mktemp -p "${TMPDIR}" -d "wpt-bisect-helper-XXXXXX")"
      mkdir "${temp_file_directory_base}/Meta"
      local baseline_log_file
      baseline_log_file="$(mktemp -p "${temp_file_directory_base}" -u "XXXXXX.log")"
      local current_branch_or_commit
      current_branch_or_commit="$(git branch --show-current 2> /dev/null)"
      if [ -z "${current_branch_or_commit}" ]; then
          current_branch_or_commit="$(git rev-parse HEAD)"
      fi

      # We create the baseline log file against the bad commit bcause building it may be significantly faster if the
      # good commit is significantly older than the bad commit.
      git checkout "${bad}" 2> /dev/null
      trap 'git checkout "${current_branch_or_commit}" 2> /dev/null' EXIT INT TERM
      echo "Generating baseline log file at \"${baseline_log_file}\""
      $0 run --log "${baseline_log_file}" "${@}" || true
      trap - EXIT INT TERM
      git checkout "${current_branch_or_commit}" 2> /dev/null

      # We need to copy scripts that will run during bisection to ensure that we will always have the latest version.
      required_build_files=(
          "shell_include.sh"
          "wpt-bisect-helper.sh"
      )
      for file in "${required_build_files[@]}"; do
          cp "${LADYBIRD_SOURCE_DIR}/Meta/${file}" "${temp_file_directory_base}/Meta/${file}"
      done
      cp "$0" "${temp_file_directory_base}/Meta/WPT.sh"

      git bisect start "${bad}" "${good}"
      trap cleanup_bisect INT TERM
      git bisect run "${temp_file_directory_base}/Meta/wpt-bisect-helper.sh" "${baseline_log_file}" "${@}" || true
      trap - INT TERM
      cleanup_bisect "${temp_file_directory_base}"
    popd > /dev/null
}

list_tests_wpt()
{
    ensure_wpt_repository

    construct_test_list "${@}"

    pushd "${WPT_SOURCE_DIR}" > /dev/null
        ./wpt run --list-tests ladybird "${TEST_LIST[@]}" "${WPT_TEST_TYPE_ARGS[@]}"
    popd > /dev/null
}

import_wpt()
{
    ensure_wpt_repository

    pushd "${WPT_SOURCE_DIR}" > /dev/null
       if ! git fetch origin > /dev/null; then
            echo "Failed to fetch the WPT repository, please check your network connection."
            exit 1
        fi
        local local_hash
        local_hash=$(git rev-parse HEAD)
        local remote_hash
        remote_hash=$(git rev-parse origin/master)

        if [ "$local_hash" != "$remote_hash" ]; then
            echo "WPT repository is not up to date, please run '$0 update' first."
            exit 1
        fi
    popd > /dev/null

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
        ./Meta/ladybird.py build test-web
        trap 'exit 1' EXIT INT TERM
        for path in "${TESTS[@]}"; do
            echo "Importing test from ${path}"
            if ! ./Meta/import-wpt-test.py "${IMPORT_ARGS[@]}" https://wpt.live/"${path}"; then
                continue
            fi
            "${TEST_WEB_BINARY}" --rebaseline -f "$path" || true
        done
        trap - EXIT INT TERM
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
    update_hosts_file_if_needed
    execute_wpt "${@}"
    rm -rf "${METADATA_DIR}"
}

if [[ "$CMD" =~ ^(update|clean|run|serve|bisect|compare|import|list-tests)$ ]]; then
    case "$CMD" in
        update)
            update_wpt
            ;;
        run)
            run_wpt "${@}"
            ;;
        clean)
            rm -rf "${WPT_PROFILES_PARENT}"
            if [[ $OSTYPE == 'linux'* ]]; then
                cleanup_run_infra
                cleanup_run_dirs true
            fi
            ;;
        serve)
            serve_wpt
            ;;
        bisect)
          if [ $# -lt 3 ]; then
              echo "Usage: $0 bisect <bad> <good> [test paths...]"
              usage
          fi
          bisect_wpt "${@}"
          ;;
        import)
            while [[ "$1" =~ ^--(force|wpt-base-url)$ ]]; do
                if [ "$1" = "--wpt-base-url" ]; then
                    if [ -z "$2" ]; then
                        echo "Missing argument for --wpt-base-url"
                        usage
                    fi
                    IMPORT_ARGS+=( "--wpt-base-url=$2" )
                    shift
                else
                    IMPORT_ARGS+=( "$1" )
                fi
                shift
            done
            if [ $# -eq 0 ]; then
                usage
            fi
            INPUT_PATHS=( "$@" )
            import_wpt
            ;;

        compare)
            INPUT_LOG_NAME="$(realpath "$1")"
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
