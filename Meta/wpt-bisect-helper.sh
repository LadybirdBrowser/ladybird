#!/usr/bin/env bash

set -euo pipefail

# An exit code of 255 tells git to stop bisecting immediately.
if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <log_file> [test paths...]"
    exit 255
fi

baseline_log_file="$1"
if [ ! -f "${baseline_log_file}" ]; then
    echo "Baseline log file '${baseline_log_file}' does not exist."
    exit 255
fi
shift

WPT_SCRIPT_PATH=${WPT_SCRIPT_PATH:-"$(dirname "$0")/WPT.sh"}

pushd "${LADYBIRD_SOURCE_DIR}" > /dev/null || exit 255
    trap "exit 255" INT TERM
    if ! ./Meta/ladybird.py rebuild WebDriver; then
        # When going back over many commits rebuilds may be flaky. Let's try again before skipping this commit.
        if ! ./Meta/ladybird.py rebuild WebDriver; then
            echo "Build failed twice in a row, skipping this commit."
            exit 125
        fi
    fi

    current_commit="$(git rev-parse HEAD)"

    # If comparing to the baseline has no unexpected results this commit is bad, because we create the basseline from
    # the given bad commit.
    if "${WPT_SCRIPT_PATH}" compare "${baseline_log_file}" "${@}"; then
        echo "Commit ${current_commit} is bad."
        exit 1
    fi
    trap - INT TERM
popd > /dev/null || exit 255

echo "Commit ${current_commit} is good."
exit 0
