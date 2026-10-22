/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs spec testsuite scripts with calls into compiled functions running compiled code, and compares everything
//! with the interpreter (see `tests/harness/script.rs`).
//!
//! ```text
//! spec_check [--verbose] [--stress-registers] [--no-licm] [--runtime] [--dump FILE] DIRECTORY|SCRIPT.json...
//! ```
//!
//! `--dump FILE` writes what the interpreter did in every action to FILE, which should be the same on every
//! architecture: with `--runtime` and `LIBWASM_JIT=off`, comparing the files of two architectures checks one's
//! interpreter against the other's.

#[path = "../tests/harness/mod.rs"]
mod harness;

fn main() {
    harness::run_main(checked_main);
}

fn checked_main() {
    let mut verbose = false;
    let mut runtime = false;
    let mut paths = Vec::new();
    let mut dump_path = None;
    let mut arguments = std::env::args().skip(1);
    let mut options = libwasm_jit::Options {
        verify_ir: true,
        ..Default::default()
    };
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--dump" => dump_path = arguments.next(),
            "--verbose" => verbose = true,
            "--stress-registers" => options.stress_registers = true,
            "--no-licm" => options.no_licm = true,
            "--runtime" => runtime = true,
            _ => paths.push(argument),
        }
    }
    if runtime && std::env::var_os("LIBWASM_JIT").is_none() {
        // SAFETY: Nothing else runs yet.
        unsafe { std::env::set_var("LIBWASM_JIT", "eager") };
    }
    let mut dump = dump_path.map(|path| {
        std::io::BufWriter::new(std::fs::File::create(&path).unwrap_or_else(|error| panic!("create {path}: {error}")))
    });
    let totals = harness::script::run_with_dump(
        &paths,
        verbose,
        options,
        runtime,
        dump.as_mut().map(|dump| dump as &mut dyn std::io::Write),
    );
    if let Some(mut dump) = dump {
        std::io::Write::flush(&mut dump).expect("write the dump");
    }
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
