#!/usr/bin/env bash

set +e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        if [[ $file == Meta/CMake/vcpkg/overlay-ports/* ]]; then
            continue
        fi
        files+=("$file")
    done < <(
        git ls-files '*.cmake' '*/CMakeLists.txt'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == Meta/CMake/vcpkg/overlay-ports/* ]]; then
            continue
        fi
        if [[ "${file}" == *".cmake" ]] || [[ "${file}" == *"CMakeLists.txt" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v cmake-format >/dev/null 2>&1 ; then
        echo "cmake-format is not available, but CMake files need linting! Please install it: pip3 install cmakelang"
        exit 1
    fi

    cmake-format -c .cmake-format --check "${files[@]}"
    result=$?
    cmake-format -c .cmake-format --in-place "${files[@]}"
    exit "$result"
else
    echo "No .cmake or CMakeLists.txt files to check."
fi
