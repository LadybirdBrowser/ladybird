/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! flapc links libclang to read LibJS data layouts from the C++ headers. CMake
//! locates it against the compiler building the tree and passes the directory
//! here, so the layouts flapc reports match what that compiler produces.

fn main() {
    println!("cargo:rerun-if-env-changed=LIBCLANG_LIB_DIR");
    if let Ok(directory) = std::env::var("LIBCLANG_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
        if std::env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{directory}");
        }
    }

    let library = match std::env::var("CARGO_CFG_TARGET_ENV").as_deref() {
        Ok("msvc") => "libclang",
        _ => "clang",
    };
    println!("cargo:rustc-link-lib=dylib={library}");
}
