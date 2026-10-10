/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! NaN-boxed encodings of the JS values the compiler creates itself.
//!
//! These match `GC::NanBoxedValue` and `JS::Value`.

/// How far the tag of a value is shifted up.
pub const TAG_SHIFT: u8 = 48;
/// The top 16 bits of values that are no doubles have these bits set.
pub const BASE_TAG: u16 = 0x7FF8;
const IS_CELL_BIT: u16 = 0x8000 | BASE_TAG;

/// The top 16 bits of each kind of value.
pub const BOOLEAN_TAG: u16 = 0b001 | BASE_TAG;
pub const INT32_TAG: u16 = 0b010 | BASE_TAG;
pub const EMPTY_TAG: u16 = 0b011 | BASE_TAG;
pub const UNDEFINED_TAG: u16 = 0b110 | BASE_TAG;
pub const NULL_TAG: u16 = 0b111 | BASE_TAG;
pub const OBJECT_TAG: u16 = 0b001 | IS_CELL_BIT;
pub const ACCESSOR_TAG: u16 = 0b100 | IS_CELL_BIT;
pub const STRING_TAG: u16 = 0b010 | IS_CELL_BIT;
pub const SYMBOL_TAG: u16 = 0b011 | IS_CELL_BIT;
pub const BIGINT_TAG: u16 = 0b101 | IS_CELL_BIT;

/// The one NaN that is a value; other NaNs are canonicalized to it.
pub const CANONICAL_NAN: u64 = tagged(BASE_TAG);

/// The bits of a value with tag `tag` and no payload.
pub const fn tagged(tag: u16) -> u64 {
    (tag as u64) << TAG_SHIFT
}

pub const EMPTY: u64 = tagged(EMPTY_TAG);
pub const UNDEFINED: u64 = tagged(UNDEFINED_TAG);
pub const NULL: u64 = tagged(NULL_TAG);
pub const FALSE: u64 = tagged(BOOLEAN_TAG);
pub const TRUE: u64 = tagged(BOOLEAN_TAG) | 1;

/// Stand-ins for the frame's arguments object while compiled code has not
/// created it, of the unmapped and mapped kind. Only frame states refer to
/// them: no value has these bits, and no node takes them as input.
pub const VIRTUAL_UNMAPPED_ARGUMENTS: u64 = tagged(EMPTY_TAG) | 0xa0;
pub const VIRTUAL_MAPPED_ARGUMENTS: u64 = tagged(EMPTY_TAG) | 0xa1;

/// Whether `bits` stand for an arguments object that was never created, and
/// if so whether it is a mapped one.
pub fn virtual_arguments_kind(bits: u64) -> Option<bool> {
    match bits {
        VIRTUAL_UNMAPPED_ARGUMENTS => Some(false),
        VIRTUAL_MAPPED_ARGUMENTS => Some(true),
        _ => None,
    }
}

/// The tag of a value: its top 16 bits.
pub fn tag(bits: u64) -> u16 {
    (bits >> TAG_SHIFT) as u16
}

/// The int32 a value is, if it is one.
pub fn as_int32(bits: u64) -> Option<i32> {
    (tag(bits) == INT32_TAG).then_some((bits as u32).cast_signed())
}

/// Whether a value is a double, canonical NaN included.
pub fn is_double(bits: u64) -> bool {
    bits == CANONICAL_NAN || tag(bits) & BASE_TAG != BASE_TAG
}

pub fn int32(value: i32) -> u64 {
    tagged(INT32_TAG) | u64::from(value.cast_unsigned())
}

/// The NaN-boxed value of the bits of a constant node of representation
/// `repr`: int32 constants hold their value in the low 32 bits, boolean
/// constants are 0 or 1.
pub fn boxed_constant(bits: u64, repr: crate::code::Repr) -> u64 {
    match repr {
        crate::code::Repr::Tagged => bits,
        crate::code::Repr::Int32 => int32((bits as u32).cast_signed()),
        crate::code::Repr::Bool => {
            if bits != 0 {
                TRUE
            } else {
                FALSE
            }
        }
        crate::code::Repr::Float64 => number(f64::from_bits(bits)),
        crate::code::Repr::Pointer => bits,
    }
}

/// A number boxed like `JS::Value(double)` boxes it: as an int32 if it is
/// one (but not -0), and NaN canonicalized.
pub fn number(number: f64) -> u64 {
    let integer = number as i32;
    if f64::from(integer) == number && !(integer == 0 && number.is_sign_negative()) {
        int32(integer)
    } else if number.is_nan() {
        CANONICAL_NAN
    } else {
        number.to_bits()
    }
}

/// A short human readable description of a NaN-boxed value, for IR dumps.
pub fn describe(bits: u64) -> String {
    match bits {
        EMPTY => "empty".to_string(),
        VIRTUAL_UNMAPPED_ARGUMENTS => "arguments".to_string(),
        VIRTUAL_MAPPED_ARGUMENTS => "mapped arguments".to_string(),
        UNDEFINED => "undefined".to_string(),
        NULL => "null".to_string(),
        FALSE => "false".to_string(),
        TRUE => "true".to_string(),
        _ if bits >> TAG_SHIFT == u64::from(INT32_TAG) => format!("{}", (bits as u32).cast_signed()),
        _ => format!("{bits:#018x}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn describes_known_values() {
        assert_eq!(describe(EMPTY), "empty");
        assert_eq!(describe(TRUE), "true");
        assert_eq!(describe(int32(-5)), "-5");
        assert_eq!(describe(int32(7)), "7");
        assert_eq!(describe(0x4000_0000_0000_0000), "0x4000000000000000");
    }
}
