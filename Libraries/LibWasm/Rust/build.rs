/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::fmt::Write;
use std::path::{Path, PathBuf};

fn generate_opcodes(manifest_dir: &Path, out_dir: &Path) -> Result<(), Box<dyn Error>> {
    let opcode_h = manifest_dir.join("../Opcode.h");
    println!("cargo:rerun-if-changed={}", opcode_h.display());

    let contents = std::fs::read_to_string(&opcode_h)?;
    let mut output = String::new();

    // Grab name and value from _M(name, value, pops, pushes)_ lines
    let re = regex::Regex::new(r"M\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)(?:[uUlL]+)?\s*,")?;
    for cap in re.captures_iter(&contents) {
        let cpp_name = &cap[1];
        let value = &cap[2];

        // Drop "structured_" and trailing underscrores, then uppercase the whole thing.
        let mut name = cpp_name.to_string();
        if let Some(stripped) = name.strip_prefix("structured_") {
            name = stripped.to_string();
        }
        if let Some(stripped) = name.strip_suffix('_') {
            name = stripped.to_string();
        }
        let name = name.to_uppercase();

        writeln!(output, "pub const {name}: u64 = {value};")?;
    }

    std::fs::write(out_dir.join("opcodes.rs"), output)?;
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");

    generate_opcodes(&manifest_dir, &out_dir)?;

    let ffi_out_dir = env::var("FFI_OUTPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| out_dir.clone());

    cbindgen::generate(manifest_dir).map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            e => panic!("{e:?}"),
        },
        |bindings| {
            bindings.write_to_file(ffi_out_dir.join("CraneliftFFI.h"));
        },
    );

    Ok(())
}
