#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

overwrite=0

if [ "$#" -gt "0" ]; then
    if  [ "--overwrite-inplace" = "$1" ] ; then
        overwrite=1
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
    if ! command -v ruff >/dev/null 2>&1 ; then
        echo "Please install ruff: pip3 install ruff"
        exit 1
    fi

    if [[ ${overwrite} -eq 0 ]] ; then
        ruff check "${files[@]}"
        ruff format --check "${files[@]}"
    else
        ruff check --fix "${files[@]}"
        ruff format "${files[@]}"
    fi
else
    echo "No py files to check."
fi
