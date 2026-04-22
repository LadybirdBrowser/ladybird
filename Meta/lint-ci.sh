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
        Meta/Linters/check_debug_flags.sh \
        Meta/Linters/check_flatpak.py \
        Meta/Linters/check_html_doctype.py \
        Meta/Linters/check_idl_files.py \
        Meta/Linters/check_newlines_at_eof.py \
        Meta/Linters/check_png_sizes.sh \
        Meta/Linters/check_style.py \
        Meta/Linters/lint_executable_resources.sh \
        Meta/Linters/lint_prettier.sh \
        Meta/Linters/lint_python.sh \
        Meta/Linters/lint_shell_scripts.sh; do
    if "${cmd}" "$@"; then
        echo -e "[${GREEN}OK${NC}]: ${cmd}"
    else
        echo -e "[${BOLD_RED}FAIL${NC}]: ${cmd}"
        ((FAILURES+=1))
    fi
done

if { git ls-files '*.ipc' | xargs Meta/Linters/lint_ipc.py; }; then
    echo -e "[${GREEN}OK${NC}]: Meta/Linters/lint_ipc.py"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: Meta/Linters/lint_ipc.py"
    ((FAILURES+=1))
fi

if Meta/Linters/lint_clang_format.py --overwrite-inplace "$@" && git diff --exit-code -- ':*.cpp' ':*.h' ':*.mm'; then
    echo -e "[${GREEN}OK${NC}]: Meta/Linters/lint_clang_format.py"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: Meta/Linters/lint_clang_format.py"
    ((FAILURES+=1))
fi

if cargo fmt --check ; then
    echo -e "[${GREEN}OK${NC}]: cargo fmt --check"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: cargo fmt --check"
    ((FAILURES+=1))
fi

if cargo clippy -- -D clippy::all ; then
    echo -e "[${GREEN}OK${NC}]: cargo clippy -- -D clippy::all"
else
    echo -e "[${BOLD_RED}FAIL${NC}]: cargo clippy -- -D clippy::all"
    ((FAILURES+=1))
fi

exit "${FAILURES}"
