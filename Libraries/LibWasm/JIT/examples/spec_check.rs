/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs spec testsuite scripts with calls into compiled functions running compiled code, and compares everything
//! with the interpreter (see `tests/harness/script.rs`).
//!
//! ```text
//! spec_check [--verbose] [--stress-registers] [--no-licm] [--gvn] [--runtime] DIRECTORY|SCRIPT.json...
//! ```

#[path = "../tests/harness/mod.rs"]
mod harness;

fn main() {
    let mut verbose = false;
    let mut runtime = false;
    let mut paths = Vec::new();
    let mut options = libwasm_jit::Options {
        verify_ir: true,
        ..Default::default()
    };
    for argument in std::env::args().skip(1) {
        match argument.as_str() {
            "--verbose" => verbose = true,
            "--stress-registers" => options.stress_registers = true,
            "--no-licm" => options.no_licm = true,
            "--gvn" => options.gvn = true,
            "--runtime" => runtime = true,
            _ => paths.push(argument),
        }
    }
    if runtime && std::env::var_os("LIBWASM_JIT").is_none() {
        // SAFETY: Nothing else runs yet.
        unsafe { std::env::set_var("LIBWASM_JIT", "eager") };
    }
    let totals = harness::script::run(&paths, verbose, options, runtime);
    println!(
        "{} scripts, {} modules, {} functions: {} compiled, {} runnable, {} exercised; {} cases, {} failures, {} reached uncompiled code",
        totals.scripts,
        totals.modules,
        totals.functions,
        totals.compiled,
        totals.runnable,
        totals.exercised,
        totals.cases,
        totals.failures,
        totals.not_compiled
    );
    if totals.failures > 0 {
        std::process::exit(1);
    }
}
