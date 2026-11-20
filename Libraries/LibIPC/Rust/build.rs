/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");

    // NB: A CMake build of the shared target directory links what it builds into libraries that link AK. Unit tests
    //     link without it.
    println!("cargo::rustc-check-cfg=cfg(ladybird_links_ak)");
    println!("cargo:rerun-if-env-changed=LADYBIRD_LINKS_AK");
    if env::var_os("LADYBIRD_LINKS_AK").is_some() {
        println!("cargo::rustc-cfg=ladybird_links_ak");
    }
}
