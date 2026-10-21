/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

//! Out-of-bounds accesses trap while another thread reserves, grows, and releases memories, which changes the set of
//! reservations the fault handler looks addresses up in.

use libwasm_engine::runtime::{ExternValue, Store, Trap, Value};
use libwasm_engine::types::{AddressType, Limits, MemoryType};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

#[test]
fn out_of_bounds_accesses_trap_while_memories_come_and_go() {
    let stop = Arc::new(AtomicBool::new(false));
    let churn = {
        let stop = stop.clone();
        std::thread::spawn(move || {
            let mut rounds = 0u64;
            while !stop.load(Ordering::Relaxed) {
                let mut store = Store::new();
                for _ in 0..8 {
                    let memory = store
                        .allocate_memory(MemoryType {
                            limits: Limits {
                                min: 1,
                                max: Some(4),
                                address_type: AddressType::I32,
                                shared: false,
                            },
                        })
                        .expect("the memory is allocated");
                    let _ = store.memory_mut(memory).grow(1);
                }
                // Dropping the store releases its memories.
                drop(store);
                rounds += 1;
            }
            rounds
        })
    };

    let bytes =
        std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/out-of-bounds.wasm"))
            .unwrap();
    let module = Arc::new(libwasm_engine::compile(&bytes).unwrap());
    let mut store = Store::new();
    let instance = store.instantiate(module, &[]).unwrap();
    let export = |name: &str| match store.instance(instance).export(name) {
        Some(ExternValue::Function(function)) => function,
        _ => panic!("no function export {name}"),
    };
    let (load, store_function) = (export("load"), export("store"));
    for iteration in 0..20_000u32 {
        let address = 65536 + (iteration % 1024) * 4096;
        assert_eq!(
            store.invoke(load, &[Value::I32(address as i32)]),
            Err(Trap::MemoryOutOfBounds)
        );
        assert_eq!(
            store.invoke(store_function, &[Value::I32(iteration as i32)]),
            Err(Trap::MemoryOutOfBounds)
        );
    }
    stop.store(true, Ordering::Relaxed);
    let rounds = churn.join().unwrap();
    assert!(rounds > 0);
}
