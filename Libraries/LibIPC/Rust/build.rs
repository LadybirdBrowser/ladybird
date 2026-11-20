/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn Error>> {
    println!("cargo:rerun-if-changed=build.rs");

    // NB: A CMake build of the shared target directory links what it builds into libraries that link AK. Unit tests
    //     link without it.
    println!("cargo::rustc-check-cfg=cfg(ladybird_links_ak)");
    println!("cargo:rerun-if-env-changed=LADYBIRD_LINKS_AK");
    if env::var_os("LADYBIRD_LINKS_AK").is_some() {
        println!("cargo::rustc-cfg=ladybird_links_ak");
    }

    // NB: Only LibIPC's copy of the crate, which C++ calls, has the C++ interface.
    if env::var_os("CARGO_FEATURE_CPP_TRANSPORT").is_none() || env::var("CARGO_CFG_TARGET_VENDOR")? != "apple" {
        return Ok(());
    }

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    println!("cargo:rerun-if-changed=cbindgen.toml");

    let mut config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;
    // NB: An ak::Span is laid out as an AK ReadonlySpan, which C++ passes it as.
    for (rust_name, cpp_name) in [("Span", "AK::ReadonlySpan"), ("RawAttachment", "IPC::RawAttachment")] {
        config.export.rename.insert(rust_name.to_string(), cpp_name.to_string());
    }

    let source = "src/cpp_mach_transport.rs";
    println!("cargo:rerun-if-changed={source}");
    cbindgen::Builder::new()
        .with_config(config)
        .with_src(manifest_dir.join(source))
        .generate()?
        .write_to_file(out_dir.join("RustFFI.h"));
    Ok(())
}
