/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the C headers of the embedding ABI into LibJS/Embedding under OUT_DIR, from the sources of the runtime
//! crate: ABI.h, the declarations of the ABI's functions and types, from src/embedding with cbindgen, and Layout.h,
//! the offsets, sizes and constants of the runtime's structures that an embedder reads or writes directly, from the
//! layout module, which the runtime's own build script also compiles to check the same layout against its types.

use std::env;
use std::fs;
use std::path::PathBuf;

// The build script only reads the layout, so most of what the runtime uses goes unused here.
#[allow(dead_code)]
#[path = "../src/layout/mod.rs"]
mod layout;

#[path = "../generate/layout_forward.rs"]
mod layout_forward;

#[path = "../src/gc/class_id.rs"]
mod class_id;

#[path = "../generate/abi_header.rs"]
mod abi_header;

#[path = "../generate/c_type_spelling.rs"]
mod c_type_spelling;

// The build script only writes the layout header, whose static assertions the runtime's build script compiles.
#[allow(dead_code)]
#[path = "../generate/layout_header.rs"]
mod layout_header;

#[path = "../generate/write_if_changed.rs"]
mod write_if_changed;

fn main() {
    let runtime_directory = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR")).join("..");
    let output_directory = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));

    println!("cargo:rerun-if-changed=build.rs");
    for input in ["generate", "src/embedding", "src/layout", "src/gc/class_id.rs"] {
        println!("cargo:rerun-if-changed={}", runtime_directory.join(input).display());
    }

    let header_directory = output_directory.join("LibJS/Embedding");
    fs::create_dir_all(&header_directory).expect("create the directory of the embedding headers");
    abi_header::generate(
        &runtime_directory.join("src/embedding/mod.rs"),
        &header_directory.join("ABI.h"),
    );
    write_if_changed::write_if_changed(&header_directory.join("Layout.h"), &layout_header::generate().text);
}
