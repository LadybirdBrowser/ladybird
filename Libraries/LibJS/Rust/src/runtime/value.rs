/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum EncodedValueKind {
    Empty,
    Boolean(bool),
    Int32(i32),
    Double(f64),
    String,
    BigInt,
    Undefined,
    Null,
    Other,
}

/// The encoded representation of a LibJS `Value`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EncodedValue(u64);

impl EncodedValue {
    const CANON_NAN_BITS: u64 = 0x7ff8_0000_0000_0000;
    const TAG_SHIFT: u64 = 48;
    const BASE_TAG: u64 = 0x7ff8;
    const IS_CELL_BIT: u64 = 0x8000 | Self::BASE_TAG;
    const STRING_TAG: u64 = 0b010 | Self::IS_CELL_BIT;
    const BIGINT_TAG: u64 = 0b101 | Self::IS_CELL_BIT;
    const UNDEFINED_TAG: u64 = 0b110 | Self::BASE_TAG;
    const NULL_TAG: u64 = 0b111 | Self::BASE_TAG;
    const BOOLEAN_TAG: u64 = 0b001 | Self::BASE_TAG;
    const INT32_TAG: u64 = 0b010 | Self::BASE_TAG;
    const EMPTY_TAG: u64 = 0b011 | Self::BASE_TAG;

    pub const fn from_encoded(encoded: u64) -> Self {
        Self(encoded)
    }

    pub const fn encoded(self) -> u64 {
        self.0
    }

    pub fn kind(self) -> EncodedValueKind {
        let tag = self.0 >> Self::TAG_SHIFT;
        if self.0 == (Self::EMPTY_TAG << Self::TAG_SHIFT) {
            EncodedValueKind::Empty
        } else if tag == Self::BOOLEAN_TAG {
            EncodedValueKind::Boolean(self.0 & 1 != 0)
        } else if tag == Self::INT32_TAG {
            EncodedValueKind::Int32((self.0 & 0xffff_ffff) as u32 as i32)
        } else if (self.0 & Self::CANON_NAN_BITS) != Self::CANON_NAN_BITS || self.0 == Self::CANON_NAN_BITS {
            EncodedValueKind::Double(f64::from_bits(self.0))
        } else if tag == Self::BIGINT_TAG {
            EncodedValueKind::BigInt
        } else if tag == Self::STRING_TAG {
            EncodedValueKind::String
        } else if self.0 == (Self::UNDEFINED_TAG << Self::TAG_SHIFT) {
            EncodedValueKind::Undefined
        } else if self.0 == (Self::NULL_TAG << Self::TAG_SHIFT) {
            EncodedValueKind::Null
        } else {
            EncodedValueKind::Other
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_immediate_values() {
        assert_eq!(
            EncodedValue::from_encoded(EncodedValue::EMPTY_TAG << EncodedValue::TAG_SHIFT).kind(),
            EncodedValueKind::Empty
        );
        assert_eq!(
            EncodedValue::from_encoded((EncodedValue::BOOLEAN_TAG << EncodedValue::TAG_SHIFT) | 1).kind(),
            EncodedValueKind::Boolean(true)
        );
        assert_eq!(
            EncodedValue::from_encoded((EncodedValue::BOOLEAN_TAG << EncodedValue::TAG_SHIFT) | 0).kind(),
            EncodedValueKind::Boolean(false)
        );
        assert_eq!(
            EncodedValue::from_encoded((EncodedValue::INT32_TAG << EncodedValue::TAG_SHIFT) | 0xffff_ffff).kind(),
            EncodedValueKind::Int32(-1)
        );
        assert!(matches!(
            EncodedValue::from_encoded(EncodedValue::CANON_NAN_BITS).kind(),
            EncodedValueKind::Double(value) if value.to_bits() == EncodedValue::CANON_NAN_BITS
        ));
        assert_eq!(
            EncodedValue::from_encoded(EncodedValue::UNDEFINED_TAG << EncodedValue::TAG_SHIFT).kind(),
            EncodedValueKind::Undefined
        );
        assert_eq!(
            EncodedValue::from_encoded(EncodedValue::NULL_TAG << EncodedValue::TAG_SHIFT).kind(),
            EncodedValueKind::Null
        );
    }
}
