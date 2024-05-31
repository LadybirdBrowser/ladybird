#!/usr/bin/env bash

set -eo pipefail

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.."

MISSING_FLAGS=n

# Check whether all_the_debug_macros.cmake sets all the flags used in C++ code.
while IFS= read -r FLAG; do
    # We intentionally don't check for commented-out lines,
    # in order to keep track of false positives.
    if ! grep -qF "set(${FLAG} ON)" Meta/CMake/all_the_debug_macros.cmake ; then
        echo "'all_the_debug_macros.cmake' is missing ${FLAG}"
        MISSING_FLAGS=y
    fi
done < <(
    if [ "$#" -eq "0" ]; then
        git ls-files -- \
            '*.cpp' \
            '*.h' \
            '*.in'
    else
        # We're in the middle of a pre-commit run, so we should only check the files that have
        # actually changed. The reason is that "git ls-files | grep" on the entire repo takes
        # about 100ms. That is perfectly fine during a CI run, but becomes noticeable during a
        # pre-commit hook. It is unnecessary to check the entire repository on every single
        # commit, so we save some time here.
        for file in "$@"; do
            if [[ ("${file}" =~ \.cpp || "${file}" =~ \.h || "${file}" =~ \.in) ]]; then
                echo "$file"
            fi
        done
    fi \
    | xargs grep -E '(_DEBUG|DEBUG_)' \
    | sed -re 's,^.*[^a-zA-Z0-9_]([a-zA-Z0-9_]*DEBUG[a-zA-Z0-9_]*).*$,\1,' \
    | sort -u
)

if [ "n" != "${MISSING_FLAGS}" ] ; then
    echo "Some flags are missing for the ALL_THE_DEBUG_MACROS feature."
    echo "If you just added a new SOMETHING_DEBUG flag, that's great!"
    echo "We want to enable all of these in automated builds, so that the code doesn't rot."
    echo "Please add it to Meta/CMake/all_the_debug_macros.cmake"
    echo "Or perhaps it's not a debug flag?"
    echo "Please also add it to Meta/CMake/all_the_debug_macros.cmake"
    exit 1
fi
