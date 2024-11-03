#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

if [ "$#" -eq "1" ]; then
    files=()
    while IFS= read -r file; do
        files+=("$file")
    done < <(
        git ls-files -- \
            '*.cpp' \
            '*.h' \
            '*.mm' \
            ':!:Base' \
            ':!:Userland/Libraries/LibCodeComprehension/Cpp/Tests/*' \
            ':!:Userland/Libraries/LibCpp/Tests/parser/*' \
            ':!:Userland/Libraries/LibCpp/Tests/preprocessor/*'
    )
else
    files=()
    for file in "${@:2}"; do
        if [[ "${file}" == *".cpp" || "${file}" == *".h" || "${file}" == *".mm" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    TOOLCHAIN_DIR=Toolchain/Local/clang/bin
    CLANG_FORMAT=false
    if command -v clang-format-18 >/dev/null 2>&1 ; then
        CLANG_FORMAT=clang-format-18
    elif command -v brew >/dev/null 2>&1 && command -v "$(brew --prefix llvm@18)"/bin/clang-format >/dev/null 2>&1 ; then
        CLANG_FORMAT="$(brew --prefix llvm@18)"/bin/clang-format
    elif command -v $TOOLCHAIN_DIR/clang-format >/dev/null 2>&1 && $TOOLCHAIN_DIR/clang-format --version | grep -qF ' 18.' ; then
        CLANG_FORMAT=$TOOLCHAIN_DIR/clang-format
    elif command -v clang-format >/dev/null 2>&1 ; then
        CLANG_FORMAT=clang-format
        if ! "${CLANG_FORMAT}" --version | awk '{ if (substr($NF, 1, index($NF, ".") - 1) != 18) exit 1; }'; then
            echo "You are using '$("${CLANG_FORMAT}" --version)', which appears to not be clang-format 18."
            echo "It is very likely that the resulting changes are not what you wanted."
        fi
    else
        echo "clang-format-18 is not available, but C or C++ files need linting! Either skip this script, or install clang-format-18."
        echo "(If you install a package 'clang-format', please make sure it's version 18.)"
        exit 1
    fi

    if [ "$#" -gt "0" ] && [ "--overwrite-inplace" = "$1" ] ; then
        true # The only way to run this script.
    else
        # Note that this branch also covers --help, -h, -help, -?, etc.
        echo "USAGE: $0 --overwrite-inplace"
        echo "The argument is necessary to make you aware that this *will* overwrite your local files."
        exit 1
    fi

    echo "Using ${CLANG_FORMAT}"

    "${CLANG_FORMAT}" -style=file -i "${files[@]}"
    echo "Maybe some files have changed. Sorry, but clang-format doesn't indicate what happened."
else
    echo "No .cpp, .h or .mm files to check."
fi
