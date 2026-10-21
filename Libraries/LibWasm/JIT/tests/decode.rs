/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The decoder reads every body of the spec testsuite's valid modules to its end, with balanced nesting.

use libwasm_jit::decode::{Decoder, nesting_change};
use std::path::PathBuf;

/// The spec testsuite's modules, if the build directory has them.
pub fn spec_modules() -> Option<PathBuf> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let directory = std::env::var_os("LIBWASM_SPEC_MODULES")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("Build/release/Libraries/LibWasm/Tests/Fixtures/SpecTests"));
    directory.is_dir().then_some(directory)
}

#[test]
fn decoded_lengths_match_every_spec_body() {
    let Some(directory) = spec_modules() else {
        eprintln!("no spec testsuite modules; skipped");
        return;
    };
    let mut bodies = 0;
    for entry in std::fs::read_dir(directory).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_none_or(|extension| extension != "wasm") {
            continue;
        }
        let Ok(module) = libwasm_engine::compile(&std::fs::read(&path).unwrap()) else {
            continue;
        };
        for body in &module.code {
            let mut decoder = Decoder::new(&module.bytes, body.code.start as usize, body.code.end as usize);
            let mut depth = 1;
            while depth > 0 {
                assert!(!decoder.is_at_end(), "{}: the body ends early", path.display());
                depth += nesting_change(&decoder.decode());
            }
            assert_eq!(decoder.position(), body.code.end as usize, "{}", path.display());
            bodies += 1;
        }
    }
    assert!(bodies > 1000, "only {bodies} bodies");
}
