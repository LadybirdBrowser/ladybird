/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A loop whose only call is a `return_call_ref` of a null reference (`tail-call-ref-loop.wat` of the engine's
//! tests), compiled in full with loop invariant code motion, as the runtime's tier-up compiles do but its OSR compiles
//! do not. Reading the callee's instance out of the null reference's header anywhere its null check does not guard
//! faults: the loop must run where it does not call, and the call must trap as null.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use harness::{CompiledModule, Outcome, TestBinding};
use libwasm_engine::abi::TRAP_NULL_REFERENCE;
use libwasm_engine::runtime::{ExternValue, Store, Value};
use std::sync::Arc;

#[test]
fn a_tail_call_ref_in_a_loop_reads_its_callee_behind_its_null_check() {
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../Tests/Fixtures/Modules/tail-call-ref-loop.wasm");
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
    for stress_registers in [false, true] {
        let options = libwasm_jit::Options {
            verify_ir: true,
            stress_registers,
            ..Default::default()
        };
        assert!(!options.no_licm);
        let mut store = Store::new();
        let instance = store.instantiate(module.clone(), &[]).unwrap();
        let Some(ExternValue::Function(run)) = store.instance(instance).export("run") else {
            panic!("no run export");
        };
        let run = harness::function_index(&store, run, instance).unwrap();
        let compiled = CompiledModule::compile(module.clone(), options);
        assert!(compiled.runnable[run as usize], "{:?}", compiled.failures[run as usize]);
        let mut binding = TestBinding::new(&compiled, &store, instance);
        let mut call = |call: i32| binding.call(&mut store, &compiled, run, &[Value::I32(call), Value::I32(10)]);
        assert_eq!(call(0), Outcome::Values(vec![Value::I32(7)]));
        assert_eq!(call(1), Outcome::Trap(TRAP_NULL_REFERENCE));
    }
}
