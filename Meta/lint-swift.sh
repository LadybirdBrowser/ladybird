#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done < <(
        git ls-files '*.swift'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == *".swift" ]] ; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v swift-format >/dev/null 2>&1 ; then
        echo "swift-format is not available, but Swift files need linting! Either skip this script, or install swift-format."
        exit 1
    fi
    swift-format -i "${files[@]}"
    echo "Maybe some files have changed. Sorry, but swift-format doesn't indicate what happened."
else
    echo "No .swift files to check."
fi
