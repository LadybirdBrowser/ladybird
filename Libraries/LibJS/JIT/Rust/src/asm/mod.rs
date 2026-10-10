/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Machine code assemblers for x86-64 and AArch64, plus the portable
//! `MacroAssembler` that codegen targets.
//!
//! Both encoders are always compiled (they only produce bytes), so their tests
//! run on any host. `MacroAssembler` is the one for the host architecture.

pub mod aarch64;
pub mod buffer;
mod macro_assembler;
pub mod x86_64;

pub use buffer::AsmError;
pub use buffer::Label;
pub use macro_assembler::MachineFrame;
pub use macro_assembler::PortableMacroAssembler;

#[cfg(target_arch = "aarch64")]
pub type MacroAssembler = aarch64::MacroAssembler;
#[cfg(target_arch = "x86_64")]
pub type MacroAssembler = x86_64::MacroAssembler;

pub mod disassembler;
#[cfg(all(test, target_arch = "x86_64"))]
pub(crate) mod executable_code;
#[cfg(all(test, target_arch = "x86_64"))]
mod execution_tests;

/// The instruction set an assembler emits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Architecture {
    X86_64,
    AArch64,
}

/// A general purpose register, by its hardware encoding.
///
/// On AArch64, encoding 31 means either the stack pointer or the zero register
/// depending on the instruction; the encoders document which. In the
/// `MacroAssembler` layer it always means the stack pointer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Gpr(pub u8);

/// A floating point / vector register, by its hardware encoding.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Fpr(pub u8);

macro_rules! register_set {
    ($name:ident, $register:ident) => {
        /// A set of registers, as a bit mask over their encodings.
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
        pub struct $name(pub u32);

        impl $name {
            pub const EMPTY: Self = Self(0);

            pub const fn of(registers: &[$register]) -> Self {
                let mut bits = 0;
                let mut i = 0;
                while i < registers.len() {
                    bits |= 1 << registers[i].0;
                    i += 1;
                }
                Self(bits)
            }

            pub const fn contains(self, register: $register) -> bool {
                self.0 & (1 << register.0) != 0
            }

            pub const fn with(self, register: $register) -> Self {
                Self(self.0 | (1 << register.0))
            }

            pub const fn without(self, register: $register) -> Self {
                Self(self.0 & !(1 << register.0))
            }

            pub const fn union(self, other: Self) -> Self {
                Self(self.0 | other.0)
            }

            pub const fn intersection(self, other: Self) -> Self {
                Self(self.0 & other.0)
            }

            pub const fn difference(self, other: Self) -> Self {
                Self(self.0 & !other.0)
            }

            pub const fn is_empty(self) -> bool {
                self.0 == 0
            }

            pub const fn len(self) -> usize {
                self.0.count_ones() as usize
            }

            /// The registers in ascending encoding order.
            pub fn iter(self) -> impl DoubleEndedIterator<Item = $register> + Clone {
                (0..32u8).filter(move |i| self.0 & (1 << i) != 0).map($register)
            }
        }
    };
}

register_set!(GprSet, Gpr);
register_set!(FprSet, Fpr);

/// The index scale of a memory operand.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Scale {
    One = 0,
    Two = 1,
    Four = 2,
    Eight = 3,
}

impl Scale {
    pub const fn log2(self) -> u8 {
        self as u8
    }
}

/// A portable memory operand: `[base + index * scale + displacement]`.
///
/// x86-64 encodes every `Address` directly. AArch64 legalizes the ones it
/// cannot encode through a reserved scratch register.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Address {
    pub base: Gpr,
    pub index: Option<(Gpr, Scale)>,
    pub displacement: i32,
}

impl Address {
    pub const fn new(base: Gpr, displacement: i32) -> Self {
        Self {
            base,
            index: None,
            displacement,
        }
    }

    pub const fn indexed(base: Gpr, index: Gpr, scale: Scale, displacement: i32) -> Self {
        Self {
            base,
            index: Some((index, scale)),
            displacement,
        }
    }
}

/// Operand width of an integer instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Width {
    W32,
    W64,
}

impl Width {
    pub const fn bits(self) -> u32 {
        match self {
            Width::W32 => 32,
            Width::W64 => 64,
        }
    }
}

/// A portable integer condition, evaluated on the flags set by a compare
/// (`lhs` against `rhs`), a test (`value & mask`), or an arithmetic operation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Condition {
    Equal,
    NotEqual,
    /// Signed comparisons.
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    /// Unsigned comparisons.
    Below,
    BelowOrEqual,
    Above,
    AboveOrEqual,
    /// Signed overflow of the last arithmetic operation.
    Overflow,
    NoOverflow,
    /// The result is zero / not zero. Same flags as `Equal` / `NotEqual`.
    Zero,
    NonZero,
    /// The sign bit of the result is set / clear.
    Negative,
    PositiveOrZero,
}

impl Condition {
    pub const fn invert(self) -> Self {
        match self {
            Condition::Equal => Condition::NotEqual,
            Condition::NotEqual => Condition::Equal,
            Condition::LessThan => Condition::GreaterThanOrEqual,
            Condition::LessThanOrEqual => Condition::GreaterThan,
            Condition::GreaterThan => Condition::LessThanOrEqual,
            Condition::GreaterThanOrEqual => Condition::LessThan,
            Condition::Below => Condition::AboveOrEqual,
            Condition::BelowOrEqual => Condition::Above,
            Condition::Above => Condition::BelowOrEqual,
            Condition::AboveOrEqual => Condition::Below,
            Condition::Overflow => Condition::NoOverflow,
            Condition::NoOverflow => Condition::Overflow,
            Condition::Zero => Condition::NonZero,
            Condition::NonZero => Condition::Zero,
            Condition::Negative => Condition::PositiveOrZero,
            Condition::PositiveOrZero => Condition::Negative,
        }
    }
}

/// A portable condition on the result of comparing two doubles. The plain
/// variants are false when either operand is NaN ("unordered"); the
/// `OrUnordered` variants are true in that case.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DoubleCondition {
    Equal,
    NotEqual,
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    EqualOrUnordered,
    NotEqualOrUnordered,
    LessThanOrUnordered,
    LessThanOrEqualOrUnordered,
    GreaterThanOrUnordered,
    GreaterThanOrEqualOrUnordered,
    Ordered,
    Unordered,
}

impl DoubleCondition {
    pub const fn invert(self) -> Self {
        match self {
            DoubleCondition::Equal => DoubleCondition::NotEqualOrUnordered,
            DoubleCondition::NotEqual => DoubleCondition::EqualOrUnordered,
            DoubleCondition::LessThan => DoubleCondition::GreaterThanOrEqualOrUnordered,
            DoubleCondition::LessThanOrEqual => DoubleCondition::GreaterThanOrUnordered,
            DoubleCondition::GreaterThan => DoubleCondition::LessThanOrEqualOrUnordered,
            DoubleCondition::GreaterThanOrEqual => DoubleCondition::LessThanOrUnordered,
            DoubleCondition::EqualOrUnordered => DoubleCondition::NotEqual,
            DoubleCondition::NotEqualOrUnordered => DoubleCondition::Equal,
            DoubleCondition::LessThanOrUnordered => DoubleCondition::GreaterThanOrEqual,
            DoubleCondition::LessThanOrEqualOrUnordered => DoubleCondition::GreaterThan,
            DoubleCondition::GreaterThanOrUnordered => DoubleCondition::LessThanOrEqual,
            DoubleCondition::GreaterThanOrEqualOrUnordered => DoubleCondition::LessThan,
            DoubleCondition::Ordered => DoubleCondition::Unordered,
            DoubleCondition::Unordered => DoubleCondition::Ordered,
        }
    }

    /// Evaluates the condition in Rust, for tests and constant folding.
    pub fn evaluate(self, lhs: f64, rhs: f64) -> bool {
        let unordered = lhs.is_nan() || rhs.is_nan();
        match self {
            DoubleCondition::Equal => lhs == rhs,
            DoubleCondition::NotEqual => !unordered && lhs != rhs,
            DoubleCondition::LessThan => lhs < rhs,
            DoubleCondition::LessThanOrEqual => lhs <= rhs,
            DoubleCondition::GreaterThan => lhs > rhs,
            DoubleCondition::GreaterThanOrEqual => lhs >= rhs,
            DoubleCondition::EqualOrUnordered => unordered || lhs == rhs,
            DoubleCondition::NotEqualOrUnordered => lhs != rhs,
            DoubleCondition::LessThanOrUnordered => unordered || lhs < rhs,
            DoubleCondition::LessThanOrEqualOrUnordered => unordered || lhs <= rhs,
            DoubleCondition::GreaterThanOrUnordered => unordered || lhs > rhs,
            DoubleCondition::GreaterThanOrEqualOrUnordered => unordered || lhs >= rhs,
            DoubleCondition::Ordered => !unordered,
            DoubleCondition::Unordered => unordered,
        }
    }
}

/// Whether converting a double to an int32 accepts negative zero (as 0).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NegativeZero {
    Allow,
    Fail,
}
