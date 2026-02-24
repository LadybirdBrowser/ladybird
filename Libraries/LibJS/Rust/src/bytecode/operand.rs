/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// A bytecode register index.
///
/// Reserved registers:
/// - 0: accumulator
/// - 1: exception
/// - 2: this_value
/// - 3: return_value
/// - 4: saved_lexical_environment
/// - 5+: user registers
#[derive(Debug, Clone, Copy)]
pub struct Register(pub u32);

impl Register {
    pub const ACCUMULATOR: Register = Register(0);
    pub const EXCEPTION: Register = Register(1);
    pub const THIS_VALUE: Register = Register(2);
    pub const RETURN_VALUE: Register = Register(3);
    pub const SAVED_LEXICAL_ENVIRONMENT: Register = Register(4);
    pub const RESERVED_COUNT: u32 = 5;
}

/// A bytecode operand.
///
/// Encoded as a single `u32` with a 3-bit type tag in the top 3 bits
/// and a 29-bit index in the lower 29 bits:
///
///   `raw = (type << 29) | index`
///
/// This encoding is ABI-compatible with the VM's operand format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Operand(u32);

impl Operand {
    const TYPE_SHIFT: u32 = 29;
    const INDEX_MASK: u32 = 0x1FFF_FFFF;
    pub const INVALID: u32 = 0xFFFF_FFFF;

    pub fn register(reg: Register) -> Self {
        Self((0 << Self::TYPE_SHIFT) | reg.0)
    }

    pub fn local(index: u32) -> Self {
        Self((1 << Self::TYPE_SHIFT) | index)
    }

    pub fn constant(index: u32) -> Self {
        Self((2 << Self::TYPE_SHIFT) | index)
    }

    pub fn argument(index: u32) -> Self {
        Self((3 << Self::TYPE_SHIFT) | index)
    }

    pub fn invalid() -> Self {
        Self(Self::INVALID)
    }

    pub fn is_invalid(self) -> bool {
        self.0 == Self::INVALID
    }

    pub fn is_register(self) -> bool {
        !self.is_invalid() && self.operand_type() == OperandType::Register
    }

    pub fn is_local(self) -> bool {
        !self.is_invalid() && self.operand_type() == OperandType::Local
    }

    pub fn is_constant(self) -> bool {
        !self.is_invalid() && self.operand_type() == OperandType::Constant
    }

    pub fn operand_type(self) -> OperandType {
        assert!(
            !self.is_invalid(),
            "operand_type() called on INVALID operand"
        );
        match (self.0 >> Self::TYPE_SHIFT) & 0x7 {
            0 => OperandType::Register,
            1 => OperandType::Local,
            2 => OperandType::Constant,
            3 => OperandType::Argument,
            _ => unreachable!("operand type bits can only be 0-3"),
        }
    }

    pub fn index(self) -> u32 {
        self.0 & Self::INDEX_MASK
    }

    pub fn raw(self) -> u32 {
        self.0
    }

    /// Offset the index by the given amount, stripping the type tag and
    /// leaving a flat index into the combined
    /// [registers | locals | constants | arguments] array.
    /// Used during operand rewriting in the assembler.
    pub fn offset_index_by(&mut self, offset: u32) {
        self.0 &= Self::INDEX_MASK;
        self.0 = self.0.checked_add(offset).expect("operand index overflow");
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OperandType {
    Register,
    Local,
    Constant,
    Argument,
}

/// A bytecode label.
///
/// During compilation, holds a basic block index. After linking,
/// holds the final byte offset in the flat bytecode stream.
/// Stored as a single `u32`.
#[derive(Debug, Clone, Copy)]
pub struct Label(pub u32);

impl Label {
    pub fn basic_block_index(self) -> usize {
        self.0 as usize
    }
}

/// Index into the string table.
#[derive(Debug, Clone, Copy)]
pub struct StringTableIndex(pub u32);

impl StringTableIndex {
    pub const INVALID: u32 = 0xFFFF_FFFF;
}

/// Index into the identifier table.
#[derive(Debug, Clone, Copy)]
pub struct IdentifierTableIndex(pub u32);

impl IdentifierTableIndex {
    pub const INVALID: u32 = 0xFFFF_FFFF;
}

/// Index into the property key table.
#[derive(Debug, Clone, Copy)]
pub struct PropertyKeyTableIndex(pub u32);

/// Index into the regex table.
#[derive(Debug, Clone, Copy)]
pub struct RegexTableIndex(pub u32);

/// Environment coordinate used as a mutable cache in some instructions.
/// Layout: two `u32` fields (hops + index).
#[derive(Debug, Clone, Copy)]
pub struct EnvironmentCoordinate {
    pub hops: u32,
    pub index: u32,
}

impl EnvironmentCoordinate {
    pub const INVALID: u32 = 0xFFFF_FFFE;

    pub fn empty() -> Self {
        Self {
            hops: Self::INVALID,
            index: Self::INVALID,
        }
    }
}
