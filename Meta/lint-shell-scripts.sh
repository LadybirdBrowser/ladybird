#!/usr/bin/env bash

set -eo pipefail

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_path/.."

if [ "$#" -eq "0" ]; then
    mapfile -t files < <(
        git ls-files -- \
            '*.sh' \
    )
else
    files=()
    for file in "$@"; do
        # Skip ports, like we in the CI case above.
        if [[ "${file}" =~ "Ports" ]]; then
           continue
        fi

        if [[ "${file}" == *".sh" && "${file}" != "Base/root/generate_manpages.sh" ]]; then
            files+=("${file}")
        fi
    done
fi

if (( ${#files[@]} )); then
    if ! command -v shellcheck &>/dev/null ; then
        echo "shellcheck is not available, but shell files need linting! Either skip this script, or install shellcheck."
        exit 1
    fi

    shellcheck --source-path=SCRIPTDIR "${files[@]}"

    for file in "${files[@]}"; do
        if (< "$file" grep -qE "grep [^|);]*-[^- ]*P"); then
            # '\x2D' is the unicode escape sequence for '-'. This is used so
            # that this script does not flag itself for containing grep dash P.
            echo -e "The script '$file' contains 'grep \x2DP', which is not supported on macOS. Please use grep -E instead."
            exit 1
        fi
    done
else
    echo "No .sh files to check."
fi
