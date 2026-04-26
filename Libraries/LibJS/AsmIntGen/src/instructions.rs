/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Canonical metadata for every DSL instruction.
//!
//! This table describes each DSL instruction's operand kinds, control-flow
//! semantics, and per-architecture register footprint (hidden clobbers,
//! implicit inputs/outputs, hard register requirements). The register
//! allocator for named temporaries consumes this metadata; it is the
//! canonical contract between the DSL surface and the codegen.
//!
//! ## What gets recorded here
//!
//! - **Operand kinds** -- the role of each named operand position in the DSL
//!   (read-only GPR, written GPR, FPR, immediate, memory, label, ...). The
//!   allocator reads this to derive use/def sets for liveness analysis.
//! - **`terminal`** -- whether control falls through to the next instruction.
//!   Terminal instructions (jmp, exit, dispatch_*, goto_handler,
//!   call_slow_path) end the basic block; the allocator does not extend any
//!   liveness past them.
//! - **`is_call`** -- whether the instruction performs a non-terminal C++
//!   call that clobbers every caller-saved register. The allocator must
//!   forbid keeping any temporary live across such instructions, since no
//!   register survives the call.
//! - **`clobbers_gpr` / `clobbers_fpr`** -- physical scratch registers used
//!   by codegen besides operand outputs. A live temp may not occupy these
//!   registers across the instruction.
//! - **`implicit_inputs` / `implicit_outputs`** -- hidden register operands
//!   not exposed in the DSL operand list. Examples: `call_helper` reads from
//!   t1 (rcx/x1) and writes to t0 (rax/x0); `divmod` writes its results to
//!   rax/rdx on x86_64 regardless of the named operands.
//! - **`fixed_operands`** -- a constraint that operand position N must
//!   resolve to a specific physical register (e.g., x86 shifts require the
//!   count register to be `rcx`).
//!
//! ## What is *not* recorded
//!
//! Codegen emit logic itself (the assembly text). The codegen functions
//! remain authoritative for the actual emitted bytes; this metadata only
//! describes what the allocator needs to know to feed them safely.
//!
//! ## Caller-saved register sets
//!
//! These are the registers an `is_call=true` instruction kills (in addition
//! to any explicitly listed clobbers). Pinned registers (pc, pb, values,
//! exec_ctx, dispatch, sp, fp) survive calls because they live in
//! callee-saved slots; aarch64 also pins x21 (ip cache), x22, x23, x24
//! (tag constants).
//!
//! - x86_64: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11, xmm0-xmm5
//! - aarch64: x0-x18, d0-d7, d16-d31

// Suppress dead-code warnings while the table lands independently of its
// consumers (the named-temporary register allocator).
#![allow(dead_code)]

use crate::registers::Arch;
use std::collections::HashMap;
use std::sync::OnceLock;

/// Kind of a single DSL operand position.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OperandKind {
    /// GPR read as input.
    GprIn,
    /// GPR written; previous contents discarded.
    GprOut,
    /// GPR both read and written.
    GprInOut,
    /// FP register read as input.
    FprIn,
    /// FP register written; previous contents discarded.
    FprOut,
    /// FP register both read and written.
    FprInOut,
    /// GPR (read) or immediate.
    GprInOrImm,
    /// Compile-time integer (immediate, named constant, or field reference).
    Imm,
    /// Memory operand `[base, ...]`. Reads the GPRs referenced inside the
    /// brackets; the memory itself may be loaded from or stored to depending
    /// on the mnemonic.
    Memory,
    /// Branch-target label (local `.foo` or global symbol).
    Label,
    /// Bytecode instruction field reference (`m_*`).
    FieldRef,
    /// Function symbol used by `call_*` instructions.
    FuncSymbol,
}

/// Per-architecture register footprint of an instruction.
#[derive(Debug, Clone, Copy)]
pub struct ArchSpec {
    /// Hidden GPR scratch clobbers (besides regs written via output operands
    /// and besides the all-caller-saved kill implied by `is_call=true`).
    pub clobbers_gpr: &'static [&'static str],
    /// Hidden FPR scratch clobbers.
    pub clobbers_fpr: &'static [&'static str],
    /// Hidden register inputs not visible in the operand list.
    /// E.g. `call_helper` reads its first argument from rcx (x1).
    pub implicit_inputs: &'static [&'static str],
    /// Hidden register outputs not visible in the operand list.
    /// E.g. `call_helper` returns its result in rax (x0).
    pub implicit_outputs: &'static [&'static str],
    /// Constraints requiring specific operand positions to resolve to fixed
    /// physical registers. Pairs of `(operand_index, physical_register)`.
    pub fixed_operands: &'static [(usize, &'static str)],
}

impl ArchSpec {
    pub const NONE: ArchSpec = ArchSpec {
        clobbers_gpr: &[],
        clobbers_fpr: &[],
        implicit_inputs: &[],
        implicit_outputs: &[],
        fixed_operands: &[],
    };
}

#[derive(Debug, Clone, Copy)]
pub struct InstructionInfo {
    pub mnemonic: &'static str,
    pub operands: &'static [OperandKind],
    /// Final operand kind that may repeat (variadic). `None` means fixed
    /// arity.
    pub variadic_kind: Option<OperandKind>,
    /// Control does not fall through to the next instruction.
    pub terminal: bool,
    /// Non-terminal C++ call. Implies all caller-saved temps are killed.
    pub is_call: bool,
    pub x86_64: ArchSpec,
    pub aarch64: ArchSpec,
}

impl InstructionInfo {
    /// Look up the per-arch spec for this instruction.
    pub fn arch(&self, arch: Arch) -> &ArchSpec {
        match arch {
            Arch::X86_64 => &self.x86_64,
            Arch::Aarch64 => &self.aarch64,
        }
    }
}

// Convenience constructors so the table stays readable.
const fn info(
    mnemonic: &'static str,
    operands: &'static [OperandKind],
    variadic_kind: Option<OperandKind>,
    terminal: bool,
    is_call: bool,
    x86_64: ArchSpec,
    aarch64: ArchSpec,
) -> InstructionInfo {
    InstructionInfo {
        mnemonic,
        operands,
        variadic_kind,
        terminal,
        is_call,
        x86_64,
        aarch64,
    }
}

const fn plain(
    mnemonic: &'static str,
    operands: &'static [OperandKind],
) -> InstructionInfo {
    info(mnemonic, operands, None, false, false, ArchSpec::NONE, ArchSpec::NONE)
}

use OperandKind::*;

/// The canonical table of every DSL instruction known to the codegen.
///
/// Keep entries grouped by category to mirror the reference comment in
/// `main.rs` (Control flow, C++ interop, Memory, Integer ALU, ...).
pub const INSTRUCTIONS: &[InstructionInfo] = &[
    // ------------------------------------------------------------------
    // Pseudo / control flow
    // ------------------------------------------------------------------
    // `label` is a pseudo-instruction emitted by the parser when it sees
    // `.foo:`; it never appears in user-written DSL.
    info("label", &[Label], None, false, false, ArchSpec::NONE, ArchSpec::NONE),

    // Unconditional branch within a handler.
    info("jmp", &[Label], None, true, false, ArchSpec::NONE, ArchSpec::NONE),

    // Exit the interpreter loop, returning to C++.
    info("exit", &[], None, true, false, ArchSpec::NONE, ArchSpec::NONE),

    // dispatch_next: advance pc by the handler's instruction size and
    // dispatch to the next bytecode handler. Terminal: the tail jump
    // through the dispatch table never falls through.
    info(
        "dispatch_next",
        &[],
        None,
        true,
        false,
        // x86_64: dispatch tail uses rax to hold the next opcode byte.
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        // aarch64: dispatch tail uses x9 (next opcode) and x10 (handler ptr).
        ArchSpec { clobbers_gpr: &["x9", "x10"], ..ArchSpec::NONE },
    ),

    // dispatch_variable: advance pc by an arbitrary amount and dispatch.
    info(
        "dispatch_variable",
        &[GprIn],
        None,
        true,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9", "x10"], ..ArchSpec::NONE },
    ),

    // dispatch_current: dispatch the instruction at the current pc without
    // advancing. On x86 this is a macro (load8 + indirect jmp); on aarch64
    // the codegen recognizes it directly.
    info(
        "dispatch_current",
        &[],
        None,
        true,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9", "x10"], ..ArchSpec::NONE },
    ),

    // goto_handler reg: set pc to `reg` (32-bit) and dispatch.
    info(
        "goto_handler",
        &[GprIn],
        None,
        true,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9", "x10"], ..ArchSpec::NONE },
    ),

    // ------------------------------------------------------------------
    // C++ interop
    // ------------------------------------------------------------------
    // call_slow_path func: i64 func(VM*, u32 pc). Terminal -- after the
    // return value is examined the handler either exits or dispatches to a
    // new pc; control never re-enters the DSL handler body.
    info(
        "call_slow_path",
        &[FuncSymbol],
        None,
        true,
        true,
        // Even though it's terminal, list known scratch (rcx for the state
        // reload, rax for the dispatch tail) so the table is precise.
        ArchSpec { clobbers_gpr: &["rax", "rcx"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9", "x10"], ..ArchSpec::NONE },
    ),

    // call_helper func [, input_temp, output_temp]
    //   u64 func(u64 value).
    //   1-operand form: reads from t1 / writes to t0 by convention.
    //   3-operand form: input_temp/output_temp are pinned to call/result
    //   registers. On x86_64 that is rcx -> rax to match the historical
    //   t1 -> t0 convention; on aarch64 both use x0, matching the ABI
    //   argument/result register and avoiding the old x1 -> x0 bridge move.
    //   Both forms still kill all caller-saved registers as per `is_call`.
    info(
        "call_helper",
        &[FuncSymbol, GprIn, GprOut],
        None,
        false,
        true,
        ArchSpec {
            implicit_inputs: &["rcx"],
            implicit_outputs: &["rax"],
            fixed_operands: &[(1, "rcx"), (2, "rax")],
            ..ArchSpec::NONE
        },
        ArchSpec {
            implicit_inputs: &["x0"],
            implicit_outputs: &["x0"],
            fixed_operands: &[(1, "x0"), (2, "x0")],
            ..ArchSpec::NONE
        },
    ),

    // call_interp func [, output_temp]
    //   i64 func(VM*, u32 pc). Result lands in t0; the named form pins
    //   that temp to t0 so the value can be carried into a downstream
    //   named-temp world.
    info(
        "call_interp",
        &[FuncSymbol, GprOut],
        None,
        false,
        true,
        ArchSpec {
            implicit_outputs: &["rax"],
            fixed_operands: &[(1, "rax")],
            ..ArchSpec::NONE
        },
        ArchSpec {
            implicit_outputs: &["x0"],
            fixed_operands: &[(1, "x0")],
            ..ArchSpec::NONE
        },
    ),

    // call_raw_native reg [, payload_temp, variant_idx_temp]
    //   Indirect call to a `ThrowCompletionOr<Value> (*)(VM&)`. Returns
    //   the variant payload in t0 and the variant index in t1. The named
    //   form pins those temps to t0/t1.
    info(
        "call_raw_native",
        &[GprIn, GprOut, GprOut],
        None,
        false,
        true,
        ArchSpec {
            clobbers_gpr: &["r11"],
            implicit_outputs: &["rax", "rcx"],
            fixed_operands: &[(1, "rax"), (2, "rcx")],
            ..ArchSpec::NONE
        },
        ArchSpec {
            clobbers_gpr: &["x9"],
            implicit_outputs: &["x0", "x1"],
            fixed_operands: &[(1, "x0"), (2, "x1")],
            ..ArchSpec::NONE
        },
    ),

    // reload_exec_ctx: refresh the exec_ctx register from VM* on the stack.
    info(
        "reload_exec_ctx",
        &[],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["rcx"], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),

    // load_vm dst: copy the hidden VM* into dst.
    plain("load_vm", &[GprOut]),

    // ------------------------------------------------------------------
    // Bytecode operand access
    // ------------------------------------------------------------------
    // load_operand dst, m_field: load the Value referenced by the u32
    // operand at m_field. On x86 the codegen materializes the operand
    // index in rax; on aarch64 it uses w9.
    info(
        "load_operand",
        &[GprOut, FieldRef],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // store_operand m_field, src: x86 normally uses r11 to materialize the
    // operand index, falling back to rax when src is itself r11 (so the
    // src register survives the index load). Only r11 is reported here as
    // the clobber: the rax fallback is reachable only when src is already
    // forbidden from being r11 by this entry.
    info(
        "store_operand",
        &[FieldRef, GprIn],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["r11"], ..ArchSpec::NONE },
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // load_label dst, m_field: read a 32-bit Label from the bytecode.
    plain("load_label", &[GprOut, FieldRef]),

    // ------------------------------------------------------------------
    // Memory access
    // ------------------------------------------------------------------
    // aarch64 needs x9 to materialize large offsets / non-imm12 displacements.
    info(
        "load64",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "load32",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "load16",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "load16s",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "load8",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "load8s",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "loadf32",
        &[FprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "store64",
        &[Memory, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        // Materializes large offsets or immediate values via x9.
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "store32",
        &[Memory, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "store16",
        &[Memory, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "store8",
        &[Memory, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "storef32",
        &[Memory, FprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // Pair load/store: aarch64 uses x10 when the address is not the cached
    // pb+pc tuple; x86 emits two sequential mov instructions.
    info(
        "load_pair64",
        &[GprOut, GprOut, Memory, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x10"], ..ArchSpec::NONE },
    ),
    info(
        "load_pair32",
        &[GprOut, GprOut, Memory, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x10"], ..ArchSpec::NONE },
    ),
    info(
        "store_pair64",
        &[Memory, Memory, GprIn, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x10"], ..ArchSpec::NONE },
    ),
    info(
        "store_pair32",
        &[Memory, Memory, GprIn, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x10"], ..ArchSpec::NONE },
    ),
    info(
        "inc32_mem",
        &[Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "lea",
        &[GprOut, Memory],
        None,
        false,
        false,
        ArchSpec::NONE,
        // Large offsets / non-power-of-two scales go through x9.
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),

    // ------------------------------------------------------------------
    // Integer ALU
    // ------------------------------------------------------------------
    info(
        "mov",
        &[GprOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    info(
        "movsxd",
        &[GprOut, GprIn],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    // Two-operand form: dst = dst OP src.
    info(
        "add",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "sub",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // mul: 2-operand (dst *= src) or 3-operand (dst = src * imm). On
    // x86 imul handles both forms; on aarch64 the 3-operand path needs x9
    // when the multiplier is an immediate, and the 2-operand path uses x9
    // for the smull-based overflow check.
    info(
        "mul",
        &[GprOut, GprIn],
        Some(GprInOrImm),
        false,
        false,
        // x86_64 imul: clobbers rax (movabs of multiplier when needed) -- in
        // practice the codegen only emits raw `imul` in the 3-operand form
        // and assumes the multiplier is a small immediate, so no scratch
        // is needed today. Leave empty.
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "neg",
        &[GprInOut],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    info(
        "not",
        &[GprInOut],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    info(
        "and",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "or",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "xor",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // Shifts: x86 variable shifts require the count register to be `cl`,
    // hence the fixed-operand constraint on operand 1. aarch64 has no such
    // constraint.
    info(
        "shl",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec { fixed_operands: &[(1, "rcx")], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),
    info(
        "shr",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec { fixed_operands: &[(1, "rcx")], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),
    info(
        "sar",
        &[GprInOut, GprInOrImm],
        None,
        false,
        false,
        ArchSpec { fixed_operands: &[(1, "rcx")], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),
    // divmod q, r, n, d: signed divide. On x86, idiv reads/writes rax/rdx,
    // so the named q/r operands MUST resolve to rax/rdx respectively.
    // aarch64 computes the quotient in x9 first so q may overlap n/d when
    // only the remainder is live.
    info(
        "divmod",
        &[GprOut, GprOut, GprIn, GprIn],
        None,
        false,
        false,
        ArchSpec {
            fixed_operands: &[(0, "rax"), (1, "rdx")],
            implicit_outputs: &["rax", "rdx"],
            ..ArchSpec::NONE
        },
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),

    // ------------------------------------------------------------------
    // 32-bit arithmetic with overflow detection
    // ------------------------------------------------------------------
    info(
        "add32_overflow",
        &[GprInOut, GprInOrImm, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "sub32_overflow",
        &[GprInOut, GprInOrImm, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "mul32_overflow",
        &[GprInOut, GprIn, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        // smull writes a full 64-bit result into the destination, then sxtw
        // into x9 for the round-trip overflow check.
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "neg32_overflow",
        &[GprInOut, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    info(
        "not32",
        &[GprInOut],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),

    // ------------------------------------------------------------------
    // NaN-boxing operations
    // ------------------------------------------------------------------
    plain("extract_tag", &[GprOut, GprIn]),
    plain("unbox_int32", &[GprOut, GprIn]),
    plain("unbox_object", &[GprOut, GprIn]),
    // box_int32 / box_int32_clean: x86 materializes the shifted tag in rax
    // and ORs it with dst. aarch64 emits movk directly into dst.
    info(
        "box_int32",
        &[GprOut, GprIn],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),
    info(
        "box_int32_clean",
        &[GprOut, GprIn],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["rax"], ..ArchSpec::NONE },
        ArchSpec::NONE,
    ),

    // ------------------------------------------------------------------
    // Bit manipulation
    // ------------------------------------------------------------------
    info(
        "toggle_bit",
        &[GprInOut, Imm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "clear_bit",
        &[GprInOut, Imm],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),

    // ------------------------------------------------------------------
    // Floating-point
    // ------------------------------------------------------------------
    plain("fp_add", &[FprInOut, FprIn]),
    plain("fp_sub", &[FprInOut, FprIn]),
    plain("fp_mul", &[FprInOut, FprIn]),
    plain("fp_div", &[FprInOut, FprIn]),
    plain("fp_sqrt", &[FprOut, FprIn]),
    plain("fp_floor", &[FprOut, FprIn]),
    plain("fp_ceil", &[FprOut, FprIn]),
    // fp_mov bridges GPR <-> FPR. The codegen looks at register names to
    // decide direction; the allocator just sees one read and one write.
    plain("fp_mov", &[GprOut, FprIn]),
    plain("int_to_double", &[FprOut, GprIn]),
    plain("float_to_double", &[FprOut, FprIn]),
    plain("double_to_float", &[FprOut, FprIn]),
    // double_to_int32 dst, fpr, fail_label
    // x86 cvttsd2si + round-trip check uses rcx and xmm3 as scratch.
    // aarch64 fcvtzs + scvtf round-trip uses d16 (outside the public FPR
    // pool, so no conflict) and no GPR scratch.
    info(
        "double_to_int32",
        &[GprOut, FprIn, Label],
        None,
        false,
        false,
        ArchSpec {
            clobbers_gpr: &["rcx"],
            clobbers_fpr: &["xmm3"],
            ..ArchSpec::NONE
        },
        ArchSpec { clobbers_fpr: &["d16"], ..ArchSpec::NONE },
    ),
    // js_to_int32: same shape on x86 (clobbers rcx). On aarch64 with
    // FEAT_JSCVT this is a single fjcvtzs; otherwise the fallback uses d16
    // for the round-trip check.
    info(
        "js_to_int32",
        &[GprOut, FprIn, Label],
        None,
        false,
        false,
        ArchSpec { clobbers_gpr: &["rcx"], ..ArchSpec::NONE },
        ArchSpec { clobbers_fpr: &["d16"], ..ArchSpec::NONE },
    ),
    // canonicalize_nan: the cold fixup path materializes CANON_NAN_BITS.
    // x86: movabs to dst (no extra scratch). aarch64: pinned d8 holds the
    // constant, so no scratch.
    plain("canonicalize_nan", &[GprOut, FprIn]),

    // ------------------------------------------------------------------
    // Branching
    // ------------------------------------------------------------------
    plain("branch_eq", &[GprIn, GprInOrImm, Label]),
    plain("branch_ne", &[GprIn, GprInOrImm, Label]),
    plain("branch_ge_unsigned", &[GprIn, GprInOrImm, Label]),
    plain("branch_lt_signed", &[GprIn, GprInOrImm, Label]),
    plain("branch_le_signed", &[GprIn, GprInOrImm, Label]),
    plain("branch_gt_signed", &[GprIn, GprInOrImm, Label]),
    plain("branch_ge_signed", &[GprIn, GprInOrImm, Label]),
    plain("branch_zero", &[GprIn, Label]),
    plain("branch_nonzero", &[GprIn, Label]),
    plain("branch_negative", &[GprIn, Label]),
    plain("branch_not_negative", &[GprIn, Label]),
    plain("branch_zero32", &[GprIn, Label]),
    plain("branch_nonzero32", &[GprIn, Label]),
    info(
        "branch_bits_set",
        &[GprIn, GprInOrImm, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        // Materializes non-logical-immediate masks via x9.
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "branch_bits_clear",
        &[GprIn, GprInOrImm, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    info(
        "branch_bit_set",
        &[GprIn, Imm, Label],
        None,
        false,
        false,
        ArchSpec::NONE,
        // Variable bit count goes through x9.
        ArchSpec { clobbers_gpr: &["x9"], ..ArchSpec::NONE },
    ),
    // branch_any_eq reg, v1, v2, ..., label -- variadic.
    info(
        "branch_any_eq",
        &[GprIn],
        Some(GprInOrImm),
        false,
        false,
        ArchSpec::NONE,
        ArchSpec::NONE,
    ),
    plain("branch_fp_unordered", &[FprIn, FprIn, Label]),
    plain("branch_fp_equal", &[FprIn, FprIn, Label]),
    plain("branch_fp_less", &[FprIn, FprIn, Label]),
    plain("branch_fp_less_or_equal", &[FprIn, FprIn, Label]),
    plain("branch_fp_greater", &[FprIn, FprIn, Label]),
    plain("branch_fp_greater_or_equal", &[FprIn, FprIn, Label]),
];

/// Lazily-built mnemonic -> info index. Populated on first lookup.
static INDEX: OnceLock<HashMap<&'static str, usize>> = OnceLock::new();

fn index() -> &'static HashMap<&'static str, usize> {
    INDEX.get_or_init(|| {
        let mut map = HashMap::with_capacity(INSTRUCTIONS.len());
        for (i, info) in INSTRUCTIONS.iter().enumerate() {
            let prev = map.insert(info.mnemonic, i);
            assert!(
                prev.is_none(),
                "duplicate InstructionInfo entry for '{}'",
                info.mnemonic
            );
        }
        map
    })
}

/// Look up the metadata for a DSL mnemonic. Returns `None` for macro
/// invocations and unknown names.
pub fn lookup(mnemonic: &str) -> Option<&'static InstructionInfo> {
    index().get(mnemonic).map(|&i| &INSTRUCTIONS[i])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every mnemonic the codegens dispatch on must have an entry here.
    /// When you add a new DSL instruction, add it to `INSTRUCTIONS` and
    /// to this list.
    const CODEGEN_MNEMONICS: &[&str] = &[
        "label",
        "jmp",
        "exit",
        "dispatch_next",
        "dispatch_variable",
        "dispatch_current",
        "goto_handler",
        "call_slow_path",
        "call_helper",
        "call_interp",
        "call_raw_native",
        "reload_exec_ctx",
        "load_vm",
        "load_operand",
        "store_operand",
        "load_label",
        "load64",
        "load32",
        "load16",
        "load16s",
        "load8",
        "load8s",
        "loadf32",
        "store64",
        "store32",
        "store16",
        "store8",
        "storef32",
        "load_pair64",
        "load_pair32",
        "store_pair64",
        "store_pair32",
        "inc32_mem",
        "lea",
        "mov",
        "movsxd",
        "add",
        "sub",
        "mul",
        "neg",
        "not",
        "and",
        "or",
        "xor",
        "shl",
        "shr",
        "sar",
        "divmod",
        "add32_overflow",
        "sub32_overflow",
        "mul32_overflow",
        "neg32_overflow",
        "not32",
        "extract_tag",
        "unbox_int32",
        "unbox_object",
        "box_int32",
        "box_int32_clean",
        "toggle_bit",
        "clear_bit",
        "fp_add",
        "fp_sub",
        "fp_mul",
        "fp_div",
        "fp_sqrt",
        "fp_floor",
        "fp_ceil",
        "fp_mov",
        "int_to_double",
        "float_to_double",
        "double_to_float",
        "double_to_int32",
        "js_to_int32",
        "canonicalize_nan",
        "branch_eq",
        "branch_ne",
        "branch_ge_unsigned",
        "branch_lt_signed",
        "branch_le_signed",
        "branch_gt_signed",
        "branch_ge_signed",
        "branch_zero",
        "branch_nonzero",
        "branch_negative",
        "branch_not_negative",
        "branch_zero32",
        "branch_nonzero32",
        "branch_bits_set",
        "branch_bits_clear",
        "branch_bit_set",
        "branch_any_eq",
        "branch_fp_unordered",
        "branch_fp_equal",
        "branch_fp_less",
        "branch_fp_less_or_equal",
        "branch_fp_greater",
        "branch_fp_greater_or_equal",
    ];

    #[test]
    fn every_codegen_mnemonic_is_in_the_table() {
        for m in CODEGEN_MNEMONICS {
            assert!(lookup(m).is_some(), "missing InstructionInfo for '{m}'");
        }
    }

    #[test]
    fn no_orphan_table_entries() {
        use std::collections::HashSet;
        let known: HashSet<&str> = CODEGEN_MNEMONICS.iter().copied().collect();
        for info in INSTRUCTIONS {
            assert!(
                known.contains(info.mnemonic),
                "table entry '{}' is not declared in CODEGEN_MNEMONICS",
                info.mnemonic
            );
        }
    }

    #[test]
    fn table_entries_are_unique() {
        // Force the lazy index to build, which asserts uniqueness.
        let _ = index();
    }

    #[test]
    fn calls_imply_non_terminal_or_terminal_consistently() {
        // call_slow_path is the one call that is also terminal.
        for info in INSTRUCTIONS {
            if info.is_call && info.terminal {
                assert_eq!(
                    info.mnemonic, "call_slow_path",
                    "unexpected terminal call: {}",
                    info.mnemonic
                );
            }
        }
    }
}
