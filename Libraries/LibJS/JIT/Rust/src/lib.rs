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

// NB: The tests keep their own data in the heap.
#![cfg_attr(test, allow(clippy::disallowed_types, clippy::disallowed_macros))]

// NB: A module with `allow(clippy::disallowed_types)` still uses containers of the heap (see clippy.toml). It loses
//     the `allow` when its data is in zones.
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod artifact;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod asm;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod bitset;
pub mod builder;
pub mod bytecode;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod code;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod codegen;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod compile_pool;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod coverage;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod fast_hash;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod inline_vec;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod ir;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod options;
pub mod passes;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod regalloc;
pub mod scratch;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod snapshot;
#[allow(clippy::disallowed_types, clippy::disallowed_macros)]
pub mod sparse_bitset;
pub mod tables;
pub mod zone_map;

// NB: The tests get their memory on Windows from `ak::windows_code_memory`, which is for x86-64.
#[cfg(all(test, windows, not(target_arch = "x86_64")))]
compile_error!("the tests of the JIT run on Windows on x86-64 only");

use artifact::Artifact;
use artifact::Tail;
use bytecode::OpCode;
use code::CompiledCode;
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
// NB: The coverage keys of a compile are still a list in the heap.
#[allow(clippy::disallowed_types)]
fn run_stages<M: asm::PortableMacroAssembler>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    scratch: &mut Scratch,
    compiled: &mut CompiledCode,
) -> Result<(), CompileFailure> {
    compiled.clear();
    if !snapshot.is_complete() {
        return Err(CompileFailure::TooLarge);
    }
    let Scratch {
        zone,
        work,
        steps,
        tables,
    } = scratch;
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
    let pass_dumps = if snapshot.options.dump_passes {
        Some(passes::optimize_with_dumps(
            &mut graph,
            &executable.layout,
            snapshot.options.verify_ir,
            &mut tables.dominators,
            work,
            steps,
        )?)
    } else {
        passes::optimize(
            &mut graph,
            &executable.layout,
            snapshot.options.verify_ir,
            &mut tables.dominators,
            work,
            steps,
        )?;
        None
    };
    let (mut registers, _) = codegen::target_registers::<M>();
    if snapshot.options.stress.few_registers {
        registers.allocatable_gprs = registers.few_registers(STRESS_EXTRA_REGISTERS);
    }
    regalloc::allocate(&graph, &registers, &mut tables.regalloc, &mut tables.dominators)?;
    if snapshot.options.verify_ir
        && let Err(errors) = regalloc::verify(&graph, &registers, &tables.regalloc.allocation)
    {
        panic!(
            "invalid register allocation:\n{errors}\n{}",
            regalloc::dump_allocation(&graph, &tables.regalloc.allocation)
        );
    }
    let mut generated = codegen::generate::<M>(&graph, snapshot, snapshot.options.verify_ir, tables, work)?;
    generated.osr_entries.sort_unstable();
    let layout = tables.artifact.finish(
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
    compiled.code.try_extend_from_slice(&generated.code)?;

    let mut dump = None;
    if snapshot.options.dump_ir || snapshot.options.dump_asm || pass_dumps.is_some() {
        let mut text = pass_dumps.unwrap_or_default();
        if snapshot.options.dump_ir {
            text.push_str(&ir::dump(&graph, &executable.layout));
            text.push_str(&regalloc::dump_allocation(&graph, &tables.regalloc.allocation));
        }
        if snapshot.options.dump_asm {
            text.push_str(&codegen::dump_code(
                &graph,
                executable,
                &snapshot.runtime,
                M::ARCHITECTURE,
                &generated,
                Artifact::new(&compiled.words, &layout),
            ));
        }
        dump = Some(text);
    }
    compiled.data_offset = generated.data_offset;
    compiled.entry_offset = generated.entry_offset;
    compiled.site_index_frame_offset = generated.site_index_frame_offset;
    compiled.register_dump_frame_offset = generated.register_dump_frame_offset;
    compiled.layout = layout;
    compiled.dump = dump;
    compiled.coverage = if snapshot.options.coverage {
        coverage::coverage_keys(&graph)
    } else {
        Vec::new()
    };
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
