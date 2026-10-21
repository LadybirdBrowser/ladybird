/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![cfg(all(target_arch = "x86_64", not(windows)))]

use libwasm_engine::runtime::{ExternValue, InstanceAddress, Store, Trap, Value};
use std::sync::Arc;

fn instantiate(store: &mut Store, name: &str) -> InstanceAddress {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(name);
    let bytes = std::fs::read(path).unwrap();
    let module = libwasm_engine::compile(&bytes).unwrap();
    store.instantiate(Arc::new(module), &[]).unwrap()
}

fn call(store: &mut Store, instance: InstanceAddress, name: &str, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
    let Some(ExternValue::Function(function)) = store.instance(instance).export(name) else {
        panic!("no function export {name}");
    };
    store.invoke(function, arguments)
}

#[test]
fn runs_control_flow_locals_calls_and_memory() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "basics.wasm");
    assert_eq!(
        call(&mut store, instance, "add", &[Value::I32(40), Value::I32(2)]),
        Ok(vec![Value::I32(42)])
    );
    assert_eq!(
        call(&mut store, instance, "fib", &[Value::I64(20)]),
        Ok(vec![Value::I64(6765)])
    );
    assert_eq!(
        call(&mut store, instance, "sum_to", &[Value::I32(100)]),
        Ok(vec![Value::I32(5050)])
    );
    assert_eq!(
        call(
            &mut store,
            instance,
            "store_and_load",
            &[Value::I32(16), Value::I32(0x1234_5678)]
        ),
        Ok(vec![Value::I32(0x56)])
    );
    for (input, output) in [(0, 100), (1, 200), (2, 300), (7, 300)] {
        assert_eq!(
            call(&mut store, instance, "classify", &[Value::I32(input)]),
            Ok(vec![Value::I32(output)])
        );
    }
    assert_eq!(call(&mut store, instance, "bump", &[]), Ok(vec![Value::I32(15)]));
    assert_eq!(call(&mut store, instance, "bump", &[]), Ok(vec![Value::I32(20)]));
    assert_eq!(
        call(&mut store, instance, "multi", &[]),
        Ok(vec![Value::I32(1), Value::I64(2), Value::I32(3)])
    );
    assert_eq!(
        call(&mut store, instance, "select", &[Value::I32(1)]),
        Ok(vec![Value::I32(11)])
    );
    assert_eq!(
        call(&mut store, instance, "select", &[Value::I32(0)]),
        Ok(vec![Value::I32(22)])
    );
    assert_eq!(
        call(&mut store, instance, "drop_in_block", &[]),
        Ok(vec![Value::I32(3)])
    );
}

#[test]
fn traps() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "basics.wasm");
    assert_eq!(call(&mut store, instance, "trap", &[]), Err(Trap::Unreachable));
    assert_eq!(
        call(&mut store, instance, "out_of_bounds", &[]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "recurse", &[Value::I32(0)]),
        Err(Trap::StackExhausted)
    );
    // The store still works after a trap.
    assert_eq!(
        call(&mut store, instance, "add", &[Value::I32(1), Value::I32(2)]),
        Ok(vec![Value::I32(3)])
    );
}

#[test]
fn checks_bounds_of_other_memories_and_memory64() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "checked_memories.wasm");
    assert_eq!(
        call(
            &mut store,
            instance,
            "store_other",
            &[Value::I32(100), Value::I32(0x1234)]
        ),
        Ok(vec![])
    );
    assert_eq!(
        call(&mut store, instance, "load_other", &[Value::I32(100)]),
        Ok(vec![Value::I32(0x1234)])
    );
    // The store went to the other memory, not to memory 0.
    assert_eq!(
        call(&mut store, instance, "load_small", &[Value::I32(100)]),
        Ok(vec![Value::I32(0)])
    );
    assert_eq!(
        call(&mut store, instance, "load_other", &[Value::I32(65533)]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "store_wide", &[Value::I64(16), Value::I64(-5)]),
        Ok(vec![])
    );
    assert_eq!(
        call(&mut store, instance, "load_wide", &[Value::I64(16)]),
        Ok(vec![Value::I64(-5)])
    );
    assert_eq!(
        call(&mut store, instance, "load_wide", &[Value::I64(1 << 40)]),
        Err(Trap::MemoryOutOfBounds)
    );
    // An address and offset that wrap around 2^64 are out of bounds too.
    assert_eq!(
        call(&mut store, instance, "load_wide", &[Value::I64(-4)]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "splat_other", &[Value::I32(100)]),
        Ok(vec![Value::I32(0x1234)])
    );
    assert_eq!(
        call(&mut store, instance, "splat_other", &[Value::I32(65535)]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "lane_wide", &[Value::I64(24)]),
        Ok(vec![Value::I32(-5)])
    );
    assert_eq!(
        call(&mut store, instance, "lane_wide", &[Value::I64(65533)]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "grow_wide", &[Value::I64(1)]),
        Ok(vec![Value::I64(1)])
    );
    assert_eq!(
        call(&mut store, instance, "grow_wide", &[Value::I64(1 << 40)]),
        Ok(vec![Value::I64(-1)])
    );
}

#[test]
fn catches_exceptions_from_webassembly_and_host_functions() {
    use libwasm_engine::types::{FuncType, ValType};
    let mut store = Store::new();
    let tag = store.allocate_tag(FuncType {
        params: vec![ValType::I32],
        results: vec![],
    });
    let thrower = store.allocate_host_function(
        FuncType {
            params: vec![ValType::I32],
            results: vec![],
        },
        std::rc::Rc::new(move |store: &mut Store, arguments: &[Value]| {
            let Value::I32(value) = arguments[0] else {
                unreachable!();
            };
            let exception = store.allocate_exception(tag, &[u64::from((value * 2) as u32)]);
            Err(Trap::UncaughtException(exception))
        }),
    );
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/exceptions.wasm");
    let module = libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap();
    let instance = store
        .instantiate(
            Arc::new(module),
            &[ExternValue::Tag(tag), ExternValue::Function(thrower)],
        )
        .unwrap();
    assert_eq!(
        call(&mut store, instance, "catch_own", &[Value::I32(7)]),
        Ok(vec![Value::I32(7)])
    );
    assert_eq!(
        call(&mut store, instance, "catch_host", &[Value::I32(7)]),
        Ok(vec![Value::I32(14)])
    );
    let Err(Trap::UncaughtException(exception)) = call(&mut store, instance, "uncaught", &[Value::I32(5)]) else {
        panic!("the exception should leave the interpreter");
    };
    assert_eq!(store.exception(exception).tag, tag);
    assert_eq!(store.exception(exception).payload, &[5]);
}

#[test]
fn waits_on_shared_memory() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "shared_waits.wasm");
    // The location holds 0, not 1.
    assert_eq!(
        call(
            &mut store,
            instance,
            "wait",
            &[Value::I32(0), Value::I32(1), Value::I64(-1)]
        ),
        Ok(vec![Value::I32(1)])
    );
    // Nobody notifies, so the wait times out after a millisecond.
    assert_eq!(
        call(
            &mut store,
            instance,
            "wait",
            &[Value::I32(0), Value::I32(0), Value::I64(1_000_000)]
        ),
        Ok(vec![Value::I32(2)])
    );
    assert_eq!(
        call(&mut store, instance, "notify", &[Value::I32(0)]),
        Ok(vec![Value::I32(0)])
    );
    assert_eq!(
        call(&mut store, instance, "or", &[Value::I32(8), Value::I32(6)]),
        Ok(vec![Value::I32(0)])
    );
    assert_eq!(
        call(&mut store, instance, "or", &[Value::I32(8), Value::I32(9)]),
        Ok(vec![Value::I32(6)])
    );
}

#[test]
fn traps_on_ranges_that_wrap_around() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "wrapping_ranges.wasm");
    let wrapping_start = Value::I64(-256);
    assert_eq!(
        call(&mut store, instance, "fill", &[wrapping_start, Value::I64(512)]),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(
            &mut store,
            instance,
            "copy",
            &[wrapping_start, Value::I64(0), Value::I64(512)]
        ),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(
            &mut store,
            instance,
            "copy",
            &[Value::I64(0), wrapping_start, Value::I64(512)]
        ),
        Err(Trap::MemoryOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "table_fill", &[Value::I64(-2), Value::I64(4)]),
        Err(Trap::TableOutOfBounds)
    );
    assert_eq!(
        call(&mut store, instance, "fill", &[Value::I64(16), Value::I64(16)]),
        Ok(vec![])
    );
}
