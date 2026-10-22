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
#[cfg(target_arch = "x86_64")]
pub mod stubs;
#[cfg(target_arch = "aarch64")]
#[path = "stubs_aarch64.rs"]
pub mod stubs;
pub mod table;

use libwasm_module::Module;
use std::sync::Arc;

/// Why a function was not compiled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompileFailure {
    /// An OSR compile at a loop it cannot enter, at module offset `pc`: one that is unreachable, or in a legacy
    /// handler, whose exception the interpreter frame has no value for. The frame goes on in the interpreter.
    NoOsrEntry { pc: u32 },
    /// The assembler could not finish the code.
    CodeGeneration,
}

impl CompileFailure {
    pub fn reason(self) -> &'static str {
        match self {
            CompileFailure::NoOsrEntry { .. } => "no OSR entry at the loop",
            CompileFailure::CodeGeneration => "code generation failed",
        }
    }
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
    /// Refuse OSR compiles at loops in no other loop, so that tests reach the loops in them.
    pub stress_nested_osr: bool,
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
    /// `wasm_runtime_allocate`, `wasm_runtime_wait` and `wasm_runtime_notify`.
    pub allocate: u64,
    pub wait: u64,
    pub notify: u64,
    /// `wasm_runtime_allocate_exception` (`Helper::AllocateException`), and `wasm_runtime_throw_from_native`, which
    /// takes an exception and the fp of the frame that throws it, unwinds the exception, and returns the context,
    /// whose `resume` says where to continue.
    pub allocate_exception: u64,
    pub throw: u64,
}

/// What to compile.
pub struct CompileRequest {
    pub module: Arc<Module>,
    /// The function, counting imports.
    pub function: u32,
    pub runtime: RuntimeInfo,
    pub options: Options,
    /// For an OSR compile, the module offset of the loop that it continues an interpreter frame at, in any number of
    /// other loops. Its code takes the frame's fp as its only argument and returns the function's results.
    pub osr: Option<u32>,
}

/// The reusable state of one compile thread: every phase's storage, cleared between compiles.
#[derive(Default)]
pub struct CompileContext {
    pub func: ir::Func,
    builder: builder::Builder,
    liveliness: passes::cleanup::Liveliness,
    licm: passes::licm::Licm,
    scheduler: passes::schedule::Scheduler,
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
}

/// The time stamp counter, for the phase timers.
pub fn ticks() -> u64 {
    #[cfg(target_arch = "x86_64")]
    // SAFETY: rdtsc only reads a counter.
    unsafe {
        core::arch::x86_64::_rdtsc()
    }
    #[cfg(target_arch = "aarch64")]
    {
        let counter: u64;
        // SAFETY: Reading the virtual counter has no effects.
        unsafe { core::arch::asm!("mrs {}, cntvct_el0", out(reg) counter, options(nomem, nostack)) };
        counter
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    0
}

/// The result of a compile.
#[derive(Debug, Default)]
pub struct CompiledFunction {
    /// Position-independent machine code; the entry point is its start.
    pub code: Vec<u8>,
    /// The bytes between fp and rsp in the function's frame.
    pub frame_size: u32,
    /// The offsets of the return addresses of the calls that handle exceptions: an exception the runtime unwinds to
    /// one of them continues there, with the context's `exception` set, as if the call had returned.
    pub exception_sites: Vec<u32>,
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
        None => context.builder.build(module, request.function, &mut context.func),
        Some(pc) => {
            let Some(entry) = context
                .builder
                .probe_osr(module, request.function, pc, &mut context.func)
                .filter(|(_, around)| !request.options.stress_nested_osr || !around.is_empty())
            else {
                return Err(CompileFailure::NoOsrEntry { pc });
            };
            context
                .builder
                .build_osr(module, request.function, pc, entry, &mut context.func);
        }
    }
    lap(0, &mut stats);
    if request.options.verify_ir
        && let Err(errors) = ir::verify::verify(&context.func, false)
    {
        panic!(
            "invalid IR after building function {}:\n{errors}\n{}",
            request.function,
            ir::dump::dump(&context.func)
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
    context
        .scheduler
        .run(&mut context.func, &mut context.liveliness, &context.licm.home);
    lap(1, &mut stats);
    if request.options.verify_ir
        && let Err(errors) = ir::verify::verify(&context.func, true)
    {
        panic!(
            "invalid IR after cleanup and scheduling of function {}:\n{errors}\n{}",
            request.function,
            ir::dump::dump(&context.func)
        );
    }
    context
        .allocator
        .allocate(&context.func, &mut context.allocation, request.options.stress_registers);
    lap(3, &mut stats);
    if timed {
        // NB: Liveness runs first in the allocator, which times it.
        stats.ticks[2] = context.allocator.liveness_ticks;
        stats.ticks[3] = stats.ticks[3].saturating_sub(stats.ticks[2]);
    }
    if request.options.verify_ir
        && let Err(errors) = regalloc::verify::verify(&context.func, &context.allocation)
    {
        panic!(
            "invalid register allocation of function {}:\n{errors}\n{}\n{}",
            request.function,
            ir::dump::dump(&context.func),
            regalloc::verify::dump(&context.func, &context.allocation)
        );
    }
    let generated = codegen::target::generate(&context.func, &context.allocation, &request.runtime)?;
    lap(4, &mut stats);
    stats.nodes = context.func.nodes.len() as u32;
    stats.slots = context.allocation.slot_count;
    stats.moves = context.allocation.moves.len() as u32;
    stats.hoisted = if no_licm { 0 } else { context.licm.hoisted };
    let mut compiled = CompiledFunction {
        code: generated.code,
        frame_size: generated.frame_size,
        exception_sites: generated.exception_sites,
        stats,
        dump: None,
    };
    if request.options.dump_ir || request.options.dump_asm {
        let mut text = String::new();
        if request.options.dump_ir {
            text.push_str(&ir::dump::dump(&context.func));
            text.push_str(&regalloc::verify::dump(&context.func, &context.allocation));
        }
        if request.options.dump_asm {
            let architecture = if cfg!(target_arch = "aarch64") {
                asm::Architecture::AArch64
            } else {
                asm::Architecture::X86_64
            };
            for line in asm::disassembler::listing(architecture, &compiled.code) {
                text.push_str(&format!("{:6x}  {}\n", line.offset, line.text));
            }
        }
        compiled.dump = Some(text);
    }
    Ok(compiled)
}
