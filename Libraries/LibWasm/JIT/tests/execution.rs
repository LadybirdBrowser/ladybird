/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiled code against the interpreter, over the spec testsuite's scripts (see `harness::script`).

#![cfg(all(target_arch = "x86_64", target_os = "linux"))]

mod harness;

use std::path::PathBuf;

#[test]
fn spec_scripts_run_the_same_compiled() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let directory = std::env::var_os("LIBWASM_SPEC_MODULES")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("Build/release/Libraries/LibWasm/Tests/Fixtures/SpecTests"));
    if !directory.is_dir() {
        eprintln!("no spec testsuite scripts; skipped");
        return;
    }
    for stress_registers in [false, true] {
        let options = libwasm_jit::Options {
            verify_ir: true,
            stress_registers,
            ..Default::default()
        };
        let totals = harness::script::run(&[directory.to_string_lossy().into_owned()], false, options, false);
        assert!(totals.cases > 10_000, "only {} cases ran compiled", totals.cases);
        assert_eq!(totals.failures, 0);
    }
}
