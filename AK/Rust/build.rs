/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles AK/MathFunctions.cpp so that unit tests link without AK. A CMake build links AK and skips this, as does a
//! build without the LLVM libc headers, which only cargo check and clippy can use.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=LADYBIRD_LINKS_AK");
    println!("cargo:rerun-if-env-changed=LLVM_LIBC_INCLUDE_DIR");
    if env::var_os("LADYBIRD_LINKS_AK").is_some() {
        return;
    }

    let manifest_directory = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let source_root = manifest_directory.join("../..");
    let source = source_root.join("AK/MathFunctions.cpp");
    let Some(llvm_libc) = llvm_libc_include_directory(&source_root) else {
        println!(
            "cargo:warning=LLVM libc is installed by vcpkg: configure a CMake build, or set LLVM_LIBC_INCLUDE_DIR"
        );
        return;
    };

    println!("cargo:rerun-if-changed={}", source.display());
    println!(
        "cargo:rerun-if-changed={}",
        source_root.join("AK/MathFunctions.h").display()
    );
    println!("cargo:rerun-if-changed={}", llvm_libc.display());
    println!("cargo:rerun-if-env-changed=MACOSX_DEPLOYMENT_TARGET");

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .file(&source)
        .include(&source_root)
        .include(&llvm_libc)
        .warnings(false)
        .flag_if_supported("-fvisibility=hidden")
        .flag_if_supported("-fno-exceptions")
        .flag_if_supported("-fno-rtti");
    if env::var("CARGO_CFG_TARGET_OS").is_ok_and(|os| os == "macos")
        && let Some(deployment_target) = rust_macos_deployment_target()
    {
        build.flag(format!("-mmacosx-version-min={deployment_target}"));
    }
    build.compile("ladybird_math");
}

/// The headers of LLVM libc that vcpkg installs. A CMake build passes their directory, and a build on its own takes them
/// from the vcpkg tree of a build under Build/.
fn llvm_libc_include_directory(source_root: &Path) -> Option<PathBuf> {
    if let Some(directory) = env::var_os("LLVM_LIBC_INCLUDE_DIR") {
        return Some(PathBuf::from(directory));
    }
    let builds = fs::read_dir(source_root.join("Build")).into_iter().flatten().flatten();
    builds
        .flat_map(|build| {
            fs::read_dir(build.path().join("vcpkg_installed"))
                .into_iter()
                .flatten()
                .flatten()
        })
        .map(|triplet| triplet.path().join("include/llvm-libc"))
        .find(|directory| directory.join("shared/math.h").is_file())
}

/// The minimum macOS version rustc builds the crate's own code for, which the compiled C++ has to match.
fn rust_macos_deployment_target() -> Option<String> {
    if let Ok(target) = env::var("MACOSX_DEPLOYMENT_TARGET") {
        return Some(target);
    }
    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".to_string());
    let target = env::var("TARGET").ok()?;
    let output = std::process::Command::new(rustc)
        .args(["--print", "deployment-target", "--target", &target])
        .output()
        .ok()?;
    String::from_utf8(output.stdout)
        .ok()?
        .trim()
        .strip_prefix("MACOSX_DEPLOYMENT_TARGET=")
        .map(str::to_string)
}
