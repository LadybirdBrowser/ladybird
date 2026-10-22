/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! `i8x16.shuffle` of one vector and of two, with lanes of either operand, in every pattern code generation picks
//! instructions for: compiled code against the interpreter, directly (with global value numbering too, which may
//! make both operands one value after building) and through the runtime in forced mode. A process of its own, since
//! the JIT's options are the process's.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use std::fmt::Write as _;

/// Lane patterns: whole 32-bit lanes, rotations, interleavings, and byte permutations without structure.
fn patterns() -> Vec<[u8; 16]> {
    let mut patterns = Vec::new();
    let dwords = |d: [u8; 4]| {
        let mut lanes = [0u8; 16];
        for (index, lane) in lanes.iter_mut().enumerate() {
            *lane = 4 * d[index / 4] + (index % 4) as u8;
        }
        lanes
    };
    for d in [
        [0, 1, 2, 3],
        [1, 0, 3, 2],
        [3, 2, 1, 0],
        [0, 4, 1, 5],
        [2, 6, 3, 7],
        [4, 0, 5, 1],
        [6, 2, 7, 3],
        [0, 1, 6, 7],
        [4, 5, 2, 3],
        [0, 5, 2, 7],
        [7, 6, 5, 4],
    ] {
        patterns.push(dwords(d));
    }
    for n in [1u8, 3, 8, 15, 17, 24, 31] {
        patterns.push(std::array::from_fn(|index| (index as u8 + n) & 31));
    }
    let mut state = 0x2545_f491u32;
    for _ in 0..8 {
        patterns.push(std::array::from_fn(|_| {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            (state % 32) as u8
        }));
    }
    patterns
}

fn script() -> String {
    let mut text = String::from("(module\n");
    let patterns = patterns();
    for (index, lanes) in patterns.iter().enumerate() {
        let lanes: String = lanes.iter().map(|lane| format!(" {lane}")).collect();
        for (name, operands) in [
            ("both", "(local.get 0) (local.get 1)"),
            ("first", "(local.get 0) (local.get 0)"),
            ("second", "(local.get 1) (local.get 1)"),
            // The same value twice, after a merge that the builder sees through only once it is done.
            (
                "merged",
                "(local.get 0) (if (result v128) (local.get 2) (then (local.get 0)) (else (local.get 0)))",
            ),
        ] {
            writeln!(
                text,
                "  (func (export \"{name}_{index}\") (param v128 v128 i32) (result v128) (i8x16.shuffle{lanes} {operands}))"
            )
            .unwrap();
        }
    }
    text.push_str(")\n");
    for index in 0..patterns.len() {
        for name in ["both", "first", "second", "merged"] {
            writeln!(
                text,
                "(invoke \"{name}_{index}\" (v128.const i8x16 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15) \
                 (v128.const i8x16 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115) (i32.const 1))"
            )
            .unwrap();
        }
    }
    text
}

#[test]
fn shuffles_run_the_same_compiled() {
    let panics = harness::panics();
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced,verify-ir") };
    let directory = std::env::temp_dir().join(format!("libwasm-shuffles-{}", std::process::id()));
    let Some(json) = harness::script::write_wast(&directory, "shuffles", &script()) else {
        eprintln!("no wasm-tools; skipped");
        return;
    };
    let paths = [json.to_string_lossy().into_owned()];
    let expected_cases = 4 * patterns().len();
    for runtime in [false, true] {
        let options = libwasm_jit::Options {
            verify_ir: true,
            ..Default::default()
        };
        let totals = harness::script::run(&paths, false, options, runtime);
        assert_eq!(
            totals.cases, expected_cases,
            "not every call ran compiled (runtime: {runtime})"
        );
        assert_eq!(totals.failures, 0, "runtime: {runtime}");
    }
    let _ = std::fs::remove_dir_all(&directory);
    assert_eq!(
        panics.load(std::sync::atomic::Ordering::Relaxed),
        0,
        "a compile panicked"
    );
}
