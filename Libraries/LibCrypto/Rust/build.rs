/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The libraries that link this crate link OpenSSL, but a crate's unit tests link no such library. With the
//! `link-openssl` feature, this links OpenSSL's libcrypto itself: the one the build system passes in
//! LADYBIRD_OPENSSL_CRYPTO_LIBRARY, or else one that a build directory has, or else the system's.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo::rerun-if-env-changed=LADYBIRD_OPENSSL_CRYPTO_LIBRARY");
    if env::var_os("CARGO_FEATURE_LINK_OPENSSL").is_none() {
        return;
    }

    let library = env::var_os("LADYBIRD_OPENSSL_CRYPTO_LIBRARY")
        .map(PathBuf::from)
        .or_else(find_in_build_directories);
    let Some(library) = library else {
        println!("cargo::rustc-link-lib=dylib=crypto");
        return;
    };
    println!("cargo::rerun-if-changed={}", library.display());

    let windows = env::var("CARGO_CFG_WINDOWS").is_ok();
    let file_name = library
        .file_name()
        .and_then(|name| name.to_str())
        .expect("library file name");
    let stem = file_name.split('.').next().expect("library name");
    let name = if windows {
        stem
    } else {
        stem.strip_prefix("lib").unwrap_or(stem)
    };
    let kind = if file_name.ends_with(".a") { "static" } else { "dylib" };

    // NB: Cargo only lets binaries it runs find shared libraries that are in its target directory, so the library
    //     and the files it loads from at runtime are linked or copied there.
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    let directory = library.parent().expect("library directory");
    let mut runtime_directories = vec![directory.to_path_buf()];
    if windows && let Some(prefix) = directory.parent() {
        runtime_directories.push(prefix.join("bin"));
    }
    for runtime_directory in runtime_directories {
        let Ok(entries) = fs::read_dir(&runtime_directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let entry_name = entry.file_name();
            let Some(entry_name) = entry_name.to_str() else {
                continue;
            };
            if entry_name.starts_with(&format!("{stem}.")) || (windows && entry_name.starts_with(&format!("{stem}-"))) {
                place(&entry.path(), &out_dir.join(entry_name));
            }
        }
    }
    // The linker looks for the library by its plain name, which the one picked may not have, such as libcrypto.so.3
    // with no libcrypto.so next to it.
    if !windows && kind == "dylib" {
        let extension = if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
            "dylib"
        } else {
            "so"
        };
        place(&library, &out_dir.join(format!("{stem}.{extension}")));
    }
    println!("cargo::rustc-link-search=native={}", out_dir.display());
    println!("cargo::rustc-link-lib={kind}={name}");
}

// The vcpkg triplets that hold libraries for the target, such as x64-linux-dynamic for x86_64 Linux.
fn is_triplet_for_target(triplet: &str) -> bool {
    let arch = match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
        Ok("x86_64") => "x64",
        Ok("aarch64") => "arm64",
        Ok("x86") => "x86",
        Ok("arm") => "arm",
        _ => return false,
    };
    let os = match env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("macos") => "osx".to_owned(),
        Ok(os) => os.to_owned(),
        Err(_) => return false,
    };
    triplet == format!("{arch}-{os}") || triplet.starts_with(&format!("{arch}-{os}-"))
}

// The libcrypto of the vcpkg dependencies of a build directory for the target, such as Build/release.
fn find_in_build_directories() -> Option<PathBuf> {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .canonicalize()
        .ok()?;
    let mut candidates = Vec::new();
    for build in fs::read_dir(workspace.join("Build")).ok()?.flatten() {
        let Ok(triplets) = fs::read_dir(build.path().join("vcpkg_installed")) else {
            continue;
        };
        for triplet in triplets.flatten() {
            if !triplet.file_name().to_str().is_some_and(is_triplet_for_target) {
                continue;
            }
            for name in ["libcrypto.so", "libcrypto.dylib", "libcrypto.a", "libcrypto.lib"] {
                let path = triplet.path().join("lib").join(name);
                if path.exists() {
                    candidates.push(path);
                }
            }
        }
    }
    candidates.sort();
    candidates.into_iter().next()
}

fn place(source: &Path, destination: &Path) {
    let _ = fs::remove_file(destination);
    #[cfg(unix)]
    let placed = std::os::unix::fs::symlink(source, destination);
    #[cfg(not(unix))]
    let placed = fs::copy(source, destination).map(|_| ());
    placed.unwrap_or_else(|error| panic!("Placing {} in {}: {error}", source.display(), destination.display()));
}
