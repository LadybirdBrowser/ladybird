/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles the interpreter, Libraries/LibWasm/Interpreter/interpreter.flap, against the layout constants of
//! src/abi.rs, and assembles it into the crate. Also generates the C++ header of the C ABI in src/capi.rs.

use std::env;
use std::fs;
use std::path::PathBuf;

#[path = "src/abi.rs"]
mod abi;

fn write_if_changed(path: &PathBuf, contents: &str) {
    if fs::read_to_string(path).is_ok_and(|existing| existing == contents) {
        return;
    }
    fs::write(path, contents).unwrap_or_else(|error| panic!("write {}: {error}", path.display()));
}

fn main() {
    let manifest_directory = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let output_directory = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let interpreter_source = manifest_directory.join("../Interpreter/interpreter.flap");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=src/abi.rs");
    println!("cargo:rerun-if-changed={}", interpreter_source.display());
    println!("cargo::rustc-check-cfg=cfg(wasm_interpreter)");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=src/capi.rs");
    println!("cargo:rerun-if-changed=src/runtime/heap.rs");

    // The bindings only cover the C ABI, so they are generated from that file alone.
    let bindings = cbindgen::Builder::new()
        .with_config(cbindgen::Config::from_file(manifest_directory.join("cbindgen.toml")).expect("read cbindgen.toml"))
        .with_src(manifest_directory.join("src/capi.rs"))
        .with_src(manifest_directory.join("src/runtime/heap.rs"))
        .generate()
        .expect("generate the C ABI header");
    bindings.write_to_file(output_directory.join("RustFFI.h"));

    let architecture = env::var("CARGO_CFG_TARGET_ARCH").expect("CARGO_CFG_TARGET_ARCH");
    let os = env::var("CARGO_CFG_TARGET_OS").expect("CARGO_CFG_TARGET_OS");
    let object_format = match os.as_str() {
        "macos" | "ios" => flapc::ObjectFormat::MachO,
        "windows" => flapc::ObjectFormat::Coff,
        _ => flapc::ObjectFormat::Elf,
    };
    // FIXME: Port the interpreter's entry and exit to AArch64 and the Windows x64 calling convention.
    if architecture != "x86_64" || object_format == flapc::ObjectFormat::Coff {
        return;
    }
    println!("cargo:rustc-cfg=wasm_interpreter");

    let layout = abi::layout_constants();
    let layout_path = output_directory.join("layout.conf");
    write_if_changed(&layout_path, &layout);

    let source = fs::read_to_string(&interpreter_source).expect("read interpreter.flap");
    let source_name = interpreter_source.display().to_string();
    let compiler = flapc::Compiler::new(flapc::CompileOptions {
        target: flapc::Target {
            architecture: flapc::Architecture::X86_64,
            object_format,
        },
        has_jscvt: false,
        enable_assertions: true,
        profiling: false,
        profile: flapc::Profile::WebAssembly,
    });
    let assembly = compiler
        .compile(flapc::CompilationUnit {
            source: flapc::SourceInput {
                name: &source_name,
                contents: &source,
            },
            constants: Some(flapc::SourceInput {
                name: "layout.conf",
                contents: &layout,
            }),
        })
        .unwrap_or_else(|error| panic!("flapc failed to compile the interpreter:\n{error}"));
    let assembly_path = output_directory.join("interpreter.S");
    write_if_changed(&assembly_path, assembly.as_str());

    let mut build = cc::Build::new();
    build.file(&assembly_path);
    build.flag_if_supported("-malign-branch-boundary=32");
    build.flag_if_supported("-malign-branch=fused,jcc");
    build.compile("libwasm_interpreter");
}
