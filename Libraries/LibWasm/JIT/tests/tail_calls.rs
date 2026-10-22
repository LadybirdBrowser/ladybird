/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tail calls between functions of every pair of parameter counts from 0 to 12, so that the callee's stack arguments
//! take less, as much and more room than the caller's incoming area, with and without a result area and with v128
//! arguments: compiled code against the interpreter, compiled directly and through the runtime in forced mode. A
//! process of its own, since the JIT's options are the process's.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use std::fmt::Write as _;

const MAXIMUM_PARAMETERS: usize = 12;

fn parameters(count: usize, ty: &str) -> String {
    (0..count).map(|_| format!(" {ty}")).collect()
}

/// Callees that combine every argument, so that a misplaced one changes the result, and callers of every arity that
/// tail call each of them.
fn script() -> String {
    let mut text = String::from("(module\n");
    for count in 0..=MAXIMUM_PARAMETERS {
        let mut body = format!("(i64.const {count})");
        for index in 0..count {
            body = format!(
                "(i64.add {body} (i64.mul (local.get {index}) (i64.const {})))",
                index + 3
            );
        }
        writeln!(
            text,
            "  (func $callee_{count} (param{}) (result i64) {body})",
            parameters(count, "i64")
        )
        .unwrap();
        // More results than result registers: a result area, whose pointer the tail call passes on.
        writeln!(
            text,
            "  (func $area_{count} (param{}) (result i64 i64 i64) {body} (i64.const 7) (i64.const 8))",
            parameters(count, "i64")
        )
        .unwrap();
        // A v128 argument first, which takes two slots.
        writeln!(
            text,
            "  (func $vector_{count} (param v128{}) (result i64) (i64.add (i64x2.extract_lane 1 (local.get 0)) \
             (call $callee_{count}{})))",
            parameters(count, "i64"),
            (1..=count)
                .map(|index| format!(" (local.get {index})"))
                .collect::<String>()
        )
        .unwrap();
    }
    for caller in 0..=MAXIMUM_PARAMETERS {
        for callee in 0..=MAXIMUM_PARAMETERS {
            let arguments: String = (0..callee)
                .map(|index| format!(" (i64.const {})", 1000 * (index as i64 + 1) + caller as i64))
                .collect();
            writeln!(
                text,
                "  (func (export \"plain_{caller}_{callee}\") (param{}) (result i64) (return_call $callee_{callee}{arguments}))",
                parameters(caller, "i64")
            )
            .unwrap();
            writeln!(
                text,
                "  (func (export \"area_{caller}_{callee}\") (param{}) (result i64 i64 i64) (return_call $area_{callee}{arguments}))",
                parameters(caller, "i64")
            )
            .unwrap();
            writeln!(
                text,
                "  (func (export \"vector_{caller}_{callee}\") (param{}) (result i64) (return_call $vector_{callee} \
                 (v128.const i64x2 5 {caller}){arguments}))",
                parameters(caller, "i64")
            )
            .unwrap();
        }
    }
    text.push_str(")\n");
    for caller in 0..=MAXIMUM_PARAMETERS {
        let arguments: String = (0..caller).map(|index| format!(" (i64.const {})", index + 1)).collect();
        for callee in 0..=MAXIMUM_PARAMETERS {
            for kind in ["plain", "area", "vector"] {
                writeln!(text, "(invoke \"{kind}_{caller}_{callee}\"{arguments})").unwrap();
            }
        }
    }
    text
}

#[test]
fn tail_calls_of_every_arity_run_the_same_compiled() {
    let panics = harness::panics();
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced,verify-ir") };
    let directory = std::env::temp_dir().join(format!("libwasm-tail-calls-{}", std::process::id()));
    let Some(json) = harness::script::write_wast(&directory, "tail_calls", &script()) else {
        eprintln!("no wasm-tools; skipped");
        return;
    };
    let paths = [json.to_string_lossy().into_owned()];
    let expected_cases = 3 * (MAXIMUM_PARAMETERS + 1) * (MAXIMUM_PARAMETERS + 1);
    for (stress_registers, runtime) in [(false, false), (true, false), (false, true)] {
        let options = libwasm_jit::Options {
            verify_ir: true,
            stress_registers,
            ..Default::default()
        };
        let totals = harness::script::run(&paths, false, options, runtime);
        assert_eq!(
            totals.cases, expected_cases,
            "not every call ran compiled (runtime: {runtime})"
        );
        assert_eq!(
            totals.failures, 0,
            "stress registers: {stress_registers}, runtime: {runtime}"
        );
    }
    let _ = std::fs::remove_dir_all(&directory);
    assert_eq!(
        panics.load(std::sync::atomic::Ordering::Relaxed),
        0,
        "a compile panicked"
    );
}
