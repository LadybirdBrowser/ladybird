/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles every function of modules as an OSR continuation at each of its loops, with the IR and register
//! allocation verified, and reports what fails. Loops that cannot be OSR entries (see `CompileFailure::NoOsrEntry`) are
//! skipped.
//!
//! ```text
//! osr_check [--threads N] [--stress-registers] [--dump FUNCTION:PC] MODULE...
//! ```

use libwasm_jit::decode::{Decoder, Instruction};
use libwasm_jit::{CompileContext, CompileFailure, CompileRequest, Options, RuntimeInfo};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

const RUNTIME: RuntimeInfo = RuntimeInfo {
    trap_exit: 0x7fff_0000_1000,
    memory_grow: 0x7fff_0000_2000,
    table_grow: 0x7fff_0000_3000,
    call_host: 0x7fff_0000_4000,
    allocate: 0x7fff_0000_5000,
    wait: 0x7fff_0000_6000,
    notify: 0x7fff_0000_7000,
    allocate_exception: 0x7fff_0000_8000,
    throw: 0x7fff_0000_9000,
};

fn main() {
    let mut threads = 1;
    let mut options = Options {
        verify_ir: true,
        ..Options::default()
    };
    let mut dump = None;
    let mut paths = Vec::new();
    let mut arguments = std::env::args().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--threads" => threads = arguments.next().and_then(|n| n.parse().ok()).expect("--threads N"),
            "--stress-registers" => options.stress_registers = true,
            "--dump" => {
                let value = arguments.next().expect("--dump FUNCTION:PC");
                let (function, pc) = value.split_once(':').expect("--dump FUNCTION:PC");
                dump = Some((
                    function.parse::<u32>().expect("a function index"),
                    pc.parse::<u32>().expect("a module offset"),
                ));
            }
            _ => paths.push(argument),
        }
    }
    let mut failures = 0;
    for path in &paths {
        let bytes = std::fs::read(path).expect("a readable module");
        let module = Arc::new(libwasm_engine::compile(&bytes).expect("a valid module"));
        if let Some((function, pc)) = dump {
            let mut context = CompileContext::default();
            let request = CompileRequest {
                module: module.clone(),
                function,
                runtime: RUNTIME,
                options: Options {
                    dump_ir: true,
                    dump_asm: true,
                    ..options
                },
                osr: Some(pc),
            };
            match libwasm_jit::compile(&mut context, &request) {
                Ok(compiled) => println!("{}", compiled.dump.unwrap_or_default()),
                Err(failure) => println!("{failure:?}"),
            }
            continue;
        }
        let first = module.imported_function_count;
        let count = module.code.len() as u32;
        let next = AtomicUsize::new(0);
        let compiled = AtomicUsize::new(0);
        let failed = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for _ in 0..threads {
                scope.spawn(|| {
                    let mut context = CompileContext::default();
                    loop {
                        let index = next.fetch_add(1, Ordering::Relaxed) as u32;
                        if index >= count {
                            break;
                        }
                        let body = &module.code[index as usize];
                        let mut decoder = Decoder::new(&module.bytes, body.code.start as usize, body.code.end as usize);
                        let mut loops = Vec::new();
                        while !decoder.is_at_end() {
                            let pc = decoder.position() as u32;
                            if let Instruction::Loop(_) = decoder.decode() {
                                loops.push(pc);
                            }
                        }
                        for pc in loops {
                            let request = CompileRequest {
                                module: module.clone(),
                                function: first + index,
                                runtime: RUNTIME,
                                options,
                                osr: Some(pc),
                            };
                            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                                libwasm_jit::compile(&mut context, &request)
                            }));
                            match result {
                                Ok(Ok(_)) => {
                                    compiled.fetch_add(1, Ordering::Relaxed);
                                }
                                Ok(Err(CompileFailure::NoOsrEntry { .. })) => {}
                                Ok(Err(failure)) => {
                                    failed.fetch_add(1, Ordering::Relaxed);
                                    println!("FAIL {path} function {}:{pc}: {failure:?}", first + index);
                                }
                                Err(_) => {
                                    failed.fetch_add(1, Ordering::Relaxed);
                                    println!("FAIL {path} function {}:{pc}: panicked", first + index);
                                    context = CompileContext::default();
                                }
                            }
                        }
                    }
                });
            }
        });
        let failed = failed.load(Ordering::Relaxed);
        failures += failed;
        println!(
            "{path}: {} OSR compiles, {failed} failures",
            compiled.load(Ordering::Relaxed)
        );
    }
    if failures > 0 {
        std::process::exit(1);
    }
}
