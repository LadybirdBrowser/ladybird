/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::process::Command;

fn main() {
    // The driver loads librustc_driver from the toolchain it was built with.
    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".into());
    let output = Command::new(rustc)
        .args(["--print", "sysroot"])
        .output()
        .expect("rustc --print sysroot");
    let sysroot = String::from_utf8(output.stdout).expect("UTF-8 sysroot");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}/lib", sysroot.trim());
}
