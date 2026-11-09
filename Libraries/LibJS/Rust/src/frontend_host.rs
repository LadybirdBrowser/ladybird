/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions the Rust frontend (Libraries/LibJS/Rust) calls on the runtime that embeds it, through host.rs.

use libregex_rust::ast::Flags;
use libregex_rust::regex::Regex;
use num_bigint::BigInt;

use crate::runtime::big_int_algorithms;
use crate::runtime::regexp_object::parse_regex_pattern;
use crate::runtime::value::number_to_string;
use crate::runtime::value_conversions;

/// Number::toString(value), which the frontend uses for numeric property names.
pub fn number_to_utf16(value: f64) -> Vec<u16> {
    number_to_string(value).encode_utf16().collect()
}

/// Compiles a regular expression literal, so that the frontend can report an invalid one as an early error.
pub fn validate_regex(pattern: &[u16], flags: &[u16]) -> Result<(), String> {
    let has_flag = |flag: u8| flags.contains(&u16::from(flag));

    let parsed_pattern = parse_regex_pattern(pattern, has_flag(b'u'), has_flag(b'v'))
        .map_err(|error| format!("RegExp compile error: {}", error.error))?;

    let compile_flags = Flags {
        global: has_flag(b'g'),
        ignore_case: has_flag(b'i'),
        multiline: has_flag(b'm'),
        dot_all: has_flag(b's'),
        unicode: has_flag(b'u'),
        unicode_sets: has_flag(b'v'),
        sticky: has_flag(b'y'),
        has_indices: has_flag(b'd'),
    };
    // Converted the way LibRegex's rust_regex_compile converts a UTF-16 pattern, although ParsePattern only produces
    // ASCII.
    let pattern_characters = char::decode_utf16(parsed_pattern.iter().copied())
        .map(|character| character.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect();
    Regex::compile_chars(pattern_characters, compile_flags)
        .map_err(|error| format!("RegExp compile error: {error}"))?;
    Ok(())
}

/// StringToNumber(str), which the frontend uses to fold operations on string constants.
pub fn string_to_number(string: &[u16]) -> f64 {
    value_conversions::string_to_number(string)
}

/// StringToBigInt(str), which the frontend uses to fold operations on a string and a BigInt constant.
pub fn string_to_bigint(string: &[u16]) -> Option<BigInt> {
    value_conversions::string_to_bigint(string)
}

/// Number::exponentiate(base, exponent), which the frontend uses to fold `**`.
pub fn exponentiate(base: f64, exponent: f64) -> f64 {
    value_conversions::exp_double(base, exponent)
}

/// BigInt::exponentiate(base, exponent), which the frontend uses to fold `**`.
pub fn bigint_exponentiate(base: &BigInt, exponent: &BigInt) -> Option<BigInt> {
    big_int_algorithms::exponentiate(base, exponent).ok()
}

/// BigInt::leftShift(x, y), which the frontend uses to fold `<<`.
pub fn bigint_left_shift(x: &BigInt, y: &BigInt) -> Option<BigInt> {
    big_int_algorithms::left_shift(x, y).ok()
}

/// BigInt::signedRightShift(x, y), which the frontend uses to fold `>>`.
pub fn bigint_signed_right_shift(x: &BigInt, y: &BigInt) -> Option<BigInt> {
    big_int_algorithms::signed_right_shift(x, y).ok()
}
