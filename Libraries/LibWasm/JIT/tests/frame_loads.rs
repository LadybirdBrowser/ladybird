/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Functions continued compiled at a loop of an interpreter frame (OSR) read their locals and operand stack from the
//! frame: locals of every type far from fp, where the offsets of loads of 4 and 16 bytes reach less far than those
//! of 8 bytes, and v128 values on the operand stack. A process of its own, since the JIT's options are the process's.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use std::fmt::Write as _;

/// Locals before the ones the loop reads, so that their offsets from fp are large: 8 bytes each.
const PADDING_LOCALS: usize = 3000;

fn script() -> String {
    let mut text = String::from("(module\n");
    // The loop reads an i32, an f32, an i64, an f64 and a v128 local beyond the padding, and keeps v128 values on
    // the operand stack across it.
    writeln!(
        text,
        "  (func (export \"far_locals\") (param i32) (result i32)\n    (local{})\n    (local $i i32) (local $f \
         f32) (local $l i64) (local $d f64) (local $v v128)",
        " i32".repeat(PADDING_LOCALS)
    )
    .unwrap();
    text.push_str(
        "    (local.set $i (local.get 0)) (local.set $f (f32.const 1.5)) (local.set $l (i64.const 7))
    (local.set $d (f64.const 2.25)) (local.set $v (v128.const i32x4 1 2 3 4))
    (local.set 2999 (i32.const 11))
    (v128.const i32x4 10 20 30 40) (v128.const i32x4 5 6 7 8)
    (loop $again (param v128 v128) (result v128 v128)
      (local.set $v (i32x4.add (local.get $v) (v128.const i32x4 1 1 1 1)))
      (local.set $f (f32.add (local.get $f) (f32.const 1)))
      (local.set $l (i64.add (local.get $l) (i64.const 3)))
      (local.set $d (f64.mul (local.get $d) (f64.const 2)))
      (local.set $i (i32.sub (local.get $i) (i32.const 1)))
      (br_if $again (local.get $i)))
    (i32x4.add) (i32x4.add (local.get $v)) (i32x4.extract_lane 3)
    (i32.add (i32.trunc_f32_s (local.get $f)))
    (i32.add (i32.wrap_i64 (local.get $l)))
    (i32.add (i32.trunc_f64_s (local.get $d)))
    (i32.add (local.get 2999)))\n",
    );
    text.push_str(")\n");
    for count in [1, 2, 5] {
        writeln!(text, "(invoke \"far_locals\" (i32.const {count}))").unwrap();
    }
    text
}

#[test]
fn osr_reads_far_locals_and_vectors_of_the_frame() {
    let panics = harness::panics();
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "stress-osr,verify-ir") };
    let directory = std::env::temp_dir().join(format!("libwasm-frame-loads-{}", std::process::id()));
    let Some(json) = harness::script::write_wast(&directory, "frame_loads", &script()) else {
        eprintln!("no wasm-tools; skipped");
        return;
    };
    let options = libwasm_jit::Options {
        verify_ir: true,
        ..Default::default()
    };
    let totals = harness::script::run(&[json.to_string_lossy().into_owned()], false, options, true);
    assert_eq!(totals.cases, 3);
    assert_eq!(totals.failures, 0);
    let _ = std::fs::remove_dir_all(&directory);
    assert_eq!(
        panics.load(std::sync::atomic::Ordering::Relaxed),
        0,
        "a compile panicked"
    );
}
