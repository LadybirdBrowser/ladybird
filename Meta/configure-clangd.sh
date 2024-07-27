#!/usr/bin/env bash

function usage {
    echo "$0 <.clangd file path> [release|debug]"
    echo "Update local clangd configuration with the proper"
    echo "compilation database according to the selected build type."
}

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

# Check if the user has sed.

if ! which sed >/dev/null 2>&1; then
    echo "Error: No sed found. Cannot configure .clangd automatically."
    exit 1
fi

# Check if the user specified the right number of parameters.
if [ $# -ne 2 ]; then
    usage 
    exit 1
fi

clangd_file_path=$1
if [ ! -f "$clangd_file_path" ]; then
    echo "Error: ${clangd_file_path} is not a regular file."
    echo
    usage
    exit 1
fi

build_type=""
case $2 in
    Debug)
        build_type="-debug"
        ;;
    default)
        build_type=""
        ;;
    Sanitizer)
        build_type="-sanitizers"
        ;;
    *)
        echo "Invalid build configuration specified: $2"
        usage
        exit 1
esac

sed -i '' "s/\(^[  ]*CompilationDatabase:\).*$/\1 Build\/ladybird${build_type}/" "$clangd_file_path"
