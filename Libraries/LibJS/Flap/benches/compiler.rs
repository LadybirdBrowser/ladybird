/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{
    Architecture, CompilationMetrics, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target,
};
use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

struct CountingAllocator;

static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);
static ALLOCATED_BYTES: AtomicU64 = AtomicU64::new(0);

unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        ALLOCATED_BYTES.fetch_add(layout.size() as u64, Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        ALLOCATED_BYTES.fetch_add(layout.size() as u64, Ordering::Relaxed);
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        ALLOCATED_BYTES.fetch_add(new_size as u64, Ordering::Relaxed);
        unsafe { System.realloc(pointer, layout, new_size) }
    }
}

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

const INTERPRETER: &str = include_str!("../../Interpreter/interpreter.flap");
const LAYOUT: &str = include_str!("../tests/interpreter-layout.conf");
const BYTECODE_DEF: &str = include_str!("../../Bytecode/Bytecode.def");

fn compiler() -> Compiler {
    Compiler::new(CompileOptions {
        target: Target {
            architecture: Architecture::X86_64,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        enable_assertions: false,
    })
}

fn compilation_unit() -> CompilationUnit<'static> {
    CompilationUnit {
        source: SourceInput {
            name: "interpreter.flap",
            contents: INTERPRETER,
        },
        constants: Some(SourceInput {
            name: "interpreter-layout.conf",
            contents: LAYOUT,
        }),
        bytecode_def: Some(SourceInput {
            name: "Bytecode.def",
            contents: BYTECODE_DEF,
        }),
    }
}

#[derive(Default)]
struct Totals {
    elapsed: Duration,
    metrics: Option<CompilationMetrics>,
    allocations: u64,
    allocated_bytes: u64,
}

fn main() {
    let iterations = std::env::args()
        .skip_while(|argument| argument != "--iterations")
        .nth(1)
        .map(|argument| {
            argument
                .parse::<u32>()
                .expect("--iterations requires a positive integer")
        })
        .unwrap_or(20);
    assert!(iterations != 0, "--iterations must not be zero");

    let compiler = compiler();
    compiler
        .compile(compilation_unit())
        .expect("interpreter warmup compilation should succeed");

    let mut totals = Totals::default();
    for _ in 0..iterations {
        let allocations_before = ALLOCATIONS.load(Ordering::Relaxed);
        let bytes_before = ALLOCATED_BYTES.load(Ordering::Relaxed);
        let started_at = Instant::now();
        let (assembly, metrics) = compiler
            .compile_with_metrics(compilation_unit())
            .expect("interpreter compilation should succeed");
        totals.elapsed += started_at.elapsed();
        totals.metrics = Some(add_metrics(totals.metrics, metrics));
        totals.allocations += ALLOCATIONS.load(Ordering::Relaxed) - allocations_before;
        totals.allocated_bytes += ALLOCATED_BYTES.load(Ordering::Relaxed) - bytes_before;
        black_box(assembly);
    }

    let metrics = totals.metrics.unwrap();
    println!("Flap AOT interpreter benchmark ({iterations} iterations)");
    println!(
        "total={:.3} ms allocations={} allocated={:.2} MiB",
        milliseconds(totals.elapsed) / f64::from(iterations),
        totals.allocations / u64::from(iterations),
        totals.allocated_bytes as f64 / f64::from(iterations) / (1024.0 * 1024.0)
    );
    println!(
        "prepare={:.3} lower={:.3} select={:.3} allocate={:.3} finalize={:.3} emit={:.3} ms",
        milliseconds(metrics.prepare) / f64::from(iterations),
        milliseconds(metrics.lower) / f64::from(iterations),
        milliseconds(metrics.select) / f64::from(iterations),
        milliseconds(metrics.allocate) / f64::from(iterations),
        milliseconds(metrics.finalize) / f64::from(iterations),
        milliseconds(metrics.emit) / f64::from(iterations),
    );
    println!(
        "handlers={} SSA={}/{}/{} LowIR={} target={}/{} machine={}",
        metrics.handlers / iterations as usize,
        metrics.ssa_blocks / iterations as usize,
        metrics.ssa_instructions / iterations as usize,
        metrics.ssa_values / iterations as usize,
        metrics.low_ir_instructions / iterations as usize,
        metrics.target_instructions / iterations as usize,
        metrics.virtual_registers / iterations as usize,
        metrics.machine_instructions / iterations as usize,
    );
}

fn add_metrics(total: Option<CompilationMetrics>, metrics: CompilationMetrics) -> CompilationMetrics {
    let Some(total) = total else {
        return metrics;
    };
    CompilationMetrics {
        prepare: total.prepare + metrics.prepare,
        lower: total.lower + metrics.lower,
        select: total.select + metrics.select,
        allocate: total.allocate + metrics.allocate,
        finalize: total.finalize + metrics.finalize,
        emit: total.emit + metrics.emit,
        handlers: total.handlers + metrics.handlers,
        ssa_blocks: total.ssa_blocks + metrics.ssa_blocks,
        ssa_instructions: total.ssa_instructions + metrics.ssa_instructions,
        ssa_values: total.ssa_values + metrics.ssa_values,
        low_ir_instructions: total.low_ir_instructions + metrics.low_ir_instructions,
        target_instructions: total.target_instructions + metrics.target_instructions,
        virtual_registers: total.virtual_registers + metrics.virtual_registers,
        machine_instructions: total.machine_instructions + metrics.machine_instructions,
    }
}

fn milliseconds(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1_000.0
}
