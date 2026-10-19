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
    println!("cargo:rerun-if-env-changed=RUST_ECHO_SERVER_ENDPOINT");
    let endpoint = if let Some(path) = env::var_os("RUST_ECHO_SERVER_ENDPOINT") {
        let path = PathBuf::from(path);
        println!("cargo:rerun-if-changed={}", path.display());
        path
    } else {
        let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
        let definition = manifest_dir.join("../RustEchoServer.ipc");
        let generator = manifest_dir.join("../../../Meta/Generators/generate_ipc_definitions.py");
        println!("cargo:rerun-if-changed={}", definition.display());
        println!("cargo:rerun-if-changed={}", generator.display());
        // NB: The generator imports these from Meta/Utils.
        for module in ["lexer.py", "utils.py"] {
            println!(
                "cargo:rerun-if-changed={}",
                manifest_dir.join("../../../Meta/Utils").join(module).display()
            );
        }
        let output = PathBuf::from(env::var("OUT_DIR")?).join("RustEchoServerEndpoint.rs");
        let status = Command::new("python3")
            .arg(generator)
            .arg("--input")
            .arg(definition)
            .arg("--rust-output")
            .arg(&output)
            .status()?;
        if !status.success() {
            return Err("IPC endpoint generation failed".into());
        }
        output
    };
    println!("cargo:rustc-env=RUST_ECHO_SERVER_ENDPOINT={}", endpoint.display());
    Ok(())
}
