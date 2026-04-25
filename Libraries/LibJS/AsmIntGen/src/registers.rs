/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arch {
    X86_64,
    Aarch64,
}

/// DSL register names and their platform mappings.
///
/// All of pc, pb, values, exec_ctx, dispatch are callee-saved so they
/// survive C++ calls.
///
/// `temporaries` and `fp_temporaries` are the public DSL temp pool: every
/// listed register is exposed as `tN`/`ftN` and is available to the
/// register allocator for named temporaries. Registers that the codegen
/// itself uses as hidden scratch are deliberately *excluded* from these
/// lists so they cannot collide with user-visible names.
///
/// Reserved codegen scratch (not in the temp pool):
/// - x86_64: none -- the codegen's hidden scratch (rax, rcx, rdx, r11,
///   xmm3) overlaps with t0/t1/t2/t8/ft3, and the per-instruction
///   metadata in `instructions.rs` records exactly when each is killed.
///   The named-temp allocator avoids those registers across the affected
///   instructions; positional `tN` users are responsible for knowing the
///   convention.
/// - aarch64: x9, x10 are universal scratch in the codegen (large-imm
///   materialization, dispatch tail, pair-memory base computation), and
///   d16 is FPR scratch for double-to-int32 round-trip checks. x21
///   caches `pb + pc` for fast dispatch; x22/x23/x24 hold pinned tag
///   constants. None of these are addressable by DSL name.
pub struct RegisterMapping {
    pub pc: &'static str,
    pub pb: &'static str,
    pub values: &'static str,
    pub exec_ctx: &'static str,
    pub dispatch: &'static str,
    pub temporaries: &'static [&'static str],
    pub fp_temporaries: &'static [&'static str],
    pub sp: &'static str,
    pub fp: &'static str,
}

pub const X86_64_REGS: RegisterMapping = RegisterMapping {
    pc: "r13",
    pb: "r14",
    values: "r15",
    exec_ctx: "rbx",
    dispatch: "r12",
    temporaries: &["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"],
    fp_temporaries: &["xmm0", "xmm1", "xmm2", "xmm3"],
    sp: "rsp",
    fp: "rbp",
};

pub const AARCH64_REGS: RegisterMapping = RegisterMapping {
    pc: "x25",
    pb: "x26",
    values: "x27",
    exec_ctx: "x28",
    dispatch: "x19",
    // x9 and x10 are reserved as codegen scratch (see the comment on
    // RegisterMapping). The public pool is t0..t8 = x0..x8, matching the
    // x86_64 temp count.
    temporaries: &["x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8"],
    fp_temporaries: &["d0", "d1", "d2", "d3"],
    sp: "sp",
    fp: "x29",
};

pub fn mapping_for(arch: Arch) -> &'static RegisterMapping {
    match arch {
        Arch::X86_64 => &X86_64_REGS,
        Arch::Aarch64 => &AARCH64_REGS,
    }
}

/// Resolve a DSL register name to a platform register name. Only the
/// pinned names are publicly addressable; named temporaries declared via
/// `temp` / `ftemp` are mapped to physical registers by the allocator
/// and substituted into the IR before this function is reached.
pub fn resolve_register(name: &str, arch: Arch) -> Option<String> {
    let m = mapping_for(arch);
    match name {
        "pc" => Some(m.pc.to_string()),
        "pb" => Some(m.pb.to_string()),
        "values" => Some(m.values.to_string()),
        "exec_ctx" => Some(m.exec_ctx.to_string()),
        "dispatch" => Some(m.dispatch.to_string()),
        "sp" => Some(m.sp.to_string()),
        "fp" => Some(m.fp.to_string()),
        _ => None,
    }
}

/// Encoding-cost score for a physical register. Lower is cheaper, so the
/// allocator should prefer assigning hot named temporaries to low-cost
/// registers.
///
/// On x86_64:
///   * `rax` is the cheapest. Many arithmetic and logical instructions have
///     a 1-byte-shorter accumulator-form encoding when the destination is
///     `rax`/`eax` (e.g. `add eax, imm32` is one byte shorter than
///     `add r/m32, imm32`).
///   * The remaining "classic" GPRs (`rcx`, `rdx`, `rbx`, `rsi`, `rdi`)
///     fit in three encoding bits, so 32-bit forms do not need a REX
///     prefix at all -- saving one byte per instruction compared to the
///     "modern" GPRs.
///   * `r8`..`r15` always need REX.B (or REX.R) to extend the register
///     field, so they are the most expensive to encode.
///
/// On aarch64 every general-purpose register encodes the same way, so the
/// cost is uniformly zero.
pub fn register_cost(name: &str, arch: Arch) -> u32 {
    match arch {
        Arch::X86_64 => match name {
            "rax" | "eax" => 0,
            "rcx" | "rdx" | "rbx" | "rsi" | "rdi" | "ecx" | "edx" | "ebx" | "esi" | "edi" => 1,
            _ => 2,
        },
        Arch::Aarch64 => 0,
    }
}
