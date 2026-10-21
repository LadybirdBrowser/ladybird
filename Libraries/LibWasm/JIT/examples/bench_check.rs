/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs a module's entry function in two stores, one interpreted and one where the runtime compiles every function
//! (`LIBWASM_JIT=eager` unless set otherwise), and compares the results, traps, exit codes, memories, tables and
//! globals afterwards. The imports are deterministic: WASI, with `clock_time_get` and `random_get` replaced by
//! counters, and any other function import returns a clock in milliseconds of its result type, 100 * calls^2, so that
//! both stores see the same. A case that runs longer than `--timeout SECONDS` (300) ends the process with a failure.
//! `--compiled-only` runs and times only the compiled store, for timing.
//!
//! ```text
//! bench_check [--timeout SECONDS] [--compiled-only] MODULE ENTRY [MODULE ENTRY]...
//! ```

#[path = "../tests/harness/mod.rs"]
mod harness;

use harness::script::{outcome, same_outcome, state_difference};
use libwasm_engine::module::ImportDescription;
use libwasm_engine::runtime::{ExternValue, HostCallback, InstanceAddress, MemoryAddress, Store, Value};
use libwasm_engine::types::ValType;
use libwasm_engine::wasi::Wasi;
use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

struct Instance {
    store: Store,
    instance: InstanceAddress,
    wasi: Rc<Wasi>,
}

fn instantiate(module: &Arc<libwasm_engine::Module>, interpreted: bool) -> Option<Instance> {
    let mut store = Store::new();
    if interpreted {
        store.disable_jit();
    }
    let wasi = Rc::new(Wasi::new(vec!["bench".to_string()], Vec::new(), Vec::new()));
    let memory: Rc<Cell<Option<MemoryAddress>>> = Rc::new(Cell::new(None));
    let counter = Rc::new(Cell::new(0u64));
    let mut imports = Vec::new();
    for import in &module.imports {
        let ImportDescription::Function(type_index) = import.description else {
            eprintln!("{}.{}: only function imports are provided", import.module, import.name);
            return None;
        };
        let ty = module.types[type_index as usize].as_func()?.clone();
        let function = match (import.module.as_str(), import.name.as_str()) {
            ("wasi_snapshot_preview1", "clock_time_get" | "random_get") => {
                let (memory, counter, name) = (memory.clone(), counter.clone(), import.name.clone());
                let callback: HostCallback = Rc::new(move |store: &mut Store, arguments: &[Value]| {
                    let Some(memory) = memory.get() else {
                        return Ok(vec![Value::I32(8)]);
                    };
                    let bytes = store.memory_mut(memory).bytes_mut();
                    let argument = |index: usize| match arguments[index] {
                        Value::I32(value) => value as u32 as usize,
                        _ => 0,
                    };
                    counter.set(counter.get() + 1_000_000);
                    if name == "clock_time_get" {
                        let at = argument(2);
                        bytes[at..at + 8].copy_from_slice(&counter.get().to_le_bytes());
                    } else {
                        for (offset, byte) in bytes[argument(0)..argument(0) + argument(1)].iter_mut().enumerate() {
                            *byte = offset as u8;
                        }
                    }
                    Ok(vec![Value::I32(0)])
                });
                store.allocate_host_function(ty, callback)
            }
            ("wasi_snapshot_preview1", name) => wasi.allocate_function(&mut store, name)?,
            _ => {
                let results = ty.results.clone();
                let calls = Rc::new(Cell::new(0u64));
                let callback: HostCallback = Rc::new(move |_: &mut Store, _: &[Value]| {
                    // NB: A clock that advances more with every call, so that a benchmark that runs until enough
                    //     time has passed, like coremark, stops after a few rounds.
                    calls.set(calls.get() + 1);
                    let now = 100 * calls.get() * calls.get();
                    Ok(results
                        .iter()
                        .map(|result| match result {
                            ValType::I32 => Value::I32(now as i32),
                            ValType::I64 => Value::I64(now as i64),
                            ValType::F32 => Value::F32((now as f32).to_bits()),
                            ValType::F64 => Value::F64((now as f64).to_bits()),
                            other => Value::default_for(*other),
                        })
                        .collect())
                });
                store.allocate_host_function(ty, callback)
            }
        };
        imports.push(ExternValue::Function(function));
    }
    let instance = match store.instantiate(module.clone(), &imports) {
        Ok(instance) => instance,
        Err(error) => {
            eprintln!("instantiation failed: {error:?}");
            return None;
        }
    };
    if let Some(ExternValue::Memory(exported)) = store.instance(instance).export("memory") {
        wasi.set_memory(exported);
        memory.set(Some(exported));
    }
    Some(Instance { store, instance, wasi })
}

fn main() {
    harness::run_main(checked_main);
}

fn checked_main() {
    if std::env::var_os("LIBWASM_JIT").is_none() {
        // SAFETY: Nothing else runs yet.
        unsafe { std::env::set_var("LIBWASM_JIT", "eager") };
    }
    let mut arguments = std::env::args().skip(1).collect::<Vec<_>>();
    let mut timeout = 300;
    if arguments.first().is_some_and(|argument| argument == "--timeout") {
        timeout = arguments[1].parse().expect("--timeout SECONDS");
        arguments.drain(..2);
    }
    // For timing: runs only the compiled store, and compares nothing.
    let compiled_only = arguments.first().is_some_and(|argument| argument == "--compiled-only");
    if compiled_only {
        arguments.drain(..1);
    }
    // A watchdog, so that a case that hangs cannot hold the machine.
    static DEADLINE: AtomicU64 = AtomicU64::new(u64::MAX);
    static CURRENT: Mutex<String> = Mutex::new(String::new());
    std::thread::spawn(|| {
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
            if now > DEADLINE.load(Ordering::Relaxed) {
                println!("TIMEOUT   {}", CURRENT.lock().unwrap());
                std::process::exit(1);
            }
        }
    });
    let mut failures = 0;
    for pair in arguments.chunks(2) {
        let [path, entry] = pair else {
            eprintln!("usage: bench_check MODULE ENTRY [MODULE ENTRY]...");
            std::process::exit(2);
        };
        *CURRENT.lock().unwrap() = path.clone();
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
        DEADLINE.store(now + timeout, Ordering::Relaxed);
        let bytes = std::fs::read(path).expect("the module can be read");
        let module = Arc::new(libwasm_engine::compile(&bytes).expect("the module validates"));
        let (Some(mut interpreted), Some(mut jit)) = (instantiate(&module, true), instantiate(&module, false)) else {
            failures += 1;
            continue;
        };
        let run = |instance: &mut Instance| {
            let Some(ExternValue::Function(function)) = instance.store.instance(instance.instance).export(entry) else {
                panic!("{path} exports no function {entry}");
            };
            let start = std::time::Instant::now();
            let outcome = outcome(instance.store.invoke(function, &[]));
            (outcome, start.elapsed())
        };
        let name = std::path::Path::new(path)
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned();
        if compiled_only {
            let (actual, jit_time) = run(&mut jit);
            println!(
                "timed     {name} ({actual:?}; {:.3} s compiled)",
                jit_time.as_secs_f64()
            );
            continue;
        }
        let (expected, interpreted_time) = run(&mut interpreted);
        let (actual, jit_time) = run(&mut jit);
        let mut problems = Vec::new();
        if !same_outcome(&expected, &actual) {
            problems.push(format!("interpreter {expected:?}, JIT {actual:?}"));
        }
        if interpreted.wasi.exit_code() != jit.wasi.exit_code() {
            problems.push(format!(
                "exit codes {:?} and {:?}",
                interpreted.wasi.exit_code(),
                jit.wasi.exit_code()
            ));
        }
        if let Some(difference) = state_difference(&interpreted.store, interpreted.instance, &jit.store, jit.instance) {
            problems.push(difference);
        }
        if problems.is_empty() {
            println!(
                "same      {name} ({expected:?}, exit {:?}; {:.2} s interpreted, {:.2} s compiled)",
                jit.wasi.exit_code(),
                interpreted_time.as_secs_f64(),
                jit_time.as_secs_f64()
            );
        } else {
            failures += 1;
            println!("DIFFERENT {name}: {}", problems.join("; "));
        }
    }
    if failures > 0 {
        std::process::exit(1);
    }
}
