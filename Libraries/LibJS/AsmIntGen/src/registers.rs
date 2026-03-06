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
/// All of pc, pb, values, exec_ctx, dispatch are callee-saved so they survive C++ calls.
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
    temporaries: &[
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
        "x14", "x15", "x16", "x17",
    ],
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

/// Resolve a DSL register name to a platform register name.
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
        _ => {
            // t0-t9 -> temporaries
            if let Some(idx_str) = name.strip_prefix('t') {
                if let Ok(idx) = idx_str.parse::<usize>() {
                    if idx < m.temporaries.len() {
                        return Some(m.temporaries[idx].to_string());
                    }
                }
            }
            // ft0-ft3 -> fp temporaries
            if let Some(idx_str) = name.strip_prefix("ft") {
                if let Ok(idx) = idx_str.parse::<usize>() {
                    if idx < m.fp_temporaries.len() {
                        return Some(m.fp_temporaries[idx].to_string());
                    }
                }
            }
            None
        }
    }
}
