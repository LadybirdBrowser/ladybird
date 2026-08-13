#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/../.." || exit 1

is_workflow_file() {
    case "$1" in
        .github/workflows/*.yml | .github/workflows/*.yaml)
            return 0
            ;;
    esac

    return 1
}

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done < <(
        git ls-files -- \
            '.github/workflows/*.yml' \
            '.github/workflows/*.yaml'
    )
else
    files=()
    for file in "$@"; do
        file="${file#"${PWD}/"}"
        file="${file#./}"

        if is_workflow_file "${file}"; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v actionlint >/dev/null 2>&1 ; then
        echo "Please install actionlint: pip3 install -r Meta/Linters/requirements.txt"
        exit 1
    fi

    # actionlint hands the contents of `run:` steps to shellcheck and pyflakes when it finds them on the PATH, which
    # makes its output depend on what happens to be installed. Disable both so that everyone sees the same results.
    actionlint -shellcheck= -pyflakes= "${files[@]}"
else
    echo "No workflow files to check."
fi
