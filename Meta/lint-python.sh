#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

check_argument="--check"

if [ "$#" -gt "0" ]; then
    if  [ "--overwrite-inplace" = "$1" ] ; then
        check_argument=""
        shift
    fi
fi

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done <  <(
        git ls-files '*.py'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == *".py" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v black >/dev/null 2>&1 ; then
        echo "black is not available, but python files need linting! Either skip this script, or install black."
        exit 1
    fi

    black ${check_argument} "${files[@]}"
else
    echo "No py files to check."
fi
