/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Why the length of the encoded instruction at some offset cannot be told.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InstructionLengthError {
    UnknownOpcode,
    TruncatedInstruction,
}

/// The last slot of a cache table, which all the sites past it share. It never remembers anything.
pub const UNCACHED_SITE: u32 = u16::MAX as u32;

/// A cache index as an instruction holds it. `u32::MAX` means none, which stays all bits set.
#[inline]
pub fn narrow_index(index: u32) -> u16 {
    if index == u32::MAX {
        return u16::MAX;
    }
    u16::try_from(index).expect("cache indices fit in 16 bits")
}

/// The name table index an instruction holds. `u32::MAX` means none, which stays all bits set.
#[inline]
pub fn name_index_bytes(index: u32) -> [u8; 3] {
    let [low, middle, high, top] = index.to_le_bytes();
    assert!(
        index == u32::MAX || (top == 0 && index != 0xff_ffff),
        "name table indices fit in 24 bits"
    );
    [low, middle, high]
}

/// The name table index of a debug name as an instruction holds it, or all bits set for none.
#[inline]
pub fn debug_name_index(index: Option<u32>) -> u16 {
    index.and_then(|index| u16::try_from(index).ok()).unwrap_or(u16::MAX)
}

#[inline]
pub fn read_u24(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], 0])
}

#[inline]
pub fn read_u16(bytes: &[u8], at: usize) -> u16 {
    u16::from_ne_bytes(bytes[at..at + 2].try_into().unwrap())
}

#[inline]
pub fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_ne_bytes(bytes[at..at + 4].try_into().unwrap())
}

#[inline]
pub fn read_u64(bytes: &[u8], at: usize) -> u64 {
    u64::from_ne_bytes(bytes[at..at + 8].try_into().unwrap())
}

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
/// The bytecode does not hold this encoding, but the frame slot that the operand ends up in.
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

    pub fn is_argument(self) -> bool {
        !self.is_invalid() && self.operand_type() == OperandType::Argument
    }

    pub fn is_constant(self) -> bool {
        !self.is_invalid() && self.operand_type() == OperandType::Constant
    }

    pub fn operand_type(self) -> OperandType {
        assert!(!self.is_invalid(), "operand_type() called on INVALID operand");
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

    pub fn from_raw(raw: u32) -> Self {
        Self(raw)
    }

    pub fn optional_from_raw(raw: u32) -> Option<Self> {
        if raw == Self::INVALID { None } else { Some(Self(raw)) }
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
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Label(pub u32);

impl Label {}

/// Index into the string table.
#[derive(Debug, Clone, Copy)]
pub struct StringTableIndex(pub u32);

impl StringTableIndex {
    pub const INVALID: u32 = 0xFFFF_FFFF;

    pub fn optional_from_raw(raw: u32) -> Option<Self> {
        if raw == Self::INVALID { None } else { Some(Self(raw)) }
    }
}

/// Index into the identifier table.
#[derive(Debug, Clone, Copy)]
pub struct IdentifierTableIndex(pub u32);

impl IdentifierTableIndex {
    pub const INVALID: u32 = 0xFFFF_FFFF;

    pub fn optional_from_raw(raw: u32) -> Option<Self> {
        if raw == Self::INVALID { None } else { Some(Self(raw)) }
    }
}

/// Index into the property key table.
#[derive(Debug, Clone, Copy)]
pub struct PropertyKeyTableIndex(pub u32);

/// Environment coordinate used by environment lookup instructions.
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
