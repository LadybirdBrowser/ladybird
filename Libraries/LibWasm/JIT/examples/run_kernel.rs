/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Times calls of one function of a module, compiled and interpreted, for evidence of what an optimization does at
//! run time while the JIT has no runtime of its own. Imported functions are stubs that return zeros.
//!
//! ```text
//! run_kernel MODULE FUNCTION [--calls N] [--runs N] [--interpreter] [--no-licm] [--gvn] ARGUMENT...
//! ```
//!
//! Arguments are `i32:5`, `i64:-1`, `f32:1.5` or `f64:2`. The time is the best of `--runs` runs of `--calls` calls.

#[path = "../tests/harness/mod.rs"]
mod harness;

use harness::{CompiledModule, Outcome, TestBinding};
use libwasm_engine::module::ImportDescription;
use libwasm_engine::runtime::{ExternValue, HostCallback, Store, Value};
use libwasm_engine::types::ValType;
use libwasm_jit::Options;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;

fn parse(argument: &str) -> Value {
    let (ty, value) = argument.split_once(':').expect("TYPE:VALUE");
    match ty {
        "i32" => Value::I32(value.parse::<i64>().expect("an i32") as i32),
        "i64" => Value::I64(value.parse().expect("an i64")),
        "f32" => Value::F32(value.parse::<f32>().expect("an f32").to_bits()),
        "f64" => Value::F64(value.parse::<f64>().expect("an f64").to_bits()),
        _ => panic!("unknown type {ty}"),
    }
}

fn main() {
    harness::run_main(checked_main);
}

fn checked_main() {
    let mut arguments = std::env::args().skip(1);
    let path = arguments.next().expect("MODULE");
    let function: u32 = arguments
        .next()
        .and_then(|function| function.parse().ok())
        .expect("FUNCTION");
    let (mut calls, mut runs, mut interpreter) = (100, 5, false);
    let mut options = Options::default();
    let mut values = Vec::new();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--calls" => calls = arguments.next().and_then(|n| n.parse().ok()).expect("--calls N"),
            "--runs" => runs = arguments.next().and_then(|n| n.parse().ok()).expect("--runs N"),
            "--interpreter" => interpreter = true,
            "--no-licm" => options.no_licm = true,
            "--gvn" => options.gvn = true,
            _ => values.push(parse(&argument)),
        }
    }
    let module = Arc::new(libwasm_engine::compile(&std::fs::read(&path).unwrap()).unwrap());
    let mut store = Store::new();
    let mut imports = Vec::new();
    for import in &module.imports {
        let ImportDescription::Function(type_index) = import.description else {
            panic!("only function imports are stubbed");
        };
        let ty = module.types[type_index as usize].as_func().unwrap().clone();
        let results = ty.results.clone();
        let callback: HostCallback = Rc::new(move |_: &mut Store, _: &[Value]| {
            Ok(results
                .iter()
                .map(|result: &ValType| Value::default_for(*result))
                .collect())
        });
        imports.push(ExternValue::Function(store.allocate_host_function(ty, callback)));
    }
    let instance = store.instantiate(module.clone(), &imports).unwrap();
    let address = store.instance(instance).functions[function as usize];
    let compiled = CompiledModule::compile(module, options);
    let mut binding = TestBinding::new(&compiled, &store, instance);
    if !interpreter {
        assert!(
            compiled.runnable[function as usize],
            "function {function} does not run compiled"
        );
    }
    let mut best = f64::MAX;
    let mut last = None;
    for _ in 0..runs {
        let start = Instant::now();
        for _ in 0..calls {
            let outcome = if interpreter {
                match store.invoke(address, &values) {
                    Ok(values) => Outcome::Values(values),
                    Err(trap) => Outcome::Trap(harness::script::trap_code(&trap)),
                }
            } else {
                binding.call(&mut store, &compiled, function, &values)
            };
            last = Some(outcome);
        }
        best = best.min(start.elapsed().as_secs_f64());
    }
    println!(
        "{:.3} ms for {calls} calls ({:.1} us each), last outcome {:?}",
        best * 1e3,
        best * 1e6 / f64::from(calls),
        last.unwrap()
    );
}
