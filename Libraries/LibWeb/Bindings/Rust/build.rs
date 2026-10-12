/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Finds the Rust bindings of the interfaces implemented in Rust, which Meta/Generators/generate_libweb_bindings.py
//! writes to Bindings/RustBindings.rs in the build directory of LibWeb. A build of LibWeb names that file in
//! LIBWEB_RUST_BINDINGS (Libraries/LibWeb/CMakeLists.txt). Building the crate on its own, as cargo test and cargo
//! clippy do, leaves the bindings out and compiles the implementations without them. rustc tracks the file itself, as
//! the crate includes it.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-check-cfg=cfg(web_idl_bindings)");
    println!("cargo:rerun-if-env-changed=LIBWEB_RUST_BINDINGS");
    let Some(bindings) = env::var_os("LIBWEB_RUST_BINDINGS") else {
        return;
    };
    let bindings = PathBuf::from(bindings);
    assert!(
        bindings.is_file(),
        "LIBWEB_RUST_BINDINGS names {}, which does not exist; a build of LibWeb generates the bindings before it builds \
         the crate",
        bindings.display()
    );
    println!("cargo:rustc-cfg=web_idl_bindings");
    println!("cargo:rustc-env=LIBWEB_RUST_BINDINGS={}", bindings.display());
}
