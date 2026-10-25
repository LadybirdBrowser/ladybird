#!/usr/bin/env bash

# Builds the lint driver on its pinned nightly and checks the crates that use libjs_rust's GC with it.
# Extra arguments go to `cargo check`.

set -euo pipefail

script_path=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
root="${script_path}/../.."

toolchain=$(sed -n 's/^channel = "\(.*\)"$/\1/p' "${script_path}/rust-toolchain.toml")
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-${root}/Build/rust-lints}"

cargo +"${toolchain}" build --release --manifest-path "${script_path}/Cargo.toml"

# Cargo fingerprints the wrapper's path, not its contents, so give each build of the driver its own path.
driver="${CARGO_TARGET_DIR}/release/ladybird-rust-lints"
driver_hash=$(shasum "${driver}" | cut -c1-16)
mkdir -p "${CARGO_TARGET_DIR}/drivers/${driver_hash}"
cp "${driver}" "${CARGO_TARGET_DIR}/drivers/${driver_hash}/"

cd "${root}"
RUSTC_WORKSPACE_WRAPPER="${CARGO_TARGET_DIR}/drivers/${driver_hash}/ladybird-rust-lints" \
    cargo +"${toolchain}" check --release \
    -p libjs_rust -p libweb_bindings_rust -p libweb_rust "$@"
