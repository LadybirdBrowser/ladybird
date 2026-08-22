/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target register sets and interpreter register conventions.

use crate::Architecture;
use crate::intrinsic::IntegerWidth;
use crate::types::InterpreterRegister;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum RegisterClass {
    GeneralPurpose,
    FloatingPoint,
}

/// A physical register belonging to one target architecture.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct PhysicalRegister {
    architecture: Architecture,
    class: RegisterClass,
    name: &'static str,
    number: u8,
    allocation_cost: u8,
}

impl PhysicalRegister {
    const fn new(
        architecture: Architecture,
        class: RegisterClass,
        name: &'static str,
        number: u8,
        allocation_cost: u8,
    ) -> Self {
        Self {
            architecture,
            class,
            name,
            number,
            allocation_cost,
        }
    }

    pub(crate) fn architecture(self) -> Architecture {
        self.architecture
    }

    pub(crate) fn class(self) -> RegisterClass {
        self.class
    }

    pub(crate) fn as_str(self) -> &'static str {
        self.name
    }

    pub(crate) fn byte_name(self) -> String {
        const NAMES: [&str; 16] = [
            "al", "cl", "dl", "bl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b", "spl",
            "bpl",
        ];
        NAMES[self.number as usize].to_string()
    }

    pub(crate) fn half_word_name(self) -> String {
        const NAMES: [&str; 16] = [
            "ax", "cx", "dx", "bx", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w", "sp",
            "bp",
        ];
        NAMES[self.number as usize].to_string()
    }

    pub(crate) fn word_name(self) -> String {
        if self.architecture == Architecture::Aarch64 {
            return match self.name {
                "sp" => "wsp".to_string(),
                "xzr" => "wzr".to_string(),
                _ => format!("w{}", self.number),
            };
        }
        const NAMES: [&str; 16] = [
            "eax", "ecx", "edx", "ebx", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
            "esp", "ebp",
        ];
        NAMES[self.number as usize].to_string()
    }

    pub(crate) fn single_name(self) -> String {
        format!("s{}", self.number)
    }

    pub(crate) fn integer_name(self, width: IntegerWidth) -> String {
        match (self.architecture, width) {
            (_, IntegerWidth::U64) => self.as_str().to_string(),
            (Architecture::Aarch64, _) | (_, IntegerWidth::U32) => self.word_name(),
            (_, IntegerWidth::U16) => self.half_word_name(),
            (_, IntegerWidth::U8) => self.byte_name(),
        }
    }

    /// The architecture's register encoding number.
    ///
    /// AArch64 deliberately gives SP and XZR the same number. XZR is introduced
    /// only during finalization, after allocator register sets are gone.
    pub(crate) fn number(self) -> u8 {
        self.number
    }

    /// Relative encoding cost used to break allocation ties.
    pub(crate) fn allocation_cost(self) -> u32 {
        u32::from(self.allocation_cost)
    }
}

impl fmt::Display for PhysicalRegister {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.name.fmt(formatter)
    }
}

macro_rules! define_registers {
    ($($name:ident = $register:expr;)*) => {
        $(pub(crate) const $name: PhysicalRegister = $register;)*
        #[cfg(test)]
        pub(crate) const ALL: &[PhysicalRegister] = &[$($name),*];
    };
}

pub(crate) mod x86_64 {
    use super::{PhysicalRegister, RegisterClass};
    use crate::Architecture;

    // The accumulator has shorter immediate encodings, classic GPRs avoid
    // a REX prefix in 32-bit forms, and extended GPRs always require one.
    const fn gpr(name: &'static str, number: u8, allocation_cost: u8) -> PhysicalRegister {
        PhysicalRegister::new(
            Architecture::X86_64,
            RegisterClass::GeneralPurpose,
            name,
            number,
            allocation_cost,
        )
    }

    const fn fpr(name: &'static str, number: u8) -> PhysicalRegister {
        PhysicalRegister::new(Architecture::X86_64, RegisterClass::FloatingPoint, name, number, 2)
    }

    define_registers! {
        RAX = gpr("rax", 0, 0);
        RCX = gpr("rcx", 1, 1);
        RDX = gpr("rdx", 2, 1);
        RBX = gpr("rbx", 3, 1);
        RSI = gpr("rsi", 4, 1);
        RDI = gpr("rdi", 5, 1);
        R8 = gpr("r8", 6, 2);
        R9 = gpr("r9", 7, 2);
        R10 = gpr("r10", 8, 2);
        R11 = gpr("r11", 9, 2);
        R12 = gpr("r12", 10, 2);
        R13 = gpr("r13", 11, 2);
        R14 = gpr("r14", 12, 2);
        R15 = gpr("r15", 13, 2);
        RSP = gpr("rsp", 14, 2);
        RBP = gpr("rbp", 15, 2);
        XMM0 = fpr("xmm0", 0);
        XMM1 = fpr("xmm1", 1);
        XMM2 = fpr("xmm2", 2);
        XMM3 = fpr("xmm3", 3);
        XMM4 = fpr("xmm4", 4);
        XMM5 = fpr("xmm5", 5);
    }
}

pub(crate) mod aarch64 {
    use super::{PhysicalRegister, RegisterClass};
    use crate::Architecture;

    const fn gpr(name: &'static str, number: u8) -> PhysicalRegister {
        PhysicalRegister::new(Architecture::Aarch64, RegisterClass::GeneralPurpose, name, number, 0)
    }

    const fn fpr(name: &'static str, number: u8) -> PhysicalRegister {
        PhysicalRegister::new(Architecture::Aarch64, RegisterClass::FloatingPoint, name, number, 0)
    }

    define_registers! {
        X0 = gpr("x0", 0);
        X1 = gpr("x1", 1);
        X2 = gpr("x2", 2);
        X3 = gpr("x3", 3);
        X4 = gpr("x4", 4);
        X5 = gpr("x5", 5);
        X6 = gpr("x6", 6);
        X7 = gpr("x7", 7);
        X8 = gpr("x8", 8);
        X9 = gpr("x9", 9);
        X10 = gpr("x10", 10);
        X11 = gpr("x11", 11);
        X12 = gpr("x12", 12);
        X13 = gpr("x13", 13);
        X14 = gpr("x14", 14);
        X15 = gpr("x15", 15);
        X16 = gpr("x16", 16);
        X17 = gpr("x17", 17);
        X19 = gpr("x19", 19);
        X20 = gpr("x20", 20);
        X21 = gpr("x21", 21);
        X22 = gpr("x22", 22);
        X23 = gpr("x23", 23);
        X24 = gpr("x24", 24);
        X25 = gpr("x25", 25);
        X26 = gpr("x26", 26);
        X27 = gpr("x27", 27);
        X28 = gpr("x28", 28);
        X29 = gpr("x29", 29);
        SP = gpr("sp", 31);
        XZR = gpr("xzr", 31);
        D0 = fpr("d0", 0);
        D1 = fpr("d1", 1);
        D2 = fpr("d2", 2);
        D3 = fpr("d3", 3);
        D4 = fpr("d4", 4);
        D5 = fpr("d5", 5);
        D6 = fpr("d6", 6);
        D7 = fpr("d7", 7);
        D8 = fpr("d8", 8);
        D16 = fpr("d16", 16);
    }
}

/// Interpreter register conventions and allocatable target registers.
///
/// All pinned registers are callee-saved so they survive C++ calls. On
/// x86-64, rbx holds `values`, r15 holds the heap region base, and `exec_ctx`
/// is derived by subtracting the fixed execution-context size. AArch64 keeps
/// the heap region base in x24.
///
/// `temporaries` and `fp_temporaries` are the allocatable register pools.
/// Registers used as hidden instruction-expansion scratch are deliberately
/// excluded from these lists.
///
/// Reserved instruction-expansion scratch:
/// - x86_64: none. The per-instruction metadata records exactly when
///   allocatable scratch registers are killed, and the allocator avoids
///   them across affected instructions.
/// - aarch64: x9, x10 are universal scratch in the codegen (large-imm
///   materialization, dispatch tail, pair-memory base computation), and
///   d16 is FPR scratch for double-to-int32 round-trip checks. x21
///   caches `pb + pc` for fast dispatch; x22/x23 hold pinned tag constants,
///   and x24 holds the heap region base. None of these are addressable by DSL
///   name.
pub(crate) struct TargetRegisterInfo {
    pub(crate) interpreter: [Option<PhysicalRegister>; InterpreterRegister::COUNT],
    pub(crate) temporaries: &'static [PhysicalRegister],
    pub(crate) fp_temporaries: &'static [PhysicalRegister],
    pub(crate) caller_saved_gpr: &'static [PhysicalRegister],
    pub(crate) caller_saved_fpr: &'static [PhysicalRegister],
}

pub(crate) const X86_64_REGS: TargetRegisterInfo = TargetRegisterInfo {
    interpreter: [
        Some(x86_64::R13),
        Some(x86_64::R14),
        Some(x86_64::RBX),
        Some(x86_64::RBX),
        Some(x86_64::R12),
        Some(x86_64::R15),
        Some(x86_64::RSP),
        Some(x86_64::RBP),
    ],
    temporaries: &[
        x86_64::RAX,
        x86_64::RCX,
        x86_64::RDX,
        x86_64::RSI,
        x86_64::RDI,
        x86_64::R8,
        x86_64::R9,
        x86_64::R10,
        x86_64::R11,
    ],
    fp_temporaries: &[x86_64::XMM0, x86_64::XMM1, x86_64::XMM2, x86_64::XMM3],
    caller_saved_gpr: &[
        x86_64::RAX,
        x86_64::RCX,
        x86_64::RDX,
        x86_64::RSI,
        x86_64::RDI,
        x86_64::R8,
        x86_64::R9,
        x86_64::R10,
        x86_64::R11,
    ],
    caller_saved_fpr: &[
        x86_64::XMM0,
        x86_64::XMM1,
        x86_64::XMM2,
        x86_64::XMM3,
        x86_64::XMM4,
        x86_64::XMM5,
    ],
};

pub(crate) const AARCH64_REGS: TargetRegisterInfo = TargetRegisterInfo {
    interpreter: [
        Some(aarch64::X25),
        Some(aarch64::X26),
        Some(aarch64::X27),
        Some(aarch64::X28),
        Some(aarch64::X19),
        Some(aarch64::X24),
        Some(aarch64::SP),
        Some(aarch64::X29),
    ],
    // x9 and x10 are reserved as instruction-expansion scratch, and x16 and
    // x17 are the procedure call standard's veneer registers, which the
    // assembler materializes call targets and awkward immediates into. Every
    // other caller-saved register is free, and larger functions can have far
    // more live at once than the single handler this pool was sized for.
    temporaries: &[
        aarch64::X0,
        aarch64::X1,
        aarch64::X2,
        aarch64::X3,
        aarch64::X4,
        aarch64::X5,
        aarch64::X6,
        aarch64::X7,
        aarch64::X8,
        aarch64::X11,
        aarch64::X12,
        aarch64::X13,
        aarch64::X14,
        aarch64::X15,
    ],
    fp_temporaries: &[aarch64::D0, aarch64::D1, aarch64::D2, aarch64::D3],
    caller_saved_gpr: &[
        aarch64::X0,
        aarch64::X1,
        aarch64::X2,
        aarch64::X3,
        aarch64::X4,
        aarch64::X5,
        aarch64::X6,
        aarch64::X7,
        aarch64::X8,
        aarch64::X9,
        aarch64::X10,
        aarch64::X11,
        aarch64::X12,
        aarch64::X13,
        aarch64::X14,
        aarch64::X15,
        aarch64::X16,
        aarch64::X17,
    ],
    caller_saved_fpr: &[
        aarch64::D0,
        aarch64::D1,
        aarch64::D2,
        aarch64::D3,
        aarch64::D4,
        aarch64::D5,
        aarch64::D6,
        aarch64::D7,
    ],
};

pub(crate) fn register_info_for(arch: Architecture) -> &'static TargetRegisterInfo {
    match arch {
        Architecture::X86_64 => &X86_64_REGS,
        Architecture::Aarch64 => &AARCH64_REGS,
    }
}

pub(crate) fn resolve_interpreter_register(
    register: InterpreterRegister,
    arch: Architecture,
) -> Option<PhysicalRegister> {
    register_info_for(arch).interpreter[register as usize]
}

#[cfg(test)]
pub(crate) fn physical_register_named(name: &str, arch: Architecture) -> Option<PhysicalRegister> {
    let registers: &[PhysicalRegister] = match arch {
        Architecture::X86_64 => x86_64::ALL,
        Architecture::Aarch64 => aarch64::ALL,
    };
    registers.iter().copied().find(|register| register.as_str() == name)
}

#[cfg(test)]
mod tests {
    use super::{aarch64, resolve_interpreter_register, x86_64};
    use crate::Architecture;
    use crate::types::InterpreterRegister;

    #[test]
    fn keeps_allocation_costs_on_typed_registers() {
        assert_eq!(x86_64::RAX.allocation_cost(), 0);
        assert_eq!(x86_64::RCX.allocation_cost(), 1);
        assert_eq!(x86_64::R11.allocation_cost(), 2);
        assert_eq!(x86_64::XMM0.allocation_cost(), 2);
        assert_eq!(aarch64::X0.allocation_cost(), 0);
        assert_eq!(aarch64::D0.allocation_cost(), 0);
    }

    #[test]
    fn pins_heap_region_base_in_a_callee_saved_register() {
        assert_eq!(
            resolve_interpreter_register(InterpreterRegister::HeapRegionBase, Architecture::X86_64),
            Some(x86_64::R15)
        );
        assert_eq!(
            resolve_interpreter_register(InterpreterRegister::HeapRegionBase, Architecture::Aarch64),
            Some(aarch64::X24)
        );
    }
}
