#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

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
    if ! command -v flake8 >/dev/null 2>&1 ; then
        echo "flake8 is not available, but python files need linting! Either skip this script, or install flake8."
        exit 1
    fi

    flake8 "${files[@]}" --max-line-length=120
else
    echo "No py files to check."
fi
