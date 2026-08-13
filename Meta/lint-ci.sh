#!/usr/bin/env bash

set -e

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "${script_path}/.." || exit 1

if [[ -z "${CARGO_TARGET_DIR:-}" ]]; then
    export CARGO_TARGET_DIR="${script_path}/../Build/release/cargo/build"
fi

should_lint_rust=1
if (( $# > 0 )); then
    should_lint_rust=0
    for path in "$@"; do
        case "${path}" in
            *.rs|*/Cargo.toml|Cargo.toml|*/Cargo.lock|Cargo.lock|*/rust-toolchain.toml|rust-toolchain.toml|*/rustfmt.toml|rustfmt.toml)
                should_lint_rust=1
                break
                ;;
        esac
    done
fi

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
        Meta/Linters/check_libweb_realm_mentions.py \
        Meta/Linters/check_newlines_at_eof.py \
        Meta/Linters/check_png_sizes.sh \
        Meta/Linters/check_style.py \
        Meta/Linters/check_vcpkg.py \
        Meta/Linters/lint_executable_resources.sh \
        Meta/Linters/lint_prettier.sh \
        Meta/Linters/lint_python.sh \
        Meta/Linters/lint_shell_scripts.sh \
        Meta/Linters/lint_workflows.sh; do
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

if (( should_lint_rust )); then
    if cargo fmt --check ; then
        echo -e "[${GREEN}OK${NC}]: cargo fmt --check"
    else
        echo -e "[${BOLD_RED}FAIL${NC}]: cargo fmt --check"
        ((FAILURES+=1))
    fi

    rust_target_triple=$(rustc -vV | sed -n 's/^host: //p')
    if cargo clippy --release --target "${rust_target_triple}" -- -D clippy::all ; then
        echo -e "[${GREEN}OK${NC}]: cargo clippy -- -D clippy::all"
    else
        echo -e "[${BOLD_RED}FAIL${NC}]: cargo clippy -- -D clippy::all"
        ((FAILURES+=1))
    fi
else
    echo "No Rust files to check."
fi

exit "${FAILURES}"
