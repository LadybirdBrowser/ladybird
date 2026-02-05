#!/usr/bin/env bash
set -e

# Feature options
export LLVM_VERSION=${LLVM_VERSION:-19}
DISTRO=${DISTRO:-ubuntu}

# call distro-specific script that lives in this directory
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# shellcheck source=/dev/null
. "${DIR}/install-${DISTRO}.sh"
