#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/../.." || exit 1

overwrite=0

is_ignored_python_file() {
    case "$1" in
        Tests/LibWeb/Text/input/wpt-import/*)
            return 0
            ;;
    esac

    return 1
}

if [ "$#" -gt "0" ]; then
    if  [ "--overwrite-inplace" = "$1" ] ; then
        overwrite=1
        shift
    fi
fi

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        if ! is_ignored_python_file "$file"; then
            files+=("$file")
        fi
    done <  <(
        git ls-files '*.py'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == *".py" ]] && ! is_ignored_python_file "$file"; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    python3 Meta/Linters/run.py pyright "${files[@]}"

    if [[ ${overwrite} -eq 0 ]] ; then
        python3 Meta/Linters/run.py ruff check "${files[@]}"
        python3 Meta/Linters/run.py ruff format --check "${files[@]}"
    else
        python3 Meta/Linters/run.py ruff check --fix "${files[@]}"
        python3 Meta/Linters/run.py ruff format "${files[@]}"
    fi
else
    echo "No py files to check."
fi
