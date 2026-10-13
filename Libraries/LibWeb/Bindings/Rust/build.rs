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
//!
//! Also generates what the WebGL implementation shares with the rest of the tree: the vocabulary of the command
//! stream, the methods of its recorder and the GL constants. Those do not need the bindings, so every build of the
//! crate has them.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

#[path = "generate/webgl_constants.rs"]
mod webgl_constants;

#[path = "generate/webgl_proxy.rs"]
mod webgl_proxy;

#[path = "generate/webgl_wire.rs"]
mod webgl_wire;

#[path = "generate/webgl_wire_model.rs"]
mod webgl_wire_model;

fn read_tracked(path: &Path) -> String {
    println!("cargo:rerun-if-changed={}", path.display());
    fs::read_to_string(path).unwrap_or_else(|error| panic!("read {}: {error}", path.display()))
}

fn generate_webgl_command_recording(libraries: &Path, out_dir: &Path) {
    let functions_path = libraries.join("LibCompositing/WebGL/GLFunctions.json");
    let generate = |json: &str| -> Result<(String, String), String> {
        let functions = webgl_wire_model::parse_functions(json)?;
        let vocabulary = webgl_wire_model::vocabulary(&functions)?;
        Ok((
            webgl_wire::write_wire_vocabulary(&vocabulary),
            webgl_proxy::write_proxy(&functions)?,
        ))
    };
    let (wire_vocabulary, proxy) = generate(&read_tracked(&functions_path))
        .unwrap_or_else(|error| panic!("{}: {error}", functions_path.display()));
    fs::write(out_dir.join("webgl_wire.rs"), wire_vocabulary).expect("write webgl_wire.rs");
    fs::write(out_dir.join("webgl_proxy.rs"), proxy).expect("write webgl_proxy.rs");
}

fn idl_files_in(directory: &Path, idl_files: &mut Vec<PathBuf>) {
    // NB: Tracking the directory reruns the script when an IDL file is added to it or removed from it.
    println!("cargo:rerun-if-changed={}", directory.display());
    let entries = fs::read_dir(directory).unwrap_or_else(|error| panic!("read {}: {error}", directory.display()));
    for entry in entries {
        let path = entry.expect("directory entry").path();
        if path.is_dir() {
            idl_files_in(&path, idl_files);
        } else if path.extension().is_some_and(|extension| extension == "idl") {
            idl_files.push(path);
        }
    }
}

fn generate_webgl_constants(libraries: &Path, out_dir: &Path) {
    let mut idl_files = Vec::new();
    idl_files_in(&libraries.join("LibWeb/WebGL"), &mut idl_files);
    // NB: The order of a directory listing is not defined, and the order of the constants follows from it.
    idl_files.sort();
    let mut constants = Vec::new();
    for idl_file in &idl_files {
        webgl_constants::collect_constants(&read_tracked(idl_file), &mut constants)
            .unwrap_or_else(|error| panic!("{}: {error}", idl_file.display()));
    }
    fs::write(
        out_dir.join("webgl_constants.rs"),
        webgl_constants::write_constants(&constants),
    )
    .expect("write webgl_constants.rs");
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=generate");
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    let libraries = manifest_dir
        .ancestors()
        .nth(3)
        .expect("the crate is in Libraries/LibWeb/Bindings/Rust");
    generate_webgl_command_recording(libraries, &out_dir);
    generate_webgl_constants(libraries, &out_dir);

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
