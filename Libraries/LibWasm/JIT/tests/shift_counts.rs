/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constant i64 shift counts outside the sign-extended immediate range must use CL without BMI2. Check all shifts
//! and rotations against the interpreter, including counts whose low bits wrap, directly and through the runtime.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use std::fmt::Write as _;
use std::sync::Arc;

use harness::{CompiledModule, Outcome, TestBinding};
use libwasm_engine::runtime::{ExternValue, Store, Value};

const OPERATIONS: [&str; 5] = ["shl", "shr_s", "shr_u", "rotl", "rotr"];
const COUNTS: [u64; 12] = [
    1,
    63,
    64,
    0x7fff_ffff,
    0x8000_0000,
    0xffff_ffff,
    0x1_0000_0000,
    0x1_0000_0001,
    0x1_0000_003f,
    0x8000_0000_0000_0001,
    0xffff_ffff_7fff_ffff,
    u64::MAX,
];
const INPUTS: [i64; 6] = [0, 3, -3, i64::MIN, i64::MAX, 0x1234_5678_9abc_def0];

fn script() -> String {
    let mut text = String::from("(module\n");
    for op in OPERATIONS {
        for (index, count) in COUNTS.iter().enumerate() {
            writeln!(
                text,
                "  (func (export \"{op}_{index}\") (param i64) (result i64) \
                 (i64.{op} (local.get 0) (i64.const {})))",
                *count as i64
            )
            .unwrap();
        }
    }
    text.push_str(")\n");
    text
}

#[test]
fn large_constant_shift_counts_run_the_same_compiled() {
    let panics = harness::panics();
    // SAFETY: Nothing else runs yet.
    unsafe { std::env::set_var("LIBWASM_JIT", "forced,verify-ir") };
    let directory = std::env::temp_dir().join(format!("libwasm-shift-counts-{}", std::process::id()));
    let Some(json) = harness::script::write_wast(&directory, "shift_counts", &script()) else {
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
            for op in OPERATIONS {
                for (index, count) in COUNTS.iter().enumerate() {
                    let name = format!("{op}_{index}");
                    let Some(ExternValue::Function(reference)) =
                        interpreted.instance(interpreted_instance).export(&name)
                    else {
                        panic!("no {name} export");
                    };
                    let Some(ExternValue::Function(function)) = jit.instance(jit_instance).export(&name) else {
                        panic!("no {name} export");
                    };
                    let index = harness::function_index(&jit, function, jit_instance).unwrap();
                    for input in INPUTS {
                        let arguments = [Value::I64(input)];
                        let expected = interpreted.invoke(reference, &arguments).unwrap();
                        let actual = if let Some((compiled, binding)) = compiled.as_ref().zip(binding.as_mut()) {
                            binding.call(&mut jit, compiled, index, &arguments)
                        } else {
                            Outcome::Values(jit.invoke(function, &arguments).unwrap())
                        };
                        assert_eq!(
                            actual,
                            Outcome::Values(expected),
                            "{op}, count={count:#x}, input={input}, runtime={runtime}, stress_registers={stress_registers}"
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
