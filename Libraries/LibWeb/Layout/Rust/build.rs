/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::{Path, PathBuf};

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-env-changed=FFI_OUTPUT_DIR");
    println!("cargo:rerun-if-changed=src");

    let ffi_out_dir = env::var("FFI_OUTPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| out_dir.clone());
    let config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;
    let header = Path::new("Layout/TreeBuilderRustFFI.h");
    let builder = cbindgen::Builder::new().with_config(config).with_crate(&manifest_dir);

    builder.generate().map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            other => panic!("{other:?}"),
        },
        |bindings| {
            let output_header = out_dir.join(header);
            std::fs::create_dir_all(output_header.parent().unwrap()).unwrap();
            bindings.write_to_file(output_header);
            if ffi_out_dir != out_dir {
                let ffi_header = ffi_out_dir.join(header);
                std::fs::create_dir_all(ffi_header.parent().unwrap()).unwrap();
                bindings.write_to_file(ffi_header);
            }
        },
    );

    Ok(())
}
