/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles every function of WebAssembly modules and reports how long it took.
//!
//! ```text
//! compile_all [--threads N] [--runs N] [--verify] [--stress-registers] [--no-licm] [--quiet]
//!             [--dump-ir FUNCTION [--osr PC]] [--dump-asm] MODULE|DIRECTORY...
//! ```
//!
//! A first pass finds the functions the JIT compiles; every measurement after it compiles only those, so that time
//! spent on functions that fail does not count. CPU time is the sum of every compile thread's own CPU time
//! (CLOCK_THREAD_CPUTIME_ID), taken once per thread after its last compile, the best of `--runs` runs, on one thread and on
//! `--threads`. A pass with phase timers (time stamp counter ticks) gives the time of each phase, the slowest
//! function per byte among those of at least 1 KiB, the cost of the smallest functions, and the nodes, stack slots
//! and moves per byte. Module decoding and validation are not included.

use libwasm_jit::{CompileContext, CompileRequest, Options, PHASES, RuntimeInfo, Stats};
use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

/// A runtime for code that never runs.
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

/// Functions at least this big count for the slowest function per byte.
const LARGE_FUNCTION: u32 = 1024;
/// Functions at most this big count for the cost per function.
const SMALL_FUNCTION: u32 = 32;

fn thread_cpu_ns() -> u64 {
    let mut time = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &raw mut time) };
    time.tv_sec as u64 * 1_000_000_000 + time.tv_nsec as u64
}

/// Time stamp counter ticks per nanosecond.
fn ticks_per_ns() -> f64 {
    let (start, start_ticks) = (Instant::now(), libwasm_jit::ticks());
    std::thread::sleep(Duration::from_millis(100));
    let ticks = libwasm_jit::ticks() - start_ticks;
    ticks as f64 / start.elapsed().as_nanos() as f64
}

struct Arguments {
    threads: usize,
    runs: usize,
    options: Options,
    dump: Option<u32>,
    /// With --dump-ir, the OSR compile at the loop at this module offset.
    osr: Option<u32>,
    quiet: bool,
    paths: Vec<String>,
}

fn parse_arguments() -> Arguments {
    let mut arguments = Arguments {
        threads: 1,
        runs: 3,
        options: Options::default(),
        dump: None,
        osr: None,
        quiet: false,
        paths: Vec::new(),
    };
    let mut iterator = std::env::args().skip(1);
    while let Some(argument) = iterator.next() {
        match argument.as_str() {
            "--threads" => arguments.threads = iterator.next().and_then(|n| n.parse().ok()).expect("--threads N"),
            "--runs" => arguments.runs = iterator.next().and_then(|n| n.parse().ok()).expect("--runs N"),
            "--verify" => arguments.options.verify_ir = true,
            "--stress-registers" => arguments.options.stress_registers = true,
            "--no-licm" => arguments.options.no_licm = true,
            "--quiet" => arguments.quiet = true,
            "--dump-asm" => arguments.options.dump_asm = true,
            "--osr" => arguments.osr = Some(iterator.next().and_then(|n| n.parse().ok()).expect("--osr PC")),
            "--dump-ir" => {
                arguments.dump = Some(
                    iterator
                        .next()
                        .and_then(|n| n.parse().ok())
                        .expect("--dump-ir FUNCTION"),
                );
            }
            _ => arguments.paths.push(argument),
        }
    }
    arguments
}

fn module_paths(paths: &[String]) -> Vec<std::path::PathBuf> {
    let mut result = Vec::new();
    for path in paths {
        let path = std::path::PathBuf::from(path);
        if path.is_dir() {
            let mut entries = std::fs::read_dir(&path)
                .expect("a readable directory")
                .filter_map(|entry| entry.ok().map(|entry| entry.path()))
                .filter(|path| path.extension().is_some_and(|extension| extension == "wasm"))
                .collect::<Vec<_>>();
            entries.sort();
            result.extend(entries);
        } else {
            result.push(path);
        }
    }
    result
}

/// Compiles `functions` (counting imports) of `module` on `threads` threads and returns the CPU time they took.
fn timed_compile(module: &Arc<libwasm_engine::Module>, functions: &[u32], threads: usize, options: Options) -> u64 {
    let next = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        let handles = (0..threads)
            .map(|_| {
                scope.spawn(|| {
                    let mut context = CompileContext::default();
                    let start = thread_cpu_ns();
                    loop {
                        let index = next.fetch_add(1, Ordering::Relaxed);
                        let Some(&function) = functions.get(index) else {
                            break;
                        };
                        let request = CompileRequest {
                            osr: None,
                            module: module.clone(),
                            function,
                            runtime: RUNTIME,
                            options,
                        };
                        libwasm_jit::compile(&mut context, &request).expect("it compiled before");
                    }
                    thread_cpu_ns() - start
                })
            })
            .collect::<Vec<_>>();
        handles.into_iter().map(|handle| handle.join().unwrap()).sum()
    })
}

#[derive(Default)]
struct Totals {
    functions: usize,
    compiled: usize,
    code_bytes: u64,
    compiled_bytes: u64,
    cpu_ns: u64,
    threaded_cpu_ns: u64,
    failures: BTreeMap<&'static str, usize>,
}

fn main() {
    let arguments = parse_arguments();
    let ticks_per_ns = ticks_per_ns();
    let mut totals = Totals::default();
    for path in module_paths(&arguments.paths) {
        let Ok(bytes) = std::fs::read(&path) else {
            continue;
        };
        let Ok(module) = libwasm_engine::compile(&bytes) else {
            continue;
        };
        let module = Arc::new(module);
        let first = module.imported_function_count;
        if let Some(function) = arguments.dump {
            let mut context = CompileContext::default();
            let mut options = arguments.options;
            options.dump_ir = !arguments.quiet;
            options.stats = true;
            let request = CompileRequest {
                osr: arguments.osr,
                module: module.clone(),
                function,
                runtime: RUNTIME,
                options,
            };
            // NB: The phases are the best of `--runs` compiles, the first of which also dumps.
            let mut best = None::<Vec<u64>>;
            for _ in 1..arguments.runs {
                let quiet = CompileRequest {
                    osr: None,
                    module: module.clone(),
                    function,
                    runtime: RUNTIME,
                    options: Options {
                        dump_ir: false,
                        ..options
                    },
                };
                if let Ok(compiled) = libwasm_jit::compile(&mut context, &quiet) {
                    let ticks = compiled.stats.ticks.to_vec();
                    best = Some(match best {
                        Some(best) => best.iter().zip(&ticks).map(|(a, b)| *a.min(b)).collect(),
                        None => ticks,
                    });
                }
            }
            match libwasm_jit::compile(&mut context, &request) {
                Ok(compiled) => {
                    let body = &module.code[(function - first) as usize];
                    let bytes = f64::from(body.code.end - body.code.start);
                    let ticks = match best {
                        Some(best) => best.iter().zip(compiled.stats.ticks).map(|(a, b)| *a.min(&b)).collect(),
                        None => compiled.stats.ticks.to_vec(),
                    };
                    let phases = libwasm_jit::PHASES
                        .iter()
                        .zip(ticks)
                        .map(|(phase, ticks)| format!("{phase} {:.1}", ticks as f64 / ticks_per_ns / bytes))
                        .collect::<Vec<_>>();
                    println!(
                        "function {function}: {bytes} bytes; phases (ns/byte): {}; {} nodes, {} slots, {} moves",
                        phases.join(", "),
                        compiled.stats.nodes,
                        compiled.stats.slots,
                        compiled.stats.moves
                    );
                    println!("{}", compiled.dump.unwrap_or_default());
                }
                Err(failure) => println!("{failure:?}"),
            }
            continue;
        }

        // Which functions compile.
        let mut context = CompileContext::default();
        let mut compiled = Vec::new();
        let mut failures: BTreeMap<&'static str, usize> = BTreeMap::new();
        for index in 0..module.code.len() as u32 {
            let request = CompileRequest {
                osr: None,
                module: module.clone(),
                function: first + index,
                runtime: RUNTIME,
                options: arguments.options,
            };
            match libwasm_jit::compile(&mut context, &request) {
                Ok(_) => compiled.push(first + index),
                Err(failure) => *failures.entry(failure.reason()).or_default() += 1,
            }
        }
        let size = |function: u32| {
            let body = &module.code[(function - first) as usize].code;
            body.end - body.start
        };

        // The phase timers, the slowest functions and the counts. Each function's time is its best of the runs, so
        // that a preemption does not make it the slowest.
        let mut stats = Stats::default();
        let mut best_ns = vec![f64::MAX; compiled.len()];
        let options = Options {
            stats: true,
            ..arguments.options
        };
        for run in 0..arguments.runs.max(1) {
            for (position, &function) in compiled.iter().enumerate() {
                let request = CompileRequest {
                    osr: None,
                    module: module.clone(),
                    function,
                    runtime: RUNTIME,
                    options,
                };
                let start = libwasm_jit::ticks();
                let result = libwasm_jit::compile(&mut context, &request).expect("it compiled before");
                let ns = (libwasm_jit::ticks() - start) as f64 / ticks_per_ns;
                best_ns[position] = best_ns[position].min(ns);
                if run > 0 {
                    continue;
                }
                for (total, phase) in stats.ticks.iter_mut().zip(result.stats.ticks) {
                    *total += phase;
                }
                stats.nodes += result.stats.nodes;
                stats.slots += result.stats.slots;
                stats.moves += result.stats.moves;
                stats.hoisted += result.stats.hoisted;
            }
        }
        let mut worst = (0.0, 0, 0);
        let mut small = Vec::new();
        for (position, &function) in compiled.iter().enumerate() {
            let (bytes, ns) = (size(function), best_ns[position]);
            if bytes >= LARGE_FUNCTION && ns / f64::from(bytes) > worst.0 {
                worst = (ns / f64::from(bytes), function, bytes);
            }
            if bytes <= SMALL_FUNCTION {
                small.push(ns);
            }
        }
        small.sort_by(f64::total_cmp);

        // CPU time.
        let best = |threads: usize| {
            (0..arguments.runs)
                .map(|_| timed_compile(&module, &compiled, threads, arguments.options))
                .min()
                .unwrap_or(0)
        };
        let cpu = best(1);
        let threaded = if arguments.threads > 1 {
            best(arguments.threads)
        } else {
            cpu
        };

        let code_bytes = module
            .code
            .iter()
            .map(|body| u64::from(body.code.end - body.code.start))
            .sum::<u64>();
        let compiled_bytes = compiled.iter().map(|function| u64::from(size(*function))).sum::<u64>();
        totals.functions += module.code.len();
        totals.compiled += compiled.len();
        totals.code_bytes += code_bytes;
        totals.compiled_bytes += compiled_bytes;
        totals.cpu_ns += cpu;
        totals.threaded_cpu_ns += threaded;
        for (what, count) in &failures {
            *totals.failures.entry(what).or_default() += count;
        }
        if arguments.quiet {
            continue;
        }
        let per_byte = |ns: f64| ns / compiled_bytes.max(1) as f64;
        println!("{}", path.display());
        println!(
            "  compiled: {}/{} functions ({:.1}%), {}/{} bytes ({:.1}%)",
            compiled.len(),
            module.code.len(),
            100.0 * compiled.len() as f64 / module.code.len().max(1) as f64,
            compiled_bytes,
            code_bytes,
            100.0 * compiled_bytes as f64 / code_bytes.max(1) as f64
        );
        println!(
            "  CPU: {:.3} ms on 1 thread, {:.1} ns/byte; {:.3} ms on {} threads, {:.1} ns/byte",
            cpu as f64 / 1e6,
            per_byte(cpu as f64),
            threaded as f64 / 1e6,
            arguments.threads,
            per_byte(threaded as f64)
        );
        let phases = PHASES
            .iter()
            .zip(stats.ticks)
            .map(|(name, ticks)| format!("{name} {:.1}", per_byte(ticks as f64 / ticks_per_ns)))
            .collect::<Vec<_>>();
        println!("  phases (ns/byte): {}", phases.join(", "));
        if worst.2 > 0 {
            println!(
                "  slowest function of {LARGE_FUNCTION}+ bytes: {:.1} ns/byte (function {}, {} bytes)",
                worst.0, worst.1, worst.2
            );
        }
        if !small.is_empty() {
            println!(
                "  functions of up to {SMALL_FUNCTION} bytes: {:.0} ns median, {} of them",
                small[small.len() / 2],
                small.len()
            );
        }
        println!(
            "  per byte: {:.2} nodes, {:.3} slots, {:.3} moves; {} nodes hoisted out of loops",
            stats.nodes as f64 / compiled_bytes.max(1) as f64,
            stats.slots as f64 / compiled_bytes.max(1) as f64,
            stats.moves as f64 / compiled_bytes.max(1) as f64,
            stats.hoisted
        );
        for (what, count) in &failures {
            println!("  unsupported: {count} x {what}");
        }
    }
    println!(
        "total: {}/{} functions, {}/{} bytes compiled, {:.3} ms CPU on 1 thread ({:.1} ns/byte), {:.3} ms on {}",
        totals.compiled,
        totals.functions,
        totals.compiled_bytes,
        totals.code_bytes,
        totals.cpu_ns as f64 / 1e6,
        totals.cpu_ns as f64 / totals.compiled_bytes.max(1) as f64,
        totals.threaded_cpu_ns as f64 / 1e6,
        arguments.threads
    );
    for (what, count) in &totals.failures {
        println!("  unsupported: {count} x {what}");
    }
}
