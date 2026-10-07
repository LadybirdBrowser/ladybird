/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of AK/StringConversions.cpp that the numeric conversions build on: decimal parsing as fast_float does
//! it, and the shortest decimal form of a double as fmt's Dragonbox computes it.

/// AK::parse_number<double>(string, TrimWhitespace::No). AK parses with fast_float in its general format with a
/// leading plus allowed and no "inf" or "nan", and requires the whole string to be consumed. Both fast_float and
/// Rust's parser round correctly, so only the grammar has to be checked here.
pub fn parse_number_f64(code_units: &[u16]) -> Option<f64> {
    let result = parse_first_number_f64(code_units)?;
    (result.characters_parsed == code_units.len()).then_some(result.value)
}

/// AK::ParseFirstNumberResult<double>.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ParseFirstNumberResult {
    pub value: f64,
    pub characters_parsed: usize,
}

/// AK::parse_first_number<double>(string, TrimWhitespace::No): the longest prefix that fast_float parses, which has
/// to start the string. Out-of-range values become infinities and zeros, as AK lets them.
pub fn parse_first_number_f64(code_units: &[u16]) -> Option<ParseFirstNumberResult> {
    let is_digit_at = |index: usize| {
        code_units
            .get(index)
            .is_some_and(|code_unit| is_ascii_digit(*code_unit))
    };
    let is_at = |index: usize, character: u8| code_units.get(index) == Some(&u16::from(character));

    let mut index = 0;
    if is_at(index, b'-') || is_at(index, b'+') {
        index += 1;
    }

    let integer_start = index;
    while is_digit_at(index) {
        index += 1;
    }
    let mut digit_count = index - integer_start;

    if is_at(index, b'.') {
        index += 1;
        let fraction_start = index;
        while is_digit_at(index) {
            index += 1;
        }
        digit_count += index - fraction_start;
    }

    if digit_count == 0 {
        return None;
    }

    if is_at(index, b'e') || is_at(index, b'E') {
        let exponent_marker = index;
        index += 1;
        if is_at(index, b'-') || is_at(index, b'+') {
            index += 1;
        }
        let exponent_start = index;
        while is_digit_at(index) {
            index += 1;
        }
        // fast_float stops before an exponent marker that has no digits, which leaves the string partially consumed.
        if index == exponent_start {
            index = exponent_marker;
        }
    }

    // NB: The prefix is ASCII: a sign, digits, a point and an exponent.
    let parse = |text: &[u8]| core::str::from_utf8(text).ok()?.parse::<f64>().ok();
    let mut text = [0; 64];
    let value = if index <= text.len() {
        for (byte, &code_unit) in text.iter_mut().zip(&code_units[..index]) {
            *byte = code_unit as u8;
        }
        parse(&text[..index])
    } else {
        parse(
            &code_units[..index]
                .iter()
                .map(|&code_unit| code_unit as u8)
                .collect::<Vec<_>>(),
        )
    }?;
    Some(ParseFirstNumberResult {
        value,
        characters_parsed: index,
    })
}

fn is_ascii_digit(code_unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit)
}

const DOUBLE_MANTISSA_BITS: u32 = 52;
const DOUBLE_EXPONENT_BIAS: i32 = 1023;

/// The exact value of a finite double, significand × 2^exponent, from the fields AK::FloatExtractor exposes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BinaryDecomposition {
    pub significand: u64,
    pub exponent: i32,
}

pub fn decompose_double(number: f64) -> BinaryDecomposition {
    let bits = number.to_bits();
    let extracted_exponent = ((bits >> DOUBLE_MANTISSA_BITS) & 0x7ff) as i32;
    let extracted_mantissa = bits & ((1 << DOUBLE_MANTISSA_BITS) - 1);

    if extracted_exponent == 0 {
        BinaryDecomposition {
            significand: extracted_mantissa,
            exponent: 1 - DOUBLE_EXPONENT_BIAS - DOUBLE_MANTISSA_BITS as i32,
        }
    } else {
        BinaryDecomposition {
            significand: extracted_mantissa | (1 << DOUBLE_MANTISSA_BITS),
            exponent: extracted_exponent - DOUBLE_EXPONENT_BIAS - DOUBLE_MANTISSA_BITS as i32,
        }
    }
}

pub use ak::DecimalExponentialForm;

/// AK::convert_to_decimal_exponential_form, which runs fmt's Dragonbox: the fewest significant decimal digits that
/// round back to the value under round-to-nearest-even, and among those the candidate closest to the value, ties
/// going to the even candidate. The fraction has no trailing zeros.
pub fn convert_to_decimal_exponential_form(value: f64) -> DecimalExponentialForm {
    debug_assert!(value.is_finite());

    if value == 0.0 {
        return DecimalExponentialForm {
            sign: value.is_sign_negative(),
            fraction: 0,
            exponent: 0,
        };
    }
    ak::convert_to_decimal_exponential_form(value)
}
