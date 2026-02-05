# shellcheck shell=bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

pick_host_compiler() {
    local output
    local status

    output=$("${DIR}/find_compiler.py")
    status=$?

    if [[ ${status} -ne 0 ]] ; then
        exit ${status}
    fi

    if [[ "${output}" != *"CC="* || "${output}" != *"CXX="* ]] ; then
        echo "Unexpected output from find_compiler.py"
        exit 1
    fi

    eval "${output}"
}
