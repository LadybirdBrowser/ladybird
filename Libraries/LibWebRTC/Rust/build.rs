/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::env;
use std::error::Error;
use std::path::PathBuf;
use std::process::Command;

fn main() -> Result<(), Box<dyn Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let source_dir = manifest_dir.join("../../..");
    let generator = source_dir.join("Meta/Generators/generate_ipc_definitions.py");
    println!("cargo:rerun-if-changed={}", generator.display());
    // NB: The generator imports these from Meta/Utils.
    for module in ["lexer.py", "utils.py"] {
        println!(
            "cargo:rerun-if-changed={}",
            source_dir.join("Meta/Utils").join(module).display()
        );
    }

    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    println!("cargo:rerun-if-env-changed=PYTHON");
    let python = env::var_os("PYTHON").unwrap_or_else(|| if cfg!(windows) { "python" } else { "python3" }.into());
    for endpoint in ["WebRTCClientServer", "WebRTCClientClient"] {
        let definition = source_dir.join(format!("Services/WebRTCClient/{endpoint}.ipc"));
        println!("cargo:rerun-if-changed={}", definition.display());
        let output = out_dir.join(format!("{endpoint}Endpoint.rs"));
        let status = Command::new(&python)
            .arg(&generator)
            .arg("--input")
            .arg(&definition)
            .arg("--rust-output")
            .arg(&output)
            .status()?;
        if !status.success() {
            return Err(format!("IPC endpoint generation failed for {endpoint}").into());
        }
    }
    Ok(())
}
