/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::PathBuf;

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

    let base_config = cbindgen::Config::from_file(manifest_dir.join("cbindgen.toml"))?;

    // CSS tokenizer header — namespace Web::CSS::Parser::FFI, CSS types only.
    let mut css_config = base_config;
    css_config.namespaces = Some(vec![
        "Web".to_string(),
        "CSS".to_string(),
        "Parser".to_string(),
        "FFI".to_string(),
    ]);
    css_config.export.include = vec![
        "CssHashType".to_string(),
        "CssNumberType".to_string(),
        "CssToken".to_string(),
        "CssTokenType".to_string(),
    ];

    cbindgen::generate_with_config(&manifest_dir, css_config).map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            other => panic!("{other:?}"),
        },
        |bindings| {
            bindings.write_to_file(out_dir.join("RustFFI.h"));
            if ffi_out_dir != out_dir {
                bindings.write_to_file(ffi_out_dir.join("RustFFI.h"));
            }
        },
    );

    Ok(())
}
