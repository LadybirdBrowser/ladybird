/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::path::{Path, PathBuf};

fn fixtures() -> Vec<PathBuf> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let mut paths = Vec::new();
    for directory in [
        "Libraries/LibWasm/Tests/Fixtures/Modules",
        "Libraries/LibWasm/Tests/CI",
        "Tests/LibWasm/Fixtures",
    ] {
        for entry in std::fs::read_dir(root.join(directory)).unwrap() {
            let path = entry.unwrap().path();
            if path.extension().is_some_and(|extension| extension == "wasm") {
                paths.push(path);
            }
        }
    }
    paths.sort();
    paths
}

#[test]
fn validates_the_fixture_modules() {
    let paths = fixtures();
    assert!(paths.len() >= 20);
    for path in paths {
        let bytes = std::fs::read(&path).unwrap();
        let result = libwasm_engine::compile(&bytes);
        let name = path.file_name().unwrap().to_str().unwrap();
        if name == "atomics-misaligned.wasm" {
            assert_eq!(
                result.unwrap_err(),
                libwasm_engine::Error::invalid("atomic memory op alignment must be equal to natural")
            );
        } else {
            result.unwrap_or_else(|error| panic!("{name}: {error}"));
        }
    }
}
