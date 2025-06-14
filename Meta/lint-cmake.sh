#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

# These use @FOO@ syntax which evidently requires preprocessing, and gersemi does not like.
ignored_files=(
    'Meta/CMake/vcpkg/overlay-ports/skia/unofficial-skia-config.cmake'
    'Meta/CMake/vcpkg/overlay-ports/skia/unofficial-skia-targets-details.cmake'
    'Meta/CMake/vcpkg/overlay-ports/skia/unofficial-skia-targets.cmake'
)

if [ "$#" -eq "0" ]; then
    files=()
    while IFS= read -r file; do
        is_ignored=0
        for name in "${ignored_files[@]}"; do
            if [[ "$name" == "$file" ]]; then
                is_ignored=1
                break
            fi
        done
        if [[ $is_ignored -eq 0 ]]; then
            files+=("$file")
        fi
    done < <(
        git ls-files '*.cmake' '*/CMakeLists.txt'
    )
else
    files=()
    for file in "$@"; do
        if [[ "${file}" == *".cmake" ]] || [[ "${file}" == *"CMakeLists.txt" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v gersemi >/dev/null 2>&1 ; then
        echo "gersemi is not available, but CMake files need linting! Please install gersemi: pip3 install gersemi"
        exit 1
    fi

    gersemi --config .gersemi --check "${files[@]}"
else
    echo "No .cmake or CMakeLists.txt files to check."
fi
