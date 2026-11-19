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

pub mod artifact;
pub mod asm;
pub mod bitset;
pub mod builder;
pub mod bytecode;
pub mod code;
pub mod codegen;
pub mod compile_pool;
pub mod coverage;
pub mod fast_hash;
pub mod inline_vec;
pub mod ir;
pub mod options;
pub mod passes;
pub mod regalloc;
pub mod scratch;
pub mod snapshot;
pub mod sparse_bitset;

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

/// Compiles `snapshot.executables[0]` for the host architecture, with the memory of `scratch`.
pub fn compile(snapshot: &Snapshot, scratch: &mut Scratch) -> Result<CompiledCode, CompileFailure> {
    compile_for::<asm::MacroAssembler>(
        snapshot,
        &|pc| {
            snapshot
                .executables
                .first()
                .is_none_or(|executable| builder::has_run(executable, pc))
        },
        scratch,
    )
}

/// With `StressOptions::few_registers`, how many registers values get
/// besides the ones calls use.
const STRESS_EXTRA_REGISTERS: u32 = 2;

/// Compiles `snapshot.executables[0]` with the macro assembler `M`. The
/// instructions for which `has_run(pc)` is false become unconditional exits.
pub fn compile_for<M: asm::PortableMacroAssembler>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    scratch: &mut Scratch,
) -> Result<CompiledCode, CompileFailure> {
    let mut graph = builder::build_graph_with_feedback(snapshot, has_run)?;
    let executable = &snapshot.executables[0];
    let pass_dumps = if snapshot.options.dump_passes {
        Some(passes::optimize_with_dumps(
            &mut graph,
            &executable.layout,
            snapshot.options.verify_ir,
        ))
    } else {
        passes::optimize(&mut graph, &executable.layout, snapshot.options.verify_ir);
        None
    };
    let (mut registers, _) = codegen::target_registers::<M>();
    if snapshot.options.stress.few_registers {
        registers.allocatable_gprs = registers.few_registers(STRESS_EXTRA_REGISTERS);
    }
    let allocation = regalloc::allocate(&graph, &registers);
    if snapshot.options.verify_ir
        && let Err(errors) = regalloc::verify(&graph, &registers, &allocation)
    {
        panic!(
            "invalid register allocation:\n{errors}\n{}",
            regalloc::dump_allocation(&graph, &allocation)
        );
    }
    let mut generated = codegen::generate::<M>(
        &graph,
        &allocation,
        &snapshot.executables,
        &snapshot.runtime,
        snapshot.options.stress,
        snapshot.options.verify_ir,
        scratch,
    )?;
    generated.osr_entries.sort_unstable();
    let layout = scratch.artifact.finish(&Tail {
        osr_entries: &generated.osr_entries,
        osr_loop_back_edges: &graph.osr_loop_back_edges,
        osr_outer_loop_back_edges: &graph.osr_outer_loop_back_edges,
        uncaught_handlers: &graph.uncaught_handlers,
        invalidation_patches: &generated.invalidation_patches,
        embedded_cells: &graph.embedded_cells,
        dependencies: &graph.dependencies,
    })?;
    let words = scratch.artifact.words();

    let mut dump = None;
    if snapshot.options.dump_ir || snapshot.options.dump_asm || pass_dumps.is_some() {
        let mut text = pass_dumps.unwrap_or_default();
        if snapshot.options.dump_ir {
            text.push_str(&ir::dump(&graph, &executable.layout));
            text.push_str(&regalloc::dump_allocation(&graph, &allocation));
        }
        if snapshot.options.dump_asm {
            text.push_str(&codegen::dump_code(
                &graph,
                executable,
                &snapshot.runtime,
                M::ARCHITECTURE,
                &generated,
                Artifact::new(words, &layout),
            ));
        }
        dump = Some(text);
    }
    Ok(CompiledCode {
        data_offset: generated.data_offset,
        code: generated.code,
        entry_offset: generated.entry_offset,
        site_index_frame_offset: generated.site_index_frame_offset,
        register_dump_frame_offset: generated.register_dump_frame_offset,
        words: words.to_vec(),
        layout,
        coverage: if snapshot.options.coverage {
            coverage::coverage_keys(&graph)
        } else {
            Vec::new()
        },
        dump,
    })
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
        let result = std::thread::spawn(move || compile(&snapshot, &mut scratch))
            .join()
            .unwrap();
        assert!(matches!(
            result.unwrap_err(),
            CompileFailure::InvalidBytecode { pc: 0, .. }
        ));
    }
}
