#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

BOLD_RED='\033[0;1;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

FAILURES=0

set +e

for cmd in \
        Meta/check-debug-flags.sh \
        Meta/check-html-doctype.py \
        Meta/check-idl-files.py \
        Meta/check-newlines-at-eof.py \
        Meta/check-png-sizes.sh \
        Meta/check-style.py \
        Meta/lint-executable-resources.sh \
        Meta/lint-gn.sh \
        Meta/lint-prettier.sh \
        Meta/lint-python.sh \
        Meta/lint-shell-scripts.sh; do
    if "${cmd}" "$@"; then
        echo -e "[${GREEN}OK${NC}]: ${cmd}"
    else
        echo -e "[${BOLD_RED}FAIL${NC}]: ${cmd}"
        ((FAILURES+=1))
    fi
done

if [ -x ./Build/lagom/bin/IPCMagicLinter ]; then
    if { git ls-files '*.ipc' | xargs ./Build/lagom/bin/IPCMagicLinter; }; then
        echo -e "[${GREEN}OK${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
    else
        echo -e "[${BOLD_RED}FAIL${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
        ((FAILURES+=1))
    fi
else
    echo -e "[${GREEN}SKIP${NC}]: IPCMagicLinter (in Meta/lint-ci.sh)"
fi

if Meta/lint-clang-format.sh --overwrite-inplace "$@" && git diff --exit-code -- ':*.cpp' ':*.h' ':*.mm'; then
    echo -e "[${GREEN}OK${NC}]: Meta/lint-clang-format.sh"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: Meta/lint-clang-format.sh"
    ((FAILURES+=1))
fi

if Meta/lint-swift.sh "$@" && git diff --exit-code -- ':*.swift'; then
    echo -e "[${GREEN}OK${NC}]: Meta/lint-swift.sh"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: Meta/lint-swift.sh"
    ((FAILURES+=1))
fi

exit "${FAILURES}"
