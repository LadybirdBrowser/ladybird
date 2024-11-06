#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done < <(
        git ls-files \
            --exclude-from .prettierignore \
            -- \
            '*.js' '*.mjs'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == *".js" ]] || [[ "${file}" == *".mjs" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v prettier >/dev/null 2>&1 ; then
        echo "prettier is not available, but JS files need linting! Either skip this script, or install prettier."
        exit 1
    fi

    if ! prettier --version | grep -q '\b3\.' ; then
        echo "You are using '$(prettier --version)', which appears to not be prettier 3."
        exit 1
    fi

    prettier --check "${files[@]}"
else
    echo "No .js or .mjs files to check."
fi
