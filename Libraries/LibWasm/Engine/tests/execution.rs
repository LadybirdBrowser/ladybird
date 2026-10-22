/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

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
fn recurses_with_frames_larger_than_a_page() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "big_frames.wasm");
    // Each frame reaches more than a page below the previous one, which Windows only commits if the interpreter probes
    // the pages in between.
    assert_eq!(
        call(&mut store, instance, "deep", &[Value::I32(100)]),
        Ok(vec![Value::I32(101)])
    );
    assert_eq!(
        call(&mut store, instance, "deep", &[Value::I32(1_000_000)]),
        Err(Trap::StackExhausted)
    );
    assert_eq!(
        call(&mut store, instance, "deep", &[Value::I32(10)]),
        Ok(vec![Value::I32(11)])
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
fn checks_values_from_the_host_against_function_types() {
    use libwasm_engine::types::{FuncType, HeapType, RefType, ValType};
    let mut store = Store::new();
    let any = ValType::Ref(RefType {
        nullable: true,
        heap: HeapType::Any,
    });
    let reference = store.allocate_host_function(
        FuncType {
            params: vec![],
            results: vec![any],
        },
        std::rc::Rc::new(|_: &mut Store, _: &[Value]| Ok(vec![Value::I64(0x4141)])),
    );
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/host_values.wasm");
    let module = libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap();
    let instance = store
        .instantiate(Arc::new(module), &[ExternValue::Function(reference)])
        .unwrap();
    let is_host_trap = |result: Result<Vec<Value>, Trap>| matches!(result, Err(Trap::Host(_)));
    let traps =
        |store: &mut Store, name: &str, arguments: &[Value]| is_host_trap(call(store, instance, name, arguments));

    assert_eq!(
        call(&mut store, instance, "is_null", &[Value::Ref(0)]),
        Ok(vec![Value::I32(1)])
    );
    assert!(traps(&mut store, "is_null", &[Value::I64(0x4141)]));
    assert!(traps(&mut store, "take_non_null", &[Value::Ref(0)]));
    assert!(traps(&mut store, "host_is_null", &[]));
    assert!(is_host_trap(store.invoke(reference, &[])));

    let takes_an_argument = store.allocate_host_function(
        FuncType {
            params: vec![ValType::I32],
            results: vec![],
        },
        std::rc::Rc::new(|_: &mut Store, arguments: &[Value]| {
            assert_eq!(arguments.len(), 1);
            Ok(vec![])
        }),
    );
    assert!(is_host_trap(store.invoke(takes_an_argument, &[])));
    assert!(is_host_trap(store.invoke(takes_an_argument, &[Value::I64(1)])));
    assert_eq!(store.invoke(takes_an_argument, &[Value::I32(1)]), Ok(vec![]));
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
fn wait_traps_in_an_agent_that_cannot_block() {
    let mut store = Store::new();
    store.can_block = false;
    let instance = instantiate(&mut store, "shared_waits.wasm");
    // Even where the value differs and nothing would block.
    for expected in [0, 1] {
        assert!(matches!(
            call(
                &mut store,
                instance,
                "wait",
                &[Value::I32(0), Value::I32(expected), Value::I64(0)]
            ),
            Err(Trap::Host(_))
        ));
    }
    assert_eq!(
        call(&mut store, instance, "notify", &[Value::I32(0)]),
        Ok(vec![Value::I32(0)])
    );
}

#[test]
fn active_element_segments_point_nowhere_after_instantiation() {
    let mut store = Store::new();
    let instance = instantiate(&mut store, "element_segments.wasm");
    // The active segment's values went into the table and the segment was dropped: nothing points at its storage.
    let segments = &store.instance(instance).element_segments;
    assert!(segments[0].contents.is_null());
    assert_eq!(segments[0].length, 0);
    assert!(!segments[1].contents.is_null());
    assert_eq!(segments[1].length, 2);
    assert_eq!(
        call(&mut store, instance, "call", &[Value::I32(1)]),
        Ok(vec![Value::I32(7)])
    );
    assert_eq!(call(&mut store, instance, "init", &[Value::I32(2)]), Ok(vec![]));
    assert_eq!(
        call(&mut store, instance, "call", &[Value::I32(3)]),
        Ok(vec![Value::I32(7)])
    );
}

#[test]
fn constant_arrays_too_large_for_memory_trap() {
    for name in ["huge_constant_arrays.wasm", "huge_constant_arrays_new.wasm"] {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures")
            .join(name);
        let module = libwasm_engine::compile(&std::fs::read(path).unwrap()).unwrap();
        let mut store = Store::new();
        // The length is 2^32 - 1: building the elements first aborted the process.
        assert!(store.instantiate(Arc::new(module), &[]).is_err(), "{name}");
    }
}

/// memory.atomic.wait waits where Atomics.wait does, on the operating system's waiter list for the address, so that
/// a wake of the address from outside WebAssembly (here a FUTEX_WAKE, as Atomics.notify does) ends it.
#[cfg(target_os = "linux")]
#[test]
fn waits_end_with_wakes_from_outside_webassembly() {
    let (sender, receiver) = std::sync::mpsc::channel();
    let waiter = std::thread::spawn(move || {
        let mut store = Store::new();
        let instance = instantiate(&mut store, "shared_memory_export.wasm");
        let Some(ExternValue::Memory(memory)) = store.instance(instance).export("memory") else {
            panic!("no memory export");
        };
        sender.send(store.memory(memory).bytes().as_ptr() as usize).unwrap();
        // Ten seconds, which only a wake cuts short.
        call(
            &mut store,
            instance,
            "wait",
            &[Value::I32(8), Value::I32(0), Value::I64(10_000_000_000)],
        )
    });
    let address = receiver.recv().unwrap() + 8;
    let start = std::time::Instant::now();
    loop {
        // Until the wait has started: a wake before it wakes nobody.
        let woken = unsafe {
            libc::syscall(
                libc::SYS_futex,
                address as *mut u32,
                libc::FUTEX_WAKE,
                1,
                std::ptr::null::<libc::timespec>(),
                std::ptr::null::<u32>(),
                0,
            )
        };
        if woken == 1 {
            break;
        }
        assert!(
            start.elapsed() < std::time::Duration::from_secs(5),
            "the wait never started"
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert_eq!(waiter.join().unwrap(), Ok(vec![Value::I32(0)]));
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

#[test]
fn calls_between_the_tiers_with_many_values() {
    let run = |interpreted: bool| {
        let mut store = Store::new();
        if interpreted {
            store.disable_jit();
        }
        let instance = instantiate(&mut store, "wide_calls.wasm");
        call(&mut store, instance, "run", &[]).unwrap()
    };
    assert_eq!(run(false), run(true));
}
