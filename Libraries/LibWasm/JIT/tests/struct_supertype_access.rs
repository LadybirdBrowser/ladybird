/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Struct accesses through a supertype that is not final, compiled in full, on an object of a subtype whose data is
//! outside the heap.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

mod harness;

use harness::{CompiledModule, Outcome, TestBinding};
use libwasm_engine::runtime::{ExternValue, Store, Value};
use std::sync::Arc;

#[test]
fn struct_accesses_through_a_supertype_that_is_not_final_reach_the_data_of_a_subtype() {
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../Tests/Fixtures/Modules/struct-supertype-access.wasm");
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap());
    for stress_registers in [false, true] {
        let options = libwasm_jit::Options {
            verify_ir: true,
            stress_registers,
            ..Default::default()
        };
        let mut store = Store::new();
        let instance = store.instantiate(module.clone(), &[]).unwrap();
        let function = |store: &Store, name: &str| {
            let Some(ExternValue::Function(address)) = store.instance(instance).export(name) else {
                panic!("no {name} export");
            };
            harness::function_index(store, address, instance).unwrap()
        };
        let (allocate, get, set) = (
            function(&store, "allocate"),
            function(&store, "get"),
            function(&store, "set"),
        );
        let compiled = CompiledModule::compile(module.clone(), options);
        for index in [allocate, get, set] {
            assert!(
                compiled.runnable[index as usize],
                "{:?}",
                compiled.failures[index as usize]
            );
        }
        let mut binding = TestBinding::new(&compiled, &store, instance);
        assert_eq!(
            binding.call(&mut store, &compiled, allocate, &[]),
            Outcome::Values(vec![])
        );
        assert_eq!(
            binding.call(&mut store, &compiled, get, &[Value::I32(10)]),
            Outcome::Values(vec![Value::I64(100)])
        );
        assert_eq!(
            binding.call(&mut store, &compiled, set, &[Value::I64(100), Value::I32(10)]),
            Outcome::Values(vec![Value::I64(406)])
        );
        assert_eq!(
            binding.call(&mut store, &compiled, get, &[Value::I32(1)]),
            Outcome::Values(vec![Value::I64(406)])
        );
    }
}
