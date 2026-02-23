#!/usr/bin/env bash

set -eo pipefail

# How many bytes optipng has to be able to strip out of the file for the optimization to be worth it. The default is 1 KiB.
: "${MINIMUM_OPTIMIZATION_BYTES:=1024}"

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.."

if ! command -v optipng >/dev/null ; then
    if [[ "$GITHUB_ACTIONS" == "true" ]]; then
        echo 'optipng is not installed, failing check because running in CI.'
        exit 1
    fi
    echo 'optipng is not installed, skipping png size check.'
    echo 'Please install optipng for your system to run this check.'
    exit 0
fi

if [ "$#" -eq "0" ]; then
    input_files=()
    while IFS= read -r file; do
        input_files+=("${file}")
    done < <(git ls-files -- '*.png')
else
    input_files=("$@")
fi

files=()
for file in "${input_files[@]}"; do
    if [[ "${file}" == *"/wpt-import/"* ]]; then
        continue
    fi
    if [[ "${file}" == *".png" ]]; then
        files+=("${file}")
    fi
done

if (( ${#files[@]} )); then
    tmpdir=$(mktemp -d)
    # Each parallel invocation writes to its own temp file (using the sh process PID) to avoid clobbering. The per-file check
    # is done inside sh -c so that filenames and byte counts stay paired despite interleaved parallel output.
    # shellcheck disable=SC2016
    results=$(printf '%s\0' "${files[@]}" \
        | xargs -0 -n1 -P0 sh -c '
            output=$(optipng -strip all -out "$0/optipng-$$.png" -clobber "$2" 2>&1) || true
            decrease=$(printf "%s\n" "$output" | sed -nE "s/Output IDAT size = [0-9]+ byte(s?) \(([0-9]+) byte(s?) decrease\)/\2/p")
            decrease="${decrease:-0}"
            if [ "$decrease" -ge "$1" ]; then
                printf "%d %s\n" "$decrease" "$2"
            fi
        ' "$tmpdir" "$MINIMUM_OPTIMIZATION_BYTES")
    rm -rf "$tmpdir"

    if [ -n "$results" ]; then
        optimizations=$(printf '%s\n' "$results" | awk '{ S+=$1 } END { print S }')
        echo "There are non-optimized PNG images. It is possible to reduce file sizes by at least $optimizations byte(s)."
        printf '%s\n' "$results" | awk '{ print "  " $2 }'
        # shellcheck disable=SC2016 # we're not trying to expand expressions here
        echo 'Please run optipng with `-strip all` on modified PNG images and try again.'
        exit 1
    fi
else
    echo 'No PNG images to check.'
fi
