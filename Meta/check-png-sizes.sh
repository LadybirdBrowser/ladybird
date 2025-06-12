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

files=()
for file in "$@"; do
    if [[ "${file}" == *".png" ]]; then
        files+=("${file}")
    fi
done

if (( ${#files[@]} )); then
    # We need to allow optipng to write output so we can check what it actually did. We use a dummy file that's discarded afterwards.
    optimizations=$( printf '%s\0' "${files[@]}" |\
        xargs -0 -n1 optipng -strip all -out dummy-optipng-output.png -clobber 2>&1 |\
        grep -i -e 'Output IDAT size =' |\
        sed -E 's/Output IDAT size = [0-9]+ byte(s?) \(([0-9]+) byte(s?) decrease\)/\2/g;s/Output IDAT size = [0-9]+ byte(s?) \(no change\)/0/g' |\
        awk "{ if (\$1 >= $MINIMUM_OPTIMIZATION_BYTES) { S+=\$1 } } END { print S }")
    rm -f dummy-optipng-output.png dummy-optipng-output.png.bak
    optimizations="${optimizations:-0}"

    if [[ "$optimizations" -ne 0 ]] ; then
        echo "There are non-optimized PNG images in Base/. It is possible to reduce file sizes by at least $optimizations byte(s)."
        # shellcheck disable=SC2016 # we're not trying to expand expressions here
        echo 'Please run optipng with `-strip all` on modified PNG images and try again.'
        exit 1
    fi
else
    echo 'No PNG images to check.'
fi
