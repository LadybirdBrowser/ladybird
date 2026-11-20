/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The LibJS optimizing JIT compiler.
//!
//! This crate is a pure compiler: it turns a `Snapshot` (plain data captured on
//! the main thread) into `CompiledCode` (plain data installed by the main
//! thread). It calls nothing in the runtime and has no access to the GC heap, so
//! it can run on a worker thread.

// NB: A compile makes no block in the heap: all its data is in zones (see `ak::Zone`). Without `std` (and without
//     `alloc`), rustc rejects code of the compiler that would make one. The tests keep `std`.
#![cfg_attr(not(test), no_std)]

pub mod artifact;
pub mod asm;
pub mod builder;
pub mod bytecode;
pub mod code;
pub mod codegen;
pub mod coverage;
pub mod debug_text;
pub mod ir;
pub mod passes;
pub mod regalloc;
pub mod scratch;
pub mod snapshot;
pub mod tables;

// NB: The tests get their memory on Windows from `ak::windows_code_memory`, which is for x86-64.
#[cfg(all(test, windows, not(target_arch = "x86_64")))]
compile_error!("the tests of the JIT run on Windows on x86-64 only");

use ak::Zone;
use artifact::Artifact;
use artifact::ArtifactWriter;
use artifact::Tail;
use bytecode::OpCode;
use code::CompiledCode;
use code::TextWriter;
use core::fmt::Write;
use scratch::Scratch;
use snapshot::Snapshot;

/// Why a compile job did not produce code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompileFailure {
    /// The bytecode could not be decoded or refers to things that do not exist.
    InvalidBytecode { pc: u32, reason: &'static str },
    /// The JIT does not handle the instruction at `pc`.
    UnsupportedInstruction {
        pc: u32,
        opcode: OpCode,
        reason: &'static str,
    },
    /// `RuntimeInfo` lacks the address of a helper the code needs: the slow
    /// path of `opcode`, or another runtime helper if `opcode` is `None`.
    MissingRuntimeHelper { opcode: Option<OpCode> },
    /// The assembler could not finish the code (a branch out of range).
    CodeGeneration,
    /// The compile needs more of something than the compiler has room for.
    TooLarge,
}

impl From<ak::ColumnFull> for CompileFailure {
    fn from(_: ak::ColumnFull) -> Self {
        Self::TooLarge
    }
}

/// The zone of the thread of a test, for the graphs that the test makes. It lives as long as the thread.
#[cfg(test)]
pub(crate) fn test_zone() -> &'static ak::Zone {
    thread_local! {
        static ZONE: &'static ak::Zone = Box::leak(Box::new(ak::Zone::new(1 << 30, c"test")));
    }
    ZONE.with(|zone| *zone)
}

/// A zone for the work data of the passes that a test runs.
#[cfg(test)]
pub(crate) fn test_work() -> ak::Zone {
    ak::Zone::new(1 << 30, c"test")
}

/// The zones for the steps of the passes that a test runs (see `Scratch::steps`).
#[cfg(test)]
pub(crate) fn test_steps() -> [ak::Zone; 2] {
    [test_work(), test_work()]
}

/// The allocator of the tests, which counts the blocks that `count_heap_blocks()` makes on its thread.
#[cfg(test)]
mod heap_counter {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;

    struct CountingAllocator;

    std::thread_local! {
        static COUNTING: Cell<bool> = const { Cell::new(false) };
        static BLOCKS: Cell<usize> = const { Cell::new(0) };
    }

    fn count_block() {
        if COUNTING.with(Cell::get) {
            BLOCKS.with(|blocks| blocks.set(blocks.get() + 1));
        }
    }

    // SAFETY: Each method passes its call on to `System` as it is.
    unsafe impl GlobalAlloc for CountingAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            count_block();
            // SAFETY: The caller keeps the contract of `GlobalAlloc::alloc`.
            unsafe { System.alloc(layout) }
        }

        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            count_block();
            // SAFETY: The caller keeps the contract of `GlobalAlloc::alloc_zeroed`.
            unsafe { System.alloc_zeroed(layout) }
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            count_block();
            // SAFETY: The caller keeps the contract of `GlobalAlloc::realloc`.
            unsafe { System.realloc(ptr, layout, new_size) }
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            // SAFETY: The caller keeps the contract of `GlobalAlloc::dealloc`.
            unsafe { System.dealloc(ptr, layout) }
        }
    }

    #[global_allocator]
    static ALLOCATOR: CountingAllocator = CountingAllocator;

    /// Runs `f`, and gives the number of blocks of the heap that it made or grew on this thread.
    pub(crate) fn count_heap_blocks(f: impl FnOnce()) -> usize {
        BLOCKS.with(|blocks| blocks.set(0));
        COUNTING.with(|counting| counting.set(true));
        f();
        COUNTING.with(|counting| counting.set(false));
        BLOCKS.with(Cell::get)
    }
}

#[cfg(test)]
pub(crate) use heap_counter::count_heap_blocks;

/// Compiles `snapshot.executables[0]` for the host architecture with the memory of `scratch`, into `compiled` in
/// place of what it holds. After a failure, `compiled` holds no complete code.
pub fn compile(snapshot: &Snapshot, scratch: &mut Scratch, compiled: &mut CompiledCode) -> Result<(), CompileFailure> {
    compile_into::<asm::MacroAssembler>(
        snapshot,
        &|pc| {
            snapshot
                .executables
                .first()
                .is_none_or(|executable| builder::has_run(snapshot, executable, pc))
        },
        scratch,
        compiled,
    )
}

/// With `StressOptions::few_registers`, how many registers values get
/// besides the ones calls use.
const STRESS_EXTRA_REGISTERS: u32 = 2;

/// Compiles `snapshot.executables[0]` with the macro assembler `M`, into memory of the code's own. The
/// instructions for which `has_run(pc)` is false become unconditional exits.
pub fn compile_for<M: asm::PortableMacroAssembler>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    scratch: &mut Scratch,
) -> Result<CompiledCode, CompileFailure> {
    let mut compiled = CompiledCode::new();
    compile_into::<M>(snapshot, has_run, scratch, &mut compiled)?;
    Ok(compiled)
}

/// Compiles `snapshot.executables[0]` with the macro assembler `M`, like `compile()`. The instructions for which
/// `has_run(pc)` is false become unconditional exits.
fn compile_into<M: asm::PortableMacroAssembler>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    scratch: &mut Scratch,
    compiled: &mut CompiledCode,
) -> Result<(), CompileFailure> {
    let result = run_stages::<M>(snapshot, has_run, scratch, compiled);
    scratch.end_compile();
    result
}

/// The stages of `compile_into()`, which leave their data in `scratch`.
fn run_stages<M: asm::PortableMacroAssembler>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    scratch: &mut Scratch,
    compiled: &mut CompiledCode,
) -> Result<(), CompileFailure> {
    compiled.clear();
    let options = &snapshot.options;
    compiled.keep_texts(
        options.dump_ir || options.dump_asm || options.dump_passes,
        options.coverage,
    );
    if !snapshot.is_complete() {
        return Err(CompileFailure::TooLarge);
    }
    let Scratch { zone, work, steps } = scratch;
    // NB: Nothing of the compile before this one is in use (see `Scratch`).
    work.reset();
    let mut graph = {
        let mut attempts = builder::Attempts::new(snapshot, has_run, work);
        loop {
            // NB: This drops the graph of the compile before this one, and a graph that the builder gave up.
            zone.reset();
            if let Some(graph) = attempts.build(zone)? {
                break graph;
            }
        }
    };
    // NB: The builder is done, and nothing of the graph is in its work data.
    work.reset();
    let executable = &snapshot.executables[0];
    // NB: The text of a dump that does not fit is cut (see `TextWriter`), so a failed write needs nothing more.
    let mut dump = compiled.dump.as_mut().map(TextWriter::dump);
    match &mut dump {
        Some(dump) if options.dump_passes => {
            passes::optimize_with_dumps(&mut graph, &executable.layout, options.verify_ir, work, steps, dump)?;
        }
        _ => passes::optimize(&mut graph, &executable.layout, options.verify_ir, work, steps)?,
    }
    let (mut registers, _) = codegen::target_registers::<M>();
    if snapshot.options.stress.few_registers {
        registers.allocatable_gprs = registers.few_registers(STRESS_EXTRA_REGISTERS);
    }
    // NB: The allocation is in the work zone, and the code generator reads it, so the work data of the register
    //     allocator, and of the stages after it, stays until the compile ends.
    let work: &Zone = work;
    let allocation = regalloc::allocate(&graph, &registers, work)?;
    if snapshot.options.verify_ir
        && let Err(errors) = regalloc::verify(&graph, &registers, &allocation, work)
    {
        panic!(
            "invalid register allocation:\n{errors}\n{}",
            regalloc::dump_allocation(&graph, &allocation)
        );
    }
    let mut artifact = ArtifactWriter::new_in(work);
    let mut generated = codegen::generate::<M>(
        &graph,
        snapshot,
        snapshot.options.verify_ir,
        &mut artifact,
        &allocation,
        &mut compiled.code,
        work,
    )?;
    generated.osr_entries.sort_unstable();
    let layout = artifact.finish(
        &Tail {
            osr_entries: &generated.osr_entries,
            osr_loop_back_edges: &graph.osr_loop_back_edges,
            osr_outer_loop_back_edges: &graph.osr_outer_loop_back_edges,
            uncaught_handlers: &graph.uncaught_handlers,
            invalidation_patches: &generated.invalidation_patches,
            embedded_cells: &graph.embedded_cells,
            dependencies: &graph.dependencies,
        },
        &mut compiled.words,
    )?;

    if let Some(dump) = &mut dump {
        if options.dump_ir {
            let _ = write!(
                dump,
                "{}{}",
                ir::dump(&graph, &executable.layout),
                regalloc::dump_allocation(&graph, &allocation)
            );
        }
        if options.dump_asm {
            let _ = write!(
                dump,
                "{}",
                codegen::dump_code(
                    &graph,
                    executable,
                    &snapshot.runtime,
                    M::ARCHITECTURE,
                    &compiled.code,
                    &generated,
                    Artifact::new(&compiled.words, &layout),
                )
            );
        }
    }
    if let Some(coverage) = &mut compiled.coverage {
        // NB: The keys that do not fit are left out (see `TextWriter`).
        let _ = coverage::write_coverage_keys(&graph, &mut TextWriter::lines(coverage));
    }
    compiled.data_offset = generated.data_offset;
    compiled.entry_offset = generated.entry_offset;
    compiled.site_index_frame_offset = generated.site_index_frame_offset;
    compiled.register_dump_frame_offset = generated.register_dump_frame_offset;
    compiled.layout = layout;
    Ok(())
}

// Compile jobs move between threads.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<Snapshot>();
    assert_send::<CompiledCode>();
    assert_send::<Scratch>();
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compile_jobs_can_move_between_threads() {
        let snapshot = Snapshot::default();
        let mut scratch = Scratch::new();
        let mut compiled = CompiledCode::new();
        let (result, compiled) = std::thread::spawn(move || {
            let result = compile(&snapshot, &mut scratch, &mut compiled);
            (result, compiled)
        })
        .join()
        .unwrap();
        assert!(matches!(
            result.unwrap_err(),
            CompileFailure::InvalidBytecode { pc: 0, .. }
        ));
        assert!(compiled.code.is_empty());
    }
}
