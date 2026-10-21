/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The LibWasm optimizing JIT compiler.
//!
//! This crate is a pure compiler: it turns one function of a validated module into machine code and the metadata
//! the runtime needs to install and run it. It touches no heap and no store, so it runs on any thread. See
//! `ARCHITECTURE.md` for how it works.

// NB: The assemblers and the small data structures are libjs_jit's, until both JITs share them from a crate of
//     their own. The test helpers of the assemblers' own tests are for libjs_jit's tests.
#[cfg_attr(test, allow(dead_code))]
#[path = "../../../LibJS/JIT/Rust/src/asm/mod.rs"]
pub mod asm;
#[path = "../../../LibJS/JIT/Rust/src/inline_vec.rs"]
pub mod inline_vec;

pub mod builder;
pub mod codegen;
pub mod convention;
pub mod decode;
pub mod ir;
pub mod passes;
pub mod regalloc;
pub mod runtime;
pub mod stubs;
pub mod table;

use libwasm_module::Module;
use std::sync::Arc;

/// Why a function was not compiled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompileFailure {
    /// The function uses something the JIT does not compile yet, at module offset `pc`.
    Unsupported { pc: u32, what: &'static str },
    /// The assembler could not finish the code.
    CodeGeneration,
}

/// Options of a compile.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Options {
    /// Check the IR and the register allocation after every phase.
    pub verify_ir: bool,
    pub dump_ir: bool,
    pub dump_asm: bool,
    /// Measure the time of every phase (`Stats::ticks`), which includes verification if that is on too.
    pub stats: bool,
    /// Give values only a few registers, so that the allocator spills and reloads all the time.
    pub stress_registers: bool,
    /// Leave loop invariant code in its loop.
    pub no_licm: bool,
    /// Replace nodes that compute what a dominating node computed already, beyond what building does (`passes/gvn.rs`).
    /// Off by default: it finds about 0.1% of the nodes of real modules redundant, for 7 to 16 ns per code byte.
    pub gvn: bool,
}

/// The addresses of the runtime compiled code reaches, which are constants of the process.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RuntimeInfo {
    /// Where code jumps to trap, with the TRAP_* code in edi and rbp the frame pointer of the trapping function.
    pub trap_exit: u64,
    /// The runtime's helpers (`wasm_runtime_memory_grow`, `wasm_runtime_table_grow` and `wasm_runtime_call_host`),
    /// which take the `ExecutionContext` with their operands in its `arguments`.
    pub memory_grow: u64,
    pub table_grow: u64,
    pub call_host: u64,
}

/// What to compile.
pub struct CompileRequest {
    pub module: Arc<Module>,
    /// The function, counting imports.
    pub function: u32,
    pub runtime: RuntimeInfo,
    pub options: Options,
    /// For an OSR compile, the module offset of the outermost loop that it continues an interpreter frame at. Its
    /// code takes the frame's fp as its only argument and returns the function's results.
    pub osr: Option<u32>,
}

/// The reusable state of one compile thread: every phase's storage, cleared between compiles.
#[derive(Default)]
pub struct CompileContext {
    pub func: ir::Func,
    builder: builder::Builder,
    pub liveliness: passes::cleanup::Liveliness,
    licm: passes::licm::Licm,
    scheduler: passes::schedule::Scheduler,
    gvn: passes::gvn::Gvn,
    allocator: regalloc::Allocator,
    pub allocation: regalloc::Allocation,
}

/// The phases of a compile, as `Stats::ticks` counts them.
pub const PHASES: [&str; 5] = ["build", "passes", "liveness", "allocation", "codegen"];

/// What a compile did.
#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    /// The time stamp counter ticks of each phase (see `PHASES`), with `Options::stats`.
    pub ticks: [u64; PHASES.len()],
    /// The nodes the function's IR has, dead ones included.
    pub nodes: u32,
    /// The stack slots of the frame.
    pub slots: u32,
    /// The moves the allocator inserted, on edges and before nodes.
    pub moves: u32,
    /// The nodes loop invariant code motion moved out of loops.
    pub hoisted: u32,
    /// The nodes global value numbering replaced.
    pub eliminated: u32,
}

/// The time stamp counter, for the phase timers.
pub fn ticks() -> u64 {
    #[cfg(target_arch = "x86_64")]
    // SAFETY: rdtsc only reads a counter.
    unsafe {
        core::arch::x86_64::_rdtsc()
    }
    #[cfg(not(target_arch = "x86_64"))]
    0
}

/// The result of a compile.
#[derive(Debug, Default)]
pub struct CompiledFunction {
    /// Position-independent machine code; the entry point is its start.
    pub code: Vec<u8>,
    /// The bytes between fp and rsp in the function's frame.
    pub frame_size: u32,
    pub stats: Stats,
    pub dump: Option<String>,
}

/// Compiles one function of a module.
pub fn compile(context: &mut CompileContext, request: &CompileRequest) -> Result<CompiledFunction, CompileFailure> {
    let module = &*request.module;
    let timed = request.options.stats;
    let mut stats = Stats::default();
    let mut last = if timed { ticks() } else { 0 };
    let mut lap = |phase: usize, stats: &mut Stats| {
        if timed {
            let now = ticks();
            stats.ticks[phase] += now - last;
            last = now;
        }
    };
    match request.osr {
        None => context.builder.build(module, request.function, &mut context.func)?,
        Some(pc) => {
            let Some(stack_types) = context
                .builder
                .probe_osr(module, request.function, pc, &mut context.func)?
            else {
                return Err(CompileFailure::Unsupported {
                    pc,
                    what: "OSR at a loop that is unreachable or in another loop",
                });
            };
            context
                .builder
                .build_osr(module, request.function, pc, stack_types, &mut context.func)?;
        }
    }
    lap(0, &mut stats);
    if request.options.verify_ir
        && let Err(errors) = ir::verify::verify(&context.func, &|_| false, false)
    {
        panic!(
            "invalid IR after building function {}:\n{errors}\n{}",
            request.function,
            ir::dump::dump(&context.func, &|_| false)
        );
    }
    passes::cleanup::cleanup(&mut context.func, &mut context.liveliness);
    // NB: The loop an OSR compile enters has two ways in, so it has no preheader to hoist into.
    let no_licm = request.options.no_licm || request.osr.is_some();
    if no_licm {
        context.licm.home.clear();
        context.licm.home.resize(context.func.nodes.len(), ir::NONE);
    } else {
        context.licm.run(&context.func, &context.liveliness);
    }
    let gvn = request.options.gvn.then_some(&mut context.gvn);
    context.scheduler.run(
        &mut context.func,
        &mut context.liveliness,
        &context.licm.home,
        gvn,
        module.global_space.len(),
    );
    lap(1, &mut stats);
    let liveliness = &context.liveliness;
    if request.options.verify_ir
        && let Err(errors) = ir::verify::verify(&context.func, &|value| liveliness.is_dead(value), true)
    {
        panic!(
            "invalid IR after cleanup and scheduling of function {}:\n{errors}\n{}",
            request.function,
            ir::dump::dump(&context.func, &|value| liveliness.is_dead(value))
        );
    }
    context.allocator.allocate_with(
        &context.func,
        &context.liveliness,
        &mut context.allocation,
        request.options.stress_registers,
    );
    lap(3, &mut stats);
    if timed {
        // NB: Liveness runs first in the allocator, which times it.
        stats.ticks[2] = context.allocator.liveness_ticks;
        stats.ticks[3] = stats.ticks[3].saturating_sub(stats.ticks[2]);
    }
    if request.options.verify_ir
        && let Err(errors) = regalloc::verify::verify(&context.func, &context.liveliness, &context.allocation)
    {
        panic!(
            "invalid register allocation of function {}:\n{errors}\n{}\n{}",
            request.function,
            ir::dump::dump(&context.func, &|value| liveliness.is_dead(value)),
            regalloc::verify::dump(&context.func, &context.liveliness, &context.allocation)
        );
    }
    let generated = codegen::x86_64::generate(
        &context.func,
        &context.liveliness,
        &context.allocation,
        &request.runtime,
    )?;
    lap(4, &mut stats);
    stats.nodes = context.func.nodes.len() as u32;
    stats.slots = context.allocation.slot_count;
    stats.moves = context.allocation.moves.len() as u32;
    stats.hoisted = if no_licm { 0 } else { context.licm.hoisted };
    stats.eliminated = if request.options.gvn { context.gvn.eliminated } else { 0 };
    let mut compiled = CompiledFunction {
        code: generated.code,
        frame_size: generated.frame_size,
        stats,
        dump: None,
    };
    if request.options.dump_ir || request.options.dump_asm {
        let mut text = String::new();
        if request.options.dump_ir {
            text.push_str(&ir::dump::dump(&context.func, &|value| liveliness.is_dead(value)));
            text.push_str(&regalloc::verify::dump(
                &context.func,
                &context.liveliness,
                &context.allocation,
            ));
        }
        if request.options.dump_asm {
            for line in asm::disassembler::listing(asm::Architecture::X86_64, &compiled.code) {
                text.push_str(&format!("{:6x}  {}\n", line.offset, line.text));
            }
        }
        compiled.dump = Some(text);
    }
    Ok(compiled)
}
