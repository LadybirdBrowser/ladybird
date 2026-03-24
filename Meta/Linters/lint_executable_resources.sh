#!/usr/bin/env bash

set -eo pipefail

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/../.." || exit 1

tests_find_args=(
    Tests/
    \( -name WPT -o -path '*/wpt-import' \) -prune
    -or
)

ignored_executable_paths='^(Meta/lint-executable-resources\.sh)$'

if [ "$(uname -s)" = "Darwin" ]; then
    # MacOS's find does not support '-executable' OR '-perm /mode'.
    BAD_FILES=$(find Base/res/ -type f -perm +111)
    BAD_FILES+=$(find "${tests_find_args[@]}" -perm +111 \! -type d -print | grep -Ev "(${ignored_executable_paths}|\\.(sh|py)$)" || true)
else
    BAD_FILES=$(find Base/res/ -type f -executable)
    BAD_FILES+=$(find "${tests_find_args[@]}" -executable \! -type d -print | grep -Ev "(${ignored_executable_paths}|\\.(sh|py)$)" || true)
fi

if [ -n "${BAD_FILES}" ]
then
    echo "These files are marked as executable, but are in directories that do not commonly"
    echo "contain executables. Please double-check the permissions of these files:"
    echo "${BAD_FILES}" | xargs ls -ld
    exit 1
fi
