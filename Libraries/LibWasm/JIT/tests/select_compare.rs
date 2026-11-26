/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Selects that compare or test their condition's operands themselves, and branches on the flags of the arithmetic
//! that computed a value. Check them against the interpreter, directly and through the runtime.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use std::sync::Arc;

use harness::{CompiledModule, Outcome, TestBinding};
use libwasm_engine::runtime::{ExternValue, Store, Value};

const CONDITIONS: [&str; 10] = [
    "eq", "ne", "lt_s", "lt_u", "gt_s", "gt_u", "le_s", "le_u", "ge_s", "ge_u",
];

/// Each function takes two i64s and returns an i64. `$a`, `$b`: the arguments, `$x`, `$y`: them wrapped to i32.
const BODIES: &[(&str, &str)] = &[
    // An `and`, tested against masks that fit an immediate, that do not, and in a register.
    (
        "and32",
        "(i64.extend_i32_s (select (local.get $x) (local.get $y) (i32.and (local.get $x) (i32.const 0x10))))",
    ),
    (
        "and32_register",
        "(i64.extend_i32_s (select (local.get $x) (local.get $y) (i32.and (local.get $x) (local.get $y))))",
    ),
    (
        "and64",
        "(select (local.get $a) (local.get $b) (i32.wrap_i64 (i64.and (local.get $a) (i64.const 0x10))))",
    ),
    (
        "and64_high",
        "(select (local.get $a) (local.get $b) (i64.ne (i64.and (local.get $a) (i64.const 0x100000000)) (i64.const 0)))",
    ),
    (
        "and64_negative",
        "(select (local.get $a) (local.get $b) (i64.ne (i64.and (local.get $a) (i64.const -0x80000000)) (i64.const 0)))",
    ),
    // An `eqz` of an `and` only it uses, of an `and` something else uses too, and of anything else.
    (
        "eqz_and32",
        "(i64.extend_i32_s (select (local.get $x) (local.get $y) (i32.eqz (i32.and (local.get $x) (i32.const 3)))))",
    ),
    (
        "eqz_and64",
        "(select (local.get $a) (local.get $b) (i64.eqz (i64.and (local.get $a) (i64.const 0x100000000))))",
    ),
    (
        "eqz_and_shared",
        "(local.set $x (i32.and (local.get $x) (local.get $y))) \
        (i64.add (i64.extend_i32_u (local.get $x)) (select (local.get $a) (local.get $b) (i32.eqz (local.get $x))))",
    ),
    (
        "eqz32",
        "(select (local.get $a) (local.get $b) (i32.eqz (local.get $x)))",
    ),
    (
        "eqz64",
        "(i64.extend_i32_s (select (local.get $x) (local.get $y) (i64.eqz (local.get $a))))",
    ),
    // A constant on either side of a comparison.
    (
        "constant_right",
        "(select (local.get $a) (local.get $b) (i32.lt_s (local.get $x) (i32.const 7)))",
    ),
    (
        "constant_left",
        "(select (local.get $a) (local.get $b) (i32.lt_s (i32.const 7) (local.get $x)))",
    ),
    (
        "constant_arm",
        "(select (i64.const 5) (local.get $b) (i64.gt_u (local.get $a) (local.get $b)))",
    ),
    // A condition several selects use, and one something else uses too.
    (
        "shared",
        "(local.set $x (i64.lt_s (local.get $a) (local.get $b))) \
        (i64.add (select (local.get $a) (local.get $b) (local.get $x)) (select (local.get $b) (i64.const 3) (local.get $x)))",
    ),
    (
        "shared_other",
        "(local.set $x (i64.lt_s (local.get $a) (local.get $b))) \
        (i64.add (i64.extend_i32_u (local.get $x)) (select (local.get $a) (local.get $b) (local.get $x)))",
    ),
    // Branches on the flags of a subtraction, an addition with and without its own register, and an `and`.
    (
        "count_down",
        "(local.set $x (i32.add (i32.and (local.get $x) (i32.const 31)) (i32.const 1))) \
        (loop $loop (local.set $a (i64.add (local.get $a) (local.get $b))) \
          (br_if $loop (local.tee $x (i32.sub (local.get $x) (i32.const 1))))) (local.get $a)",
    ),
    (
        "add_branch",
        "(block $out (br_if $out (i64.eqz (local.tee $a (i64.add (local.get $a) (local.get $b))))) \
        (return (i64.const 1))) (local.get $a)",
    ),
    (
        "add_both_live",
        "(block $out (br_if $out (i32.eqz (local.tee $y (i32.add (local.get $x) (local.get $y))))) \
        (return (i64.extend_i32_s (local.get $x)))) (i64.extend_i32_s (local.get $y))",
    ),
    (
        "and_branch",
        "(block $out (br_if $out (i32.and (local.get $x) (local.get $y))) (return (i64.const 1))) (i64.const 2)",
    ),
    (
        "wider_flags",
        "(block $out (br_if $out (i32.eqz (i32.wrap_i64 (local.tee $a (i64.add (local.get $a) (local.get $b)))))) \
        (return (i64.const 1))) (local.get $a)",
    ),
    // Code that sets the flags between the arithmetic that computed a value and the branch on it.
    (
        "stale_flags",
        "(local.set $x (i32.and (local.get $x) (local.get $y))) (local.set $y (i32.shl (local.get $y) (i32.const 1))) \
        (block $out (br_if $out (local.get $x)) (return (i64.extend_i32_s (local.get $y)))) (i64.const 2)",
    ),
    // The flags of the arithmetic right before a loop do not hold where its back edge gets to the header.
    (
        "loop_entry_flags",
        "(local.set $y (i32.and (local.get $y) (i32.const 1))) (local.set $x (i32.add (i32.and (local.get $x) (i32.const 7)) (i32.const 1))) \
        (block $out (loop $loop (br_if $out (local.get $y)) (local.set $a (i64.add (local.get $a) (i64.const 3))) \
          (br_if $loop (local.tee $x (i32.sub (local.get $x) (i32.const 1)))))) (local.get $a)",
    ),
];

const INPUTS: [i64; 10] = [
    0,
    1,
    -1,
    7,
    0x10,
    0x8000_0000,
    0x1_0000_0000,
    0xffff_ffff,
    i64::MIN,
    0x1234_5678_9abc_def0,
];

fn functions() -> Vec<(String, String)> {
    let mut functions = Vec::new();
    for condition in CONDITIONS {
        for ty in ["i32", "i64"] {
            let (lhs, rhs) = if ty == "i32" { ("$x", "$y") } else { ("$a", "$b") };
            let body = format!(
                "(select (local.get $a) (local.get $b) ({ty}.{condition} (local.get {lhs}) (local.get {rhs})))"
            );
            functions.push((format!("{ty}_{condition}"), body));
            let body = format!(
                "(select (i64.const 11) (i64.const 22) ({ty}.{condition} (local.get {lhs}) (local.get {rhs})))"
            );
            functions.push((format!("{ty}_{condition}_const_arms"), body));
        }
    }
    functions.extend(BODIES.iter().map(|(name, body)| (name.to_string(), body.to_string())));
    functions
}

fn script(functions: &[(String, String)]) -> String {
    let mut text = String::from("(module\n");
    for (name, body) in functions {
        text.push_str(&format!(
            "  (func (export \"{name}\") (param $a i64) (param $b i64) (result i64) (local $x i32) (local $y i32) \
             (local.set $x (i32.wrap_i64 (local.get $a))) (local.set $y (i32.wrap_i64 (local.get $b))) {body})\n"
        ));
    }
    text.push_str(")\n");
    text
}

#[test]
fn selects_and_branches_on_flags_run_the_same_compiled() {
    let panics = harness::panics();
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced,verify-ir") };
    let directory = std::env::temp_dir().join(format!("libwasm-select-compare-{}", std::process::id()));
    let functions = functions();
    let Some(json) = harness::script::write_wast(&directory, "select_compare", &script(&functions)) else {
        eprintln!("no wasm-tools; skipped");
        return;
    };
    let json: serde_json::Value = serde_json::from_slice(&std::fs::read(json).unwrap()).unwrap();
    let path = directory.join(json["commands"][0]["filename"].as_str().unwrap());
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
    for stress_registers in [false, true] {
        for runtime in [false, true] {
            let options = libwasm_jit::Options {
                verify_ir: true,
                stress_registers,
                ..Default::default()
            };
            let mut interpreted = Store::new();
            interpreted.disable_jit();
            let mut jit = Store::new();
            let interpreted_instance = interpreted.instantiate(module.clone(), &[]).unwrap();
            let jit_instance = jit.instantiate(module.clone(), &[]).unwrap();
            let compiled = (!runtime).then(|| CompiledModule::compile(module.clone(), options));
            let mut binding = compiled.as_ref().map(|compiled| {
                assert!(compiled.runnable.iter().all(|runnable| *runnable));
                TestBinding::new(compiled, &jit, jit_instance)
            });
            for (name, _) in &functions {
                let Some(ExternValue::Function(reference)) = interpreted.instance(interpreted_instance).export(name)
                else {
                    panic!("no {name} export");
                };
                let Some(ExternValue::Function(function)) = jit.instance(jit_instance).export(name) else {
                    panic!("no {name} export");
                };
                let index = harness::function_index(&jit, function, jit_instance).unwrap();
                for a in INPUTS {
                    for b in INPUTS {
                        let arguments = [Value::I64(a), Value::I64(b)];
                        let expected = interpreted.invoke(reference, &arguments).unwrap();
                        let actual = if let Some((compiled, binding)) = compiled.as_ref().zip(binding.as_mut()) {
                            binding.call(&mut jit, compiled, index, &arguments)
                        } else {
                            Outcome::Values(jit.invoke(function, &arguments).unwrap())
                        };
                        assert_eq!(
                            actual,
                            Outcome::Values(expected),
                            "{name}, a={a:#x}, b={b:#x}, runtime={runtime}, stress_registers={stress_registers}"
                        );
                    }
                }
            }
        }
    }
    let _ = std::fs::remove_dir_all(&directory);
    assert_eq!(
        panics.load(std::sync::atomic::Ordering::Relaxed),
        0,
        "a compile panicked"
    );
}
