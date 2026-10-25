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
        Meta/Linters/check_fetch_engine_includes.py \
        Meta/Linters/check_flatpak.py \
        Meta/Linters/check_html_doctype.py \
        Meta/Linters/check_idl_files.py \
        Meta/Linters/check_libjs_includes.py \
        Meta/Linters/check_libjs_rust_api.py \
        Meta/Linters/check_libweb_realm_mentions.py \
        Meta/Linters/check_newlines_at_eof.py \
        Meta/Linters/check_png_sizes.sh \
        Meta/Linters/check_rust_process_wide_state.py \
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

    # The GC lints link the compiler of a pinned nightly, which only some setups have.
    rust_lints_toolchain=$(sed -n 's/^channel = "\(.*\)"$/\1/p' Meta/RustLints/rust-toolchain.toml)
    if rustup component list --toolchain "${rust_lints_toolchain}" --installed 2>/dev/null | grep -q '^rustc-dev'; then
        rust_lints_target_dir="${script_path}/../Build/rust-lints"
        if CARGO_TARGET_DIR="${rust_lints_target_dir}" cargo +"${rust_lints_toolchain}" test --quiet \
                --manifest-path Meta/RustLints/Cargo.toml \
            && CARGO_TARGET_DIR="${rust_lints_target_dir}" Meta/RustLints/run.sh --quiet ; then
            echo -e "[${GREEN}OK${NC}]: Meta/RustLints/run.sh"
        else
            echo -e "[${BOLD_RED}FAIL${NC}]: Meta/RustLints/run.sh"
            ((FAILURES+=1))
        fi
    else
        echo "Skipping Meta/RustLints/run.sh: ${rust_lints_toolchain} with rustc-dev is not installed."
    fi
else
    echo "No Rust files to check."
fi

exit "${FAILURES}"
